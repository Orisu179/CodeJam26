#include "ConvolutionEngine.h"

#include <algorithm>
#include <cstring>

namespace
{
    // PFFFT requires N >= 32 for PFFFT_REAL; since N = 2 * B, enforce B >= 16 and power-of-2.
    AkUInt32 NextPowerOfTwoMin16(AkUInt32 v)
    {
        if (v <= 16u)
            return 16u;

        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1u;
    }

    AkReal32* AllocAlignedFloats(size_t in_count)
    {
        AkReal32* ptr = static_cast<AkReal32*>(pffft_aligned_malloc(in_count * sizeof(AkReal32)));
        if (ptr)
        {
            std::memset(ptr, 0, in_count * sizeof(AkReal32));
        }
        return ptr;
    }

    void FreeAlignedFloats(AkReal32*& io_ptr)
    {
        if (io_ptr)
        {
            pffft_aligned_free(io_ptr);
            io_ptr = nullptr;
        }
    }
}

AKRESULT ConvolutionEngine::Init(
    AK::IAkPluginMemAlloc* in_pAllocator,
    AkUInt32 in_uBlockSize,
    AkUInt32 in_uNumChannels)
{
    if (!in_pAllocator || in_uNumChannels == 0)
        return AK_InvalidParameter;

    m_pAllocator        = in_pAllocator;
    m_uBlockSize        = NextPowerOfTwoMin16(in_uBlockSize);
    m_uFftSize          = m_uBlockSize * 2u; // 2x block size avoids circular convolution aliasing
    m_uNumAudioChannels = in_uNumChannels;

    // 1. Initialize PFFFT plan for real-valued FFT of length N = 2B
    m_pFftSetup = pffft_new_setup(static_cast<int>(m_uFftSize), PFFFT_REAL);
    if (!m_pFftSetup)
        return AK_InsufficientMemory;

    // 2. Allocate aligned scratch buffers used during both SetIR() and Process()
    m_pTimeBuf  = AllocAlignedFloats(m_uFftSize);
    m_pAccumFft = AllocAlignedFloats(m_uFftSize);
    m_pWorkBuf  = AllocAlignedFloats(m_uFftSize);
    if (!m_pTimeBuf || !m_pAccumFft || !m_pWorkBuf)
    {
        Term(in_pAllocator);
        return AK_InsufficientMemory;
    }

    // 3. Allocate per-channel state structs and overlap buffers
    m_pChannels = static_cast<ChannelState*>(
        AK_PLUGIN_ALLOC(m_pAllocator, sizeof(ChannelState) * m_uNumAudioChannels)
    );
    if (!m_pChannels)
    {
        Term(in_pAllocator);
        return AK_InsufficientMemory;
    }

    for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
    {
        m_pChannels[ch] = ChannelState{};
        m_pChannels[ch].pOverlap = AllocAlignedFloats(m_uBlockSize);
        if (!m_pChannels[ch].pOverlap)
        {
            Term(in_pAllocator);
            return AK_InsufficientMemory;
        }
    }

    return AK_Success;
}

void ConvolutionEngine::Term(AK::IAkPluginMemAlloc* in_pAllocator)
{
    if (in_pAllocator)
        m_pAllocator = in_pAllocator;

    FreeIRAndPartitions();
    FreeChannelStates();

    FreeAlignedFloats(m_pTimeBuf);
    FreeAlignedFloats(m_pAccumFft);
    FreeAlignedFloats(m_pWorkBuf);

    if (m_pFftSetup)
    {
        pffft_destroy_setup(m_pFftSetup);
        m_pFftSetup = nullptr;
    }

    m_uBlockSize        = 0;
    m_uFftSize          = 0;
    m_uNumAudioChannels = 0;
    m_pAllocator        = nullptr;
}

void ConvolutionEngine::Reset()
{
    if (!m_pChannels)
        return;

    const size_t fdlBytes     = static_cast<size_t>(m_uNumPartitions) * m_uFftSize * sizeof(AkReal32);
    const size_t overlapBytes = static_cast<size_t>(m_uBlockSize) * sizeof(AkReal32);

    for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
    {
        m_pChannels[ch].uFdlHead = 0;

        if (m_pChannels[ch].pOverlap)
            std::memset(m_pChannels[ch].pOverlap, 0, overlapBytes);

        if (m_pChannels[ch].pFdl && fdlBytes > 0)
            std::memset(m_pChannels[ch].pFdl, 0, fdlBytes);
    }
}

AKRESULT ConvolutionEngine::SetIR(const AkUInt8* in_pData, AkUInt32 in_uDataSize)
{
    if (!m_pAllocator || !m_pFftSetup)
        return AK_Fail;

    // Empty/cleared media slot: unload any current IR cleanly
    if (!in_pData || in_uDataSize == 0)
    {
        FreeIRAndPartitions();
        Reset();
        return AK_Success;
    }

    if (in_uDataSize < sizeof(OKIRMediaHeader))
        return AK_InvalidFile;

    OKIRMediaHeader header{};
    std::memcpy(&header, in_pData, sizeof(OKIRMediaHeader));

    if (header.magic != kOKIRMediaMagic || header.version != kOKIRMediaVersion)
        return AK_InvalidFile;

    if (header.sampleRate == 0 || header.numChannels == 0 || header.numFrames == 0)
        return AK_InvalidFile;

    const uint64_t totalSamples          = static_cast<uint64_t>(header.numChannels) * static_cast<uint64_t>(header.numFrames);
    const uint64_t payloadBytes          = totalSamples * sizeof(AkReal32);
    const uint64_t availablePayloadBytes = static_cast<uint64_t>(in_uDataSize) - sizeof(OKIRMediaHeader);

    if (payloadBytes == 0 || payloadBytes > availablePayloadBytes)
        return AK_InvalidFile;

    AkReal32* pNewSamples = static_cast<AkReal32*>(
        AK_PLUGIN_ALLOC(m_pAllocator, static_cast<size_t>(payloadBytes))
    );
    if (!pNewSamples)
        return AK_InsufficientMemory;

    std::memcpy(pNewSamples, in_pData + sizeof(OKIRMediaHeader), static_cast<size_t>(payloadBytes));

    FreeIRAndPartitions();

    m_ir.uSampleRate  = header.sampleRate;
    m_ir.uNumChannels = header.numChannels;
    m_ir.uNumFrames   = header.numFrames;
    m_ir.pSamples     = pNewSamples;

    // Slice the parsed IR into uniform B-sample partitions and precompute their FFTs
    AKRESULT res = BuildPartitions();
    if (res != AK_Success)
    {
        FreeIRAndPartitions();
        return res;
    }

    return AK_Success;
}

AKRESULT ConvolutionEngine::BuildPartitions()
{
    if (!m_ir.IsValid() || m_uBlockSize == 0)
        return AK_Fail;

    // Number of uniform partitions needed to cover the full IR: P = ceil(L / B)
    m_uNumPartitions = (m_ir.uNumFrames + m_uBlockSize - 1u) / m_uBlockSize;

    // 1. Allocate frequency-domain storage for all IR channels & partitions
    const size_t totalIrFftFloats =
        static_cast<size_t>(m_ir.uNumChannels) * m_uNumPartitions * m_uFftSize;

    m_pIrPartitions = AllocAlignedFloats(totalIrFftFloats);
    if (!m_pIrPartitions)
        return AK_InsufficientMemory;

    // 2. Transform each IR partition into the frequency domain
    for (AkUInt32 irCh = 0; irCh < m_ir.uNumChannels; ++irCh)
    {
        const AkReal32* pIrChannel = m_ir.GetChannel(irCh);

        for (AkUInt32 p = 0; p < m_uNumPartitions; ++p)
        {
            const AkUInt32 startFrame  = p * m_uBlockSize;
            const AkUInt32 copyFrames  = (std::min)(m_uBlockSize, m_ir.uNumFrames - startFrame);

            // Place this partition's B samples in the first half; zero-pad the second half (B .. 2B-1)
            std::memset(m_pTimeBuf, 0, m_uFftSize * sizeof(AkReal32));
            std::memcpy(m_pTimeBuf, pIrChannel + startFrame, copyFrames * sizeof(AkReal32));

            AkReal32* pDestSpectrum = m_pIrPartitions
                + (static_cast<size_t>(irCh) * m_uNumPartitions + p) * m_uFftSize;

            // Ordered forward real FFT so scalar complex math can index bins directly
            pffft_transform_ordered(m_pFftSetup, m_pTimeBuf, pDestSpectrum, m_pWorkBuf, PFFFT_FORWARD);
        }
    }

    // 3. Allocate each audio channel's Frequency-Domain Delay Line (P slots of N floats)
    const size_t fdlFloatsPerChannel = static_cast<size_t>(m_uNumPartitions) * m_uFftSize;
    for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
    {
        m_pChannels[ch].pFdl = AllocAlignedFloats(fdlFloatsPerChannel);
        if (!m_pChannels[ch].pFdl)
            return AK_InsufficientMemory;
    }

    Reset();
    return AK_Success;
}

void ConvolutionEngine::Process(AkAudioBuffer* io_pBuffer, float /*in_fWetGain*/, float /*in_fDryGain*/)
{
    if (!io_pBuffer || io_pBuffer->uValidFrames == 0)
        return;

    // If no IR is loaded yet, leave the buffer untouched (bypass)
    if (!m_pIrPartitions || m_uNumPartitions == 0 || !m_pChannels)
        return;

    const AkUInt32 uChannelsToProcess = std::min(io_pBuffer->NumChannels(), m_uNumAudioChannels);
    const AkUInt32 uTotalFrames       = io_pBuffer->uValidFrames;

    for (AkUInt32 ch = 0; ch < uChannelsToProcess; ++ch)
    {
        AkReal32* pAudio = io_pBuffer->GetChannel(ch);
        if (!pAudio)
            continue;

        // Map audio channel to IR channel (e.g., stereo input with mono IR reuses IR ch 0)
        const AkUInt32 uIrCh = ch % m_ir.uNumChannels;
        const AkReal32* pIrChannelPartitions =
            m_pIrPartitions + static_cast<size_t>(uIrCh) * m_uNumPartitions * m_uFftSize;

        // Process in chunks of m_uBlockSize.
        // When m_uBlockSize matches Wwise's MaxFrames, this executes once with zero extra latency.
        AkUInt32 framesDone = 0;
        while (framesDone < uTotalFrames)
        {
            const AkUInt32 chunkFrames = std::min(m_uBlockSize, uTotalFrames - framesDone);

            ProcessChannelBlock(
                m_pChannels[ch],
                pIrChannelPartitions,
                pAudio + framesDone,
                chunkFrames);

            framesDone += chunkFrames;
        }
    }
}

void ConvolutionEngine::ProcessChannelBlock(
    ChannelState& io_channel,
    const AkReal32* in_pIrChannelPartitions,
    AkReal32* io_pAudio,
    AkUInt32 in_uValidFrames)
{
    // -------------------------------------------------------------------------
    // STEP 1: Zero-pad the incoming block of B samples to length N = 2B
    // -------------------------------------------------------------------------
    std::memset(m_pTimeBuf, 0, m_uFftSize * sizeof(AkReal32));
    std::memcpy(m_pTimeBuf, io_pAudio, in_uValidFrames * sizeof(AkReal32));

    // -------------------------------------------------------------------------
    // STEP 2: Forward FFT into the current slot of the circular FDL
    // -------------------------------------------------------------------------
    AkReal32* pCurrentInputFft =
        io_channel.pFdl + static_cast<size_t>(io_channel.uFdlHead) * m_uFftSize;

    pffft_transform_ordered(m_pFftSetup, m_pTimeBuf, pCurrentInputFft, m_pWorkBuf, PFFFT_FORWARD);

    // -------------------------------------------------------------------------
    // STEP 3: Complex Multiply-Accumulate (CMAC) across all P partitions
    //
    // Output spectrum Y = sum_{p=0}^{P-1} X[head - p] * H[p]
    //
    // PFFFT ordered PFFFT_REAL packing (N floats total):
    //   index 0       : DC (real only)
    //   index 1       : Nyquist (real only)
    //   index 2*k     : Real part of bin k      (k = 1 .. N/2 - 1)
    //   index 2*k + 1 : Imaginary part of bin k (k = 1 .. N/2 - 1)
    // -------------------------------------------------------------------------
    std::memset(m_pAccumFft, 0, m_uFftSize * sizeof(AkReal32));

    for (AkUInt32 p = 0; p < m_uNumPartitions; ++p)
    {
        // Walk backwards through the circular FDL without unsigned underflow
        const AkUInt32 fdlIdx = (io_channel.uFdlHead + m_uNumPartitions - p) % m_uNumPartitions;

        const AkReal32* X = io_channel.pFdl + static_cast<size_t>(fdlIdx) * m_uFftSize;
        const AkReal32* H = in_pIrChannelPartitions + static_cast<size_t>(p) * m_uFftSize;

        // DC and Nyquist bins are purely real
        m_pAccumFft[0] += X[0] * H[0];
        m_pAccumFft[1] += X[1] * H[1];

        // Remaining complex bins: (a + jb)(c + jd) = (ac - bd) + j(ad + bc)
        for (AkUInt32 k = 2; k < m_uFftSize; k += 2)
        {
            const AkReal32 xRe = X[k];
            const AkReal32 xIm = X[k + 1];
            const AkReal32 hRe = H[k];
            const AkReal32 hIm = H[k + 1];

            m_pAccumFft[k]     += xRe * hRe - xIm * hIm;
            m_pAccumFft[k + 1] += xRe * hIm + xIm * hRe;
        }
    }

    // -------------------------------------------------------------------------
    // STEP 4: Inverse FFT back to the time domain (length N = 2B)
    // -------------------------------------------------------------------------
    pffft_transform_ordered(m_pFftSetup, m_pAccumFft, m_pTimeBuf, m_pWorkBuf, PFFFT_BACKWARD);

    // -------------------------------------------------------------------------
    // STEP 5: Normalize (1/N) and apply Overlap-Add
    //
    // PFFFT is unscaled: IFFT(FFT(x)) = N * x.
    //   - First B samples  + previous overlap -> current audio output
    //   - Second B samples                    -> saved as next block's overlap
    // -------------------------------------------------------------------------
    const AkReal32 invN = 1.0f / static_cast<AkReal32>(m_uFftSize);

    for (AkUInt32 i = 0; i < in_uValidFrames; ++i)
    {
        io_pAudio[i] = m_pTimeBuf[i] * invN + io_channel.pOverlap[i];
    }

    for (AkUInt32 i = 0; i < m_uBlockSize; ++i)
    {
        io_channel.pOverlap[i] = m_pTimeBuf[m_uBlockSize + i] * invN;
    }

    // -------------------------------------------------------------------------
    // STEP 6: Advance FDL ring-buffer head for the next block
    // -------------------------------------------------------------------------
    io_channel.uFdlHead = (io_channel.uFdlHead + 1u) % m_uNumPartitions;
}

void ConvolutionEngine::FreeIRAndPartitions()
{
    FreeAlignedFloats(m_pIrPartitions);

    if (m_pChannels)
    {
        for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
        {
            FreeAlignedFloats(m_pChannels[ch].pFdl);
            m_pChannels[ch].uFdlHead = 0;
        }
    }

    m_uNumPartitions = 0;

    if (m_ir.pSamples && m_pAllocator)
    {
        AK_PLUGIN_FREE(m_pAllocator, m_ir.pSamples);
    }
    m_ir = IRData{};
}

void ConvolutionEngine::FreeChannelStates()
{
    if (m_pChannels)
    {
        for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
        {
            FreeAlignedFloats(m_pChannels[ch].pFdl);
            FreeAlignedFloats(m_pChannels[ch].pOverlap);
        }

        if (m_pAllocator)
        {
            AK_PLUGIN_FREE(m_pAllocator, m_pChannels);
        }
        m_pChannels = nullptr;
    }
}