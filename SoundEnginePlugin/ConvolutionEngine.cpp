#include "ConvolutionEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace
{
    constexpr double kHalfPi = 1.57079632679489661923;

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

    // Converts a crossfade time in milliseconds into an integer sample count.
    // Returns 0 for non-positive or non-finite values (signaling an immediate snap) and
    // clamps any tiny positive duration to at least 1 sample to prevent division by zero.
    AkUInt32 FadeMsToSamples(AkReal32 in_fFadeTimeMs, AkUInt32 in_uSampleRate)
    {
        if (!std::isfinite(in_fFadeTimeMs) || in_fFadeTimeMs <= 0.0f)
            return 0u;

        const double exactSamples =
            static_cast<double>(in_fFadeTimeMs) * 0.001 * static_cast<double>(in_uSampleRate);

        if (exactSamples <= 1.0)
            return 1u;

        return static_cast<AkUInt32>(std::llround(exactSamples));
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
    AkUInt32 in_uNumChannels,
    AkUInt32 in_uSampleRate)
{
    if (!in_pAllocator || in_uNumChannels == 0)
        return AK_InvalidParameter;

    m_pAllocator          = in_pAllocator;
    m_uBlockSize          = NextPowerOfTwoMin16(in_uBlockSize);
    m_uFftSize            = m_uBlockSize * 2u; // N = 2B provides a B-sample alias-free region per IFFT
    m_uNumAudioChannels   = in_uNumChannels;
    m_uSampleRate         = (in_uSampleRate > 0) ? in_uSampleRate : 48000u;
    m_uFdlCapacity        = 0;
    m_uLoadedSlotCount    = 0;
    m_uActiveSlot         = 0;
    m_uOutgoingSlot       = 0;
    m_bIsCrossfading      = false;
    m_uFadeSamplesElapsed = 0;
    m_uFadeTotalSamples   = 0;
    m_bHasPendingSlot     = false;
    m_uPendingSlot        = 0;
    m_fPendingFadeTimeMs  = 100.0f;
    m_bHasPrevGains       = false;

    // 1. Initialize PFFFT plan for real-valued FFTs of length N = 2B.
    m_pFftSetup = pffft_new_setup(static_cast<int>(m_uFftSize), PFFFT_REAL);
    if (!m_pFftSetup)
        return AK_InsufficientMemory;

    // 2. Allocate aligned scratch buffers reused across all channels and blocks.
    m_pTimeBuf  = AllocAlignedFloats(m_uFftSize);
    m_pAccumFft = AllocAlignedFloats(m_uFftSize);
    m_pWorkBuf  = AllocAlignedFloats(m_uFftSize);
    m_pWetBufA  = AllocAlignedFloats(m_uBlockSize);
    m_pWetBufB  = AllocAlignedFloats(m_uBlockSize);
    if (!m_pTimeBuf || !m_pAccumFft || !m_pWorkBuf || !m_pWetBufA || !m_pWetBufB)
    {
        Term(in_pAllocator);
        return AK_InsufficientMemory;
    }

    // 3. Allocate per-channel Overlap-Save state and previous-input history buffers.
    //    Each channel's shared FDL is allocated on demand when IR slots are loaded.
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
        m_pChannels[ch].pPrevInput = AllocAlignedFloats(m_uBlockSize);
        if (!m_pChannels[ch].pPrevInput)
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

    FreeAllIRSlots();
    FreeChannelStates();

    FreeAlignedFloats(m_pTimeBuf);
    FreeAlignedFloats(m_pAccumFft);
    FreeAlignedFloats(m_pWorkBuf);
    FreeAlignedFloats(m_pWetBufA);
    FreeAlignedFloats(m_pWetBufB);

    if (m_pFftSetup)
    {
        pffft_destroy_setup(m_pFftSetup);
        m_pFftSetup = nullptr;
    }

    m_uBlockSize          = 0;
    m_uFftSize            = 0;
    m_uNumAudioChannels   = 0;
    m_uFdlCapacity        = 0;
    m_uLoadedSlotCount    = 0;
    m_uActiveSlot         = 0;
    m_uOutgoingSlot       = 0;
    m_bIsCrossfading      = false;
    m_uFadeSamplesElapsed = 0;
    m_uFadeTotalSamples   = 0;
    m_bHasPendingSlot     = false;
    m_uPendingSlot        = 0;
    m_fPendingFadeTimeMs  = 100.0f;
    m_bHasPrevGains       = false;
    m_pAllocator          = nullptr;
}

void ConvolutionEngine::Reset()
{
    m_bHasPrevGains = false;

    // Settle any in-progress or queued crossfade directly onto the final requested slot
    // so playback after a voice/bus reset starts cleanly on the target IR.
    if (m_bHasPendingSlot)
    {
        m_uActiveSlot     = m_uPendingSlot;
        m_bHasPendingSlot = false;
    }
    m_uOutgoingSlot       = m_uActiveSlot;
    m_bIsCrossfading      = false;
    m_uFadeSamplesElapsed = 0;
    m_uFadeTotalSamples   = 0;

    if (!m_pChannels)
        return;

    const size_t fdlBytes       = static_cast<size_t>(m_uFdlCapacity) * m_uFftSize * sizeof(AkReal32);
    const size_t prevInputBytes = static_cast<size_t>(m_uBlockSize) * sizeof(AkReal32);

    for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
    {
        m_pChannels[ch].uFdlHead = 0;

        if (m_pChannels[ch].pPrevInput && prevInputBytes > 0)
            std::memset(m_pChannels[ch].pPrevInput, 0, prevInputBytes);

        if (m_pChannels[ch].pFdl && fdlBytes > 0)
            std::memset(m_pChannels[ch].pFdl, 0, fdlBytes);
    }
}

AKRESULT ConvolutionEngine::SetIRSlot(AkUInt32 in_uSlotIndex, const AkUInt8* in_pData, AkUInt32 in_uDataSize)
{
    if (!m_pAllocator || !m_pFftSetup || m_uBlockSize == 0)
        return AK_Fail;

    if (in_uSlotIndex >= kMaxIRSlots)
        return AK_InvalidParameter;

    // An empty media blob clears the requested slot cleanly.
    if (!in_pData || in_uDataSize == 0)
    {
        ClearIRSlot(in_uSlotIndex);
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

    // Number of uniform B-sample partitions covering the full IR: P = ceil(L / B).
    const AkUInt32 uNumPartitions = (header.numFrames + m_uBlockSize - 1u) / m_uBlockSize;

    // 1. Allocate aligned frequency-domain partition storage for this slot.
    //    Partitions are built directly from the media payload without retaining a duplicate
    //    time-domain copy of the IR samples in RAM.
    const size_t totalIrFftFloats =
        static_cast<size_t>(header.numChannels) * uNumPartitions * m_uFftSize;

    AkReal32* pNewPartitions = AllocAlignedFloats(totalIrFftFloats);
    if (!pNewPartitions)
        return AK_InsufficientMemory;

    // 2. Transform each IR partition into the frequency domain.
    //    For Overlap-Save with FFT length N = 2B, each partition h_p has B impulse samples in
    //    [0 .. B-1] and B trailing zeros in [B .. 2B-1]. Convolving this circularly with a
    //    2B sliding input window [x_{t-1}, x_t] confines wrap-around aliasing strictly to
    //    output indices [0 .. B-1], leaving [B .. 2B-1] as exact linear convolution.
    const AkUInt8* pPayloadBytes = in_pData + sizeof(OKIRMediaHeader);
    const size_t channelStrideBytes = static_cast<size_t>(header.numFrames) * sizeof(AkReal32);

    for (AkUInt32 irCh = 0; irCh < header.numChannels; ++irCh)
    {
        const AkUInt8* pIrChannelBytes = pPayloadBytes + static_cast<size_t>(irCh) * channelStrideBytes;

        for (AkUInt32 p = 0; p < uNumPartitions; ++p)
        {
            const AkUInt32 startFrame = p * m_uBlockSize;
            const AkUInt32 copyFrames = (std::min)(m_uBlockSize, header.numFrames - startFrame);

            std::memset(m_pTimeBuf, 0, m_uFftSize * sizeof(AkReal32));
            std::memcpy(
                m_pTimeBuf,
                pIrChannelBytes + static_cast<size_t>(startFrame) * sizeof(AkReal32),
                static_cast<size_t>(copyFrames) * sizeof(AkReal32));

            AkReal32* pDestSpectrum = pNewPartitions
                + (static_cast<size_t>(irCh) * uNumPartitions + p) * m_uFftSize;

            // Ordered forward real FFT so scalar complex math can index frequency bins directly.
            pffft_transform_ordered(m_pFftSetup, m_pTimeBuf, pDestSpectrum, m_pWorkBuf, PFFFT_FORWARD);
        }
    }

    // 3. Grow each audio channel's shared FDL if this IR requires more partitions than
    //    any previously loaded slot. Existing FDL history is preserved chronologically.
    const AKRESULT fdlRes = EnsureFdlCapacity(uNumPartitions);
    if (fdlRes != AK_Success)
    {
        FreeAlignedFloats(pNewPartitions);
        return fdlRes;
    }

    // 4. Commit the new precomputed partitions into the slot.
    IRSlot& slot = m_slots[in_uSlotIndex];
    const bool bWasLoaded = slot.IsLoaded();

    FreeAlignedFloats(slot.pPartitions);
    slot.info.uSampleRate    = header.sampleRate;
    slot.info.uNumChannels   = header.numChannels;
    slot.info.uNumFrames     = header.numFrames;
    slot.info.uNumPartitions = uNumPartitions;
    slot.pPartitions         = pNewPartitions;

    if (!bWasLoaded)
    {
        ++m_uLoadedSlotCount;
    }

    return AK_Success;
}

void ConvolutionEngine::ClearIRSlot(AkUInt32 in_uSlotIndex)
{
    if (in_uSlotIndex >= kMaxIRSlots)
        return;

    IRSlot& slot = m_slots[in_uSlotIndex];
    if (slot.IsLoaded() && m_uLoadedSlotCount > 0)
    {
        --m_uLoadedSlotCount;
    }

    FreeAlignedFloats(slot.pPartitions);
    slot.info = IRSlotInfo{};

    // If all IR slots are now empty, clear accumulated audio history so a future load starts clean.
    if (m_uLoadedSlotCount == 0)
    {
        Reset();
    }
}

AKRESULT ConvolutionEngine::SetIR(const AkUInt8* in_pData, AkUInt32 in_uDataSize)
{
    return SetIRSlot(0u, in_pData, in_uDataSize);
}

void ConvolutionEngine::SetActiveIR(AkUInt32 in_uSlotIndex, AkReal32 in_fFadeTimeMs)
{
    const AkUInt32 uTargetSlot  = (std::min)(in_uSlotIndex, kMaxIRSlots - 1u);
    const AkUInt32 uFadeSamples = FadeMsToSamples(in_fFadeTimeMs, m_uSampleRate);

    // Snap immediately when fade time is zero/non-positive or when no IR is loaded yet
    // (the engine is transparently bypassed while empty, so no fade timer is running).
    if (uFadeSamples == 0u || m_uLoadedSlotCount == 0u)
    {
        m_uActiveSlot         = uTargetSlot;
        m_uOutgoingSlot       = uTargetSlot;
        m_bIsCrossfading      = false;
        m_uFadeSamplesElapsed = 0;
        m_uFadeTotalSamples   = 0;
        m_bHasPendingSlot     = false;
        m_uPendingSlot        = uTargetSlot;
        return;
    }

    // Steady state: start a new equal-power crossfade if the requested slot differs from active.
    if (!m_bIsCrossfading)
    {
        if (uTargetSlot == m_uActiveSlot)
            return;

        m_uOutgoingSlot       = m_uActiveSlot;
        m_uActiveSlot         = uTargetSlot;
        m_uFadeSamplesElapsed = 0;
        m_uFadeTotalSamples   = uFadeSamples;
        m_bIsCrossfading      = true;
        m_bHasPendingSlot     = false;
        return;
    }

    // Mid-fade retargeting:
    // Case A: Target matches the slot we are already fading into. Cancel any queued 3rd slot
    //         and let the current transition finish naturally.
    if (uTargetSlot == m_uActiveSlot)
    {
        m_bHasPendingSlot = false;
        return;
    }

    // Case B: Target reverses back to the outgoing slot.
    //         Invert normalized progress (alphaNew = 1 - alphaOld) and swap outgoing/active:
    //           cos(alphaNew * pi/2) = sin(alphaOld * pi/2)
    //           sin(alphaNew * pi/2) = cos(alphaOld * pi/2)
    //         Both IR gains match continuously at the reversal boundary with zero click.
    if (uTargetSlot == m_uOutgoingSlot)
    {
        const double alphaOld = (m_uFadeTotalSamples > 0u)
            ? std::clamp(static_cast<double>(m_uFadeSamplesElapsed) / static_cast<double>(m_uFadeTotalSamples), 0.0, 1.0)
            : 1.0;
        const double alphaNew = 1.0 - alphaOld;

        std::swap(m_uOutgoingSlot, m_uActiveSlot);
        m_uFadeTotalSamples   = uFadeSamples;
        m_uFadeSamplesElapsed = static_cast<AkUInt32>(std::llround(alphaNew * static_cast<double>(uFadeSamples)));
        m_bHasPendingSlot     = false;

        if (m_uFadeSamplesElapsed >= m_uFadeTotalSamples)
        {
            m_uOutgoingSlot       = m_uActiveSlot;
            m_bIsCrossfading      = false;
            m_uFadeSamplesElapsed = 0;
            m_uFadeTotalSamples   = 0;
        }
        return;
    }

    // Case C: Target is a 3rd distinct slot while a fade is already running.
    //         Queue it as the pending target (overwriting any earlier unstarted target if the
    //         caller scrubs rapidly across slots). This caps peak convolution CPU to at most
    //         two active IR evaluations per block while transitioning smoothly on completion.
    m_bHasPendingSlot    = true;
    m_uPendingSlot       = uTargetSlot;
    m_fPendingFadeTimeMs = in_fFadeTimeMs;
}

void ConvolutionEngine::SetActiveSlot(AkUInt32 in_uSlotIndex)
{
    SetActiveIR(in_uSlotIndex, 0.0f);
}

bool ConvolutionEngine::IsSlotLoaded(AkUInt32 in_uSlotIndex) const
{
    return (in_uSlotIndex < kMaxIRSlots) && m_slots[in_uSlotIndex].IsLoaded();
}

IRSlotInfo ConvolutionEngine::GetIRSlotInfo(AkUInt32 in_uSlotIndex) const
{
    if (in_uSlotIndex >= kMaxIRSlots)
        return IRSlotInfo{};
    return m_slots[in_uSlotIndex].info;
}

AKRESULT ConvolutionEngine::EnsureFdlCapacity(AkUInt32 in_uRequiredPartitions)
{
    if (in_uRequiredPartitions <= m_uFdlCapacity)
        return AK_Success;

    if (!m_pChannels || !m_pAllocator || m_uFftSize == 0)
        return AK_Fail;

    // Allocate all new channel FDL buffers before modifying any channel state so an
    // out-of-memory failure leaves the existing FDL intact.
    AkReal32** ppNewFdls = static_cast<AkReal32**>(
        AK_PLUGIN_ALLOC(m_pAllocator, sizeof(AkReal32*) * m_uNumAudioChannels)
    );
    if (!ppNewFdls)
        return AK_InsufficientMemory;

    std::memset(ppNewFdls, 0, sizeof(AkReal32*) * m_uNumAudioChannels);

    const size_t newFdlFloats = static_cast<size_t>(in_uRequiredPartitions) * m_uFftSize;
    for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
    {
        ppNewFdls[ch] = AllocAlignedFloats(newFdlFloats);
        if (!ppNewFdls[ch])
        {
            for (AkUInt32 i = 0; i < ch; ++i)
            {
                FreeAlignedFloats(ppNewFdls[i]);
            }
            AK_PLUGIN_FREE(m_pAllocator, ppNewFdls);
            return AK_InsufficientMemory;
        }
    }

    // Unroll each channel's existing circular FDL in chronological order into [0 .. oldCap - 1]
    // of the larger buffer:
    //   - Between blocks, uFdlHead points to the oldest entry (next write position).
    //   - Copying entries starting at oldHead places the oldest block at index 0 and the most
    //     recent block at index (oldCap - 1).
    //   - Setting the new head to oldCap ensures the next incoming block is written at oldCap,
    //     so walking backwards by p = 1..oldCap reads the exact previous blocks in order and
    //     any deeper partitions beyond oldCap read zero-initialized silence.
    const size_t spectrumBytes = static_cast<size_t>(m_uFftSize) * sizeof(AkReal32);
    for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
    {
        ChannelState& channel = m_pChannels[ch];
        if (channel.pFdl && m_uFdlCapacity > 0)
        {
            const AkUInt32 uOldCap  = m_uFdlCapacity;
            const AkUInt32 uOldHead = channel.uFdlHead % uOldCap;

            for (AkUInt32 i = 0; i < uOldCap; ++i)
            {
                const AkUInt32 uSrcIdx = (uOldHead + i) % uOldCap;
                std::memcpy(
                    ppNewFdls[ch] + static_cast<size_t>(i) * m_uFftSize,
                    channel.pFdl + static_cast<size_t>(uSrcIdx) * m_uFftSize,
                    spectrumBytes);
            }

            FreeAlignedFloats(channel.pFdl);
            channel.uFdlHead = uOldCap;
        }
        else
        {
            channel.uFdlHead = 0;
        }

        channel.pFdl = ppNewFdls[ch];
    }

    AK_PLUGIN_FREE(m_pAllocator, ppNewFdls);
    m_uFdlCapacity = in_uRequiredPartitions;
    return AK_Success;
}

void ConvolutionEngine::Process(AkAudioBuffer* io_pBuffer, float in_fWetGain, float in_fDryGain)
{
    if (!io_pBuffer || io_pBuffer->uValidFrames == 0)
        return;

    // If no IR slot is loaded, leave the audio buffer untouched (transparent bypass).
    if (m_uLoadedSlotCount == 0 || m_uFdlCapacity == 0 || !m_pChannels)
        return;

    const AkUInt32 uChannelsToProcess = (std::min)(io_pBuffer->NumChannels(), m_uNumAudioChannels);
    const AkUInt32 uTotalFrames       = io_pBuffer->uValidFrames;

    // Snap gains on the first block after Init()/Reset() so playback does not fade in.
    if (!m_bHasPrevGains)
    {
        m_fPrevDryGain  = in_fDryGain;
        m_fPrevWetGain  = in_fWetGain;
        m_bHasPrevGains = true;
    }

    // Per-sample linear ramp increments shared across all channels for this buffer.
    const AkReal32 fInvFrames = 1.0f / static_cast<AkReal32>(uTotalFrames);
    const AkReal32 fDryStep   = (in_fDryGain - m_fPrevDryGain) * fInvFrames;
    const AkReal32 fWetStep   = (in_fWetGain - m_fPrevWetGain) * fInvFrames;

    // Snapshot the crossfade sample position before looping over channels so every channel
    // (L/R, 5.1, etc.) evaluates the exact same equal-power envelope across this buffer.
    const AkUInt32 uBufferFadeStart = m_uFadeSamplesElapsed;

    for (AkUInt32 ch = 0; ch < uChannelsToProcess; ++ch)
    {
        AkReal32* pAudio = io_pBuffer->GetChannel(ch);
        if (!pAudio)
            continue;

        // Process in chunks of partition size B (m_uBlockSize).
        // When m_uBlockSize matches Wwise's MaxFrames, this executes once with zero extra latency.
        AkUInt32 framesDone = 0;
        while (framesDone < uTotalFrames)
        {
            const AkUInt32 chunkFrames = (std::min)(m_uBlockSize, uTotalFrames - framesDone);

            const AkReal32 fChunkDryStart = m_fPrevDryGain + fDryStep * static_cast<AkReal32>(framesDone);
            const AkReal32 fChunkWetStart = m_fPrevWetGain + fWetStep * static_cast<AkReal32>(framesDone);

            ProcessChannelBlock(
                m_pChannels[ch],
                ch,
                pAudio + framesDone,
                chunkFrames,
                fChunkDryStart,
                fDryStep,
                fChunkWetStart,
                fWetStep,
                uBufferFadeStart + framesDone);

            framesDone += chunkFrames;
        }
    }

    // Advance the crossfade clock once per buffer after all channels complete.
    if (m_bIsCrossfading)
    {
        const AkUInt32 uRemaining =
            (m_uFadeSamplesElapsed < m_uFadeTotalSamples)
                ? (m_uFadeTotalSamples - m_uFadeSamplesElapsed)
                : 0u;

        if (uTotalFrames >= uRemaining)
        {
            // Crossfade completed during (or at the end of) this buffer: settle on active slot.
            m_bIsCrossfading      = false;
            m_uOutgoingSlot       = m_uActiveSlot;
            m_uFadeSamplesElapsed = 0;
            m_uFadeTotalSamples   = 0;

            // If a 3rd slot was requested mid-fade, begin transitioning to it on the next buffer.
            if (m_bHasPendingSlot)
            {
                const AkUInt32 uNextSlot   = m_uPendingSlot;
                const AkReal32 fNextFadeMs = m_fPendingFadeTimeMs;
                m_bHasPendingSlot          = false;
                SetActiveIR(uNextSlot, fNextFadeMs);
            }
        }
        else
        {
            m_uFadeSamplesElapsed += uTotalFrames;
        }
    }

    // Store target gains as the starting point for the next buffer's linear ramp.
    m_fPrevDryGain = in_fDryGain;
    m_fPrevWetGain = in_fWetGain;
}

void ConvolutionEngine::ConvolveSlotToTime(
    const ChannelState& in_channel,
    const IRSlot& in_slot,
    AkUInt32 in_uAudioChannel,
    AkReal32* out_pWetBlock) const
{
    // An unloaded slot produces silence while allowing the shared FDL to keep recording input.
    if (!in_slot.IsLoaded())
    {
        std::memset(out_pWetBlock, 0, static_cast<size_t>(m_uBlockSize) * sizeof(AkReal32));
        return;
    }

    // Map audio channel to IR channel (e.g., stereo audio with a mono IR reuses IR channel 0).
    const AkUInt32 uIrCh          = in_uAudioChannel % in_slot.info.uNumChannels;
    const AkUInt32 uNumPartitions = in_slot.info.uNumPartitions;
    const AkReal32* pIrChannelPartitions =
        in_slot.pPartitions + static_cast<size_t>(uIrCh) * uNumPartitions * m_uFftSize;

    // Frequency-domain Complex Multiply-Accumulate (CMAC) across this slot's P partitions:
    //
    //   Y[k] = sum_{p=0}^{P-1} U[head - p][k] * H_p[k]
    //
    // Each FDL entry U[head - p] is the 2B FFT of the overlapping input window [x_{t-p-1}, x_{t-p}],
    // indexed modulo m_uFdlCapacity (the shared ring capacity across all loaded slots).
    //
    // PFFFT ordered PFFFT_REAL packing (N = 2B floats total):
    //   index 0       : DC (real only)
    //   index 1       : Nyquist (real only)
    //   index 2*k     : Real part of bin k      (k = 1 .. N/2 - 1)
    //   index 2*k + 1 : Imaginary part of bin k (k = 1 .. N/2 - 1)
    std::memset(m_pAccumFft, 0, static_cast<size_t>(m_uFftSize) * sizeof(AkReal32));

    for (AkUInt32 p = 0; p < uNumPartitions; ++p)
    {
        // Walk backwards through the shared circular FDL without unsigned underflow.
        const AkUInt32 fdlIdx = (in_channel.uFdlHead + m_uFdlCapacity - p) % m_uFdlCapacity;

        const AkReal32* X = in_channel.pFdl + static_cast<size_t>(fdlIdx) * m_uFftSize;
        const AkReal32* H = pIrChannelPartitions + static_cast<size_t>(p) * m_uFftSize;

        // DC and Nyquist bins are purely real.
        m_pAccumFft[0] += X[0] * H[0];
        m_pAccumFft[1] += X[1] * H[1];

        // Complex bins: (a + jb)(c + jd) = (ac - bd) + j(ad + bc)
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

    // Inverse FFT back to the time domain (length N = 2B, unnormalized by factor N).
    pffft_transform_ordered(m_pFftSetup, m_pAccumFft, m_pTimeBuf, m_pWorkBuf, PFFFT_BACKWARD);

    // Overlap-Save extraction:
    //   - First half  m_pTimeBuf[0 .. B-1]  : circular wrap-around (time-aliased) samples -> discarded.
    //   - Second half m_pTimeBuf[B .. 2B-1] : exact, alias-free linear convolution for block t -> saved.
    // Scale by 1/N because PFFFT's forward + backward transform scales by N.
    // Because no output overlap buffer is carried across blocks, any IR slot produces exact
    // output immediately on the very first block it is evaluated.
    const AkReal32 invN = 1.0f / static_cast<AkReal32>(m_uFftSize);
    for (AkUInt32 i = 0; i < m_uBlockSize; ++i)
    {
        out_pWetBlock[i] = m_pTimeBuf[m_uBlockSize + i] * invN;
    }
}

void ConvolutionEngine::ProcessChannelBlock(
    ChannelState& io_channel,
    AkUInt32 in_uAudioChannel,
    AkReal32* io_pAudio,
    AkUInt32 in_uValidFrames,
    AkReal32 in_fStartDryGain,
    AkReal32 in_fDryStep,
    AkReal32 in_fStartWetGain,
    AkReal32 in_fWetStep,
    AkUInt32 in_uFadeStartSample) const
{
    // STEP 1 (Overlap-Save sliding input window):
    // Assemble the 2B time-domain analysis window in m_pTimeBuf:
    //   [0 .. B-1]  = previous block's B samples (io_channel.pPrevInput)
    //   [B .. 2B-1] = current block's incoming samples (zero-padded if in_uValidFrames < B)
    //
    // Having the preceding B samples in the first half supplies all past samples needed for
    // each partition's circular convolution to equal linear convolution on indices [B .. 2B-1].
    std::memcpy(m_pTimeBuf, io_channel.pPrevInput, static_cast<size_t>(m_uBlockSize) * sizeof(AkReal32));
    std::memcpy(m_pTimeBuf + m_uBlockSize, io_pAudio, static_cast<size_t>(in_uValidFrames) * sizeof(AkReal32));

    if (in_uValidFrames < m_uBlockSize)
    {
        std::memset(
            m_pTimeBuf + m_uBlockSize + in_uValidFrames,
            0,
            static_cast<size_t>(m_uBlockSize - in_uValidFrames) * sizeof(AkReal32));
    }

    // Store the second half [B .. 2B-1] as pPrevInput for the next block so the sliding
    // window advances by exactly B samples in lockstep with the FDL stride.
    std::memcpy(
        io_channel.pPrevInput,
        m_pTimeBuf + m_uBlockSize,
        static_cast<size_t>(m_uBlockSize) * sizeof(AkReal32));

    // STEP 2 (Shared forward FFT into circular FDL):
    // Transform the 2B sliding input window once and store its spectrum at uFdlHead.
    // All IR slots read from this shared FDL without repeating the forward FFT.
    AkReal32* pCurrentInputFft =
        io_channel.pFdl + static_cast<size_t>(io_channel.uFdlHead) * m_uFftSize;

    pffft_transform_ordered(m_pFftSetup, m_pTimeBuf, pCurrentInputFft, m_pWorkBuf, PFFFT_FORWARD);

    // STEP 3 (Partitioned CMAC + IFFT + Overlap-Save extraction & Dry/Wet blend):
    // Determine whether this chunk falls inside an active crossfade window.
    const bool bChunkCrossfading =
        m_bIsCrossfading && (in_uFadeStartSample < m_uFadeTotalSamples);

    AkReal32 fDryGain = in_fStartDryGain;
    AkReal32 fWetGain = in_fStartWetGain;

    if (!bChunkCrossfading)
    {
        // Steady state: evaluate only the active IR slot (1x CMAC + 1x IFFT).
        ConvolveSlotToTime(io_channel, m_slots[m_uActiveSlot], in_uAudioChannel, m_pWetBufA);

        for (AkUInt32 i = 0; i < in_uValidFrames; ++i)
        {
            const AkReal32 fDrySample = io_pAudio[i];
            const AkReal32 fWetSample = m_pWetBufA[i];

            io_pAudio[i] = fDrySample * fDryGain + fWetSample * fWetGain;

            fDryGain += in_fDryStep;
            fWetGain += in_fWetStep;
        }
    }
    else
    {
        // Active crossfade: evaluate both the outgoing IR (into m_pWetBufA) and the target IR
        // (into m_pWetBufB) against the exact same shared FDL history.
        ConvolveSlotToTime(io_channel, m_slots[m_uOutgoingSlot], in_uAudioChannel, m_pWetBufA);
        ConvolveSlotToTime(io_channel, m_slots[m_uActiveSlot],   in_uAudioChannel, m_pWetBufB);

        // Per-sample equal-power crossfade curve:
        //   gOut(n) = cos(theta(n)),  gIn(n) = sin(theta(n)),  gOut^2 + gIn^2 = 1
        //   theta(n) = (pi / 2) * ((n + 0.5) / totalFadeSamples)
        //
        // Equal-power weighting maintains constant perceived broadband loudness across
        // uncorrelated IR tails (avoiding the -3 dB midpoint dip of a linear fade).
        // Anchoring cos/sin once per chunk and stepping via a unit rotation matrix
        // (cosStep, sinStep) avoids trigonometric calls inside the sample loop while
        // keeping floating-point drift negligible in double precision.
        const double dTheta  = kHalfPi / static_cast<double>(m_uFadeTotalSamples);
        const double theta0  = (static_cast<double>(in_uFadeStartSample) + 0.5) * dTheta;
        const double cosStep = std::cos(dTheta);
        const double sinStep = std::sin(dTheta);

        double gOut = std::cos(theta0);
        double gIn  = std::sin(theta0);

        const AkUInt32 uRemainingFade = m_uFadeTotalSamples - in_uFadeStartSample;
        const AkUInt32 uFadeFrames    = (std::min)(in_uValidFrames, uRemainingFade);

        // Blend outgoing (A) and target (B) wet signals across the remaining fade samples.
        for (AkUInt32 i = 0; i < uFadeFrames; ++i)
        {
            const AkReal32 fDrySample = io_pAudio[i];
            const AkReal32 fWetSample =
                m_pWetBufA[i] * static_cast<AkReal32>(gOut) +
                m_pWetBufB[i] * static_cast<AkReal32>(gIn);

            io_pAudio[i] = fDrySample * fDryGain + fWetSample * fWetGain;

            fDryGain += in_fDryStep;
            fWetGain += in_fWetStep;

            // Rotate unit phasor (gOut, gIn) forward by dTheta:
            //   cos(theta + dTheta) = cos(theta)*cos(dTheta) - sin(theta)*sin(dTheta)
            //   sin(theta + dTheta) = sin(theta)*cos(dTheta) + cos(theta)*sin(dTheta)
            const double nextOut = gOut * cosStep - gIn * sinStep;
            const double nextIn  = gIn * cosStep + gOut * sinStep;
            gOut = nextOut;
            gIn  = nextIn;
        }

        // If the crossfade completes mid-block, remaining samples pass the target IR (B) at unity.
        for (AkUInt32 i = uFadeFrames; i < in_uValidFrames; ++i)
        {
            const AkReal32 fDrySample = io_pAudio[i];
            const AkReal32 fWetSample = m_pWetBufB[i];

            io_pAudio[i] = fDrySample * fDryGain + fWetSample * fWetGain;

            fDryGain += in_fDryStep;
            fWetGain += in_fWetStep;
        }
    }

    // STEP 4 (Advance shared FDL ring-buffer head for the next block):
    io_channel.uFdlHead = (io_channel.uFdlHead + 1u) % m_uFdlCapacity;
}

void ConvolutionEngine::FreeAllIRSlots()
{
    for (AkUInt32 slot = 0; slot < kMaxIRSlots; ++slot)
    {
        FreeAlignedFloats(m_slots[slot].pPartitions);
        m_slots[slot].info = IRSlotInfo{};
    }

    m_uLoadedSlotCount = 0;
}

void ConvolutionEngine::FreeChannelStates()
{
    if (m_pChannels)
    {
        for (AkUInt32 ch = 0; ch < m_uNumAudioChannels; ++ch)
        {
            FreeAlignedFloats(m_pChannels[ch].pFdl);
            FreeAlignedFloats(m_pChannels[ch].pPrevInput);
        }

        if (m_pAllocator)
        {
            AK_PLUGIN_FREE(m_pAllocator, m_pChannels);
        }
        m_pChannels = nullptr;
    }

    m_uFdlCapacity = 0;
}