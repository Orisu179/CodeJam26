#pragma once

#include <AK/SoundEngine/Common/AkCommonDefs.h>
#include <AK/SoundEngine/Common/IAkPluginMemAlloc.h>
#include <AK/Tools/Common/AkArray.h>
#include "OKConvolutionMediaFormat.h"
#include "pffft.h"

struct IRData
{
    AkUInt32 uSampleRate  = 0;
    AkUInt32 uNumChannels = 0;
    AkUInt32 uNumFrames   = 0;
    AkReal32* pSamples    = nullptr; // Planar: channel c starts at pSamples + c * uNumFrames

    const AkReal32* GetChannel(AkUInt32 in_uChannel) const
    {
        if (!pSamples || in_uChannel >= uNumChannels)
            return nullptr;
        return pSamples + (static_cast<size_t>(in_uChannel) * uNumFrames);
    }

    bool IsValid() const
    {
        return pSamples != nullptr && uNumChannels > 0 && uNumFrames > 0;
    }
};

class ConvolutionEngine
{
public:
    ConvolutionEngine() = default;
    ~ConvolutionEngine() = default;

    // in_uBlockSize: partition size B (pass in_pContext->GetMaxBufferLength() for zero extra latency)
    // in_uNumChannels: audio bus/voice channel count (pass in_rFormat.GetNumChannels())
    AKRESULT Init(
        AK::IAkPluginMemAlloc* in_pAllocator,
        AkUInt32 in_uBlockSize = 512,
        AkUInt32 in_uNumChannels = 2);

    void Term(AK::IAkPluginMemAlloc* in_pAllocator);

    // Clears internal delay lines and overlap buffers (e.g., when voice/bus restarts)
    void Reset();

    // Parses a single converted IR blob and precomputes its FFT partitions
    AKRESULT SetIR(const AkUInt8* in_pData, AkUInt32 in_uDataSize);

    // Dry/wet gains are ignored for now; outputs 100% wet convolved audio
    void Process(AkAudioBuffer* io_pBuffer, float in_fWetGain, float in_fDryGain);

    const IRData& GetIR() const { return m_ir; }

private:
    struct ChannelState
    {
        AkReal32* pFdl     = nullptr; // Frequency-Domain Delay Line: [m_uNumPartitions * m_uFftSize]
        AkReal32* pOverlap = nullptr; // Saved overlap-add tail from previous block: [m_uBlockSize]
        AkUInt32  uFdlHead = 0;       // Current write slot in the circular FDL [0 .. m_uNumPartitions - 1]
    };

    AKRESULT BuildPartitions();
    void ProcessChannelBlock(
        ChannelState& io_channel,
        const AkReal32* in_pIrChannelPartitions,
        AkReal32* io_pAudio,
        AkUInt32 in_uValidFrames);

    void FreeIRAndPartitions();
    void FreeChannelStates();

    AK::IAkPluginMemAlloc* m_pAllocator = nullptr;

    AkUInt32 m_uBlockSize        = 0; // Partition size B (power of 2, >= 16)
    AkUInt32 m_uFftSize          = 0; // FFT size N = 2 * B (power of 2, >= 32 for PFFFT_REAL)
    AkUInt32 m_uNumAudioChannels = 0;
    AkUInt32 m_uNumPartitions    = 0; // P = ceil(IR_length / B)

    PFFFT_Setup* m_pFftSetup = nullptr;

    // Aligned scratch buffers reused across channels/blocks (no audio-thread allocations)
    AkReal32* m_pTimeBuf  = nullptr; // Size N: zero-padded time input & IFFT time output
    AkReal32* m_pAccumFft = nullptr; // Size N: frequency-domain complex accumulator
    AkReal32* m_pWorkBuf  = nullptr; // Size N: PFFFT internal scratch workspace

    // Precomputed FFT spectra of all IR partitions:
    // Layout: [irChannel][partition][N floats in PFFFT ordered real format]
    AkReal32* m_pIrPartitions = nullptr;

    // Per-audio-channel convolution state
    ChannelState* m_pChannels = nullptr;

    IRData m_ir;
};