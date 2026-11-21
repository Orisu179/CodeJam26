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

    // in_uBlockSize: partition size B (pass in_pContext->GlobalContext()->GetMaxBufferLength())
    // in_uNumChannels: audio bus/voice channel count (pass in_rFormat.GetNumChannels())
    // in_uSampleRate: audio bus/voice sample rate (pass in_rFormat.uSampleRate)
    AKRESULT Init(
        AK::IAkPluginMemAlloc* in_pAllocator,
        AkUInt32 in_uBlockSize = 512,
        AkUInt32 in_uNumChannels = 2,
        AkUInt32 in_uSampleRate = 48000);

    void Term(AK::IAkPluginMemAlloc* in_pAllocator);

    // Clears internal delay lines, overlap buffers, and limiter envelope
    void Reset();

    // Parses a single converted IR blob and precomputes its FFT partitions
    AKRESULT SetIR(const AkUInt8* in_pData, AkUInt32 in_uDataSize);

    // Convolves audio in-place, blends dry/wet gains with per-block smoothing, and limits output
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
        AkUInt32 in_uValidFrames,
        AkReal32 in_fStartDryGain,
        AkReal32 in_fDryStep,
        AkReal32 in_fStartWetGain,
        AkReal32 in_fWetStep) const;


    void FreeIRAndPartitions();
    void FreeChannelStates();

    AK::IAkPluginMemAlloc* m_pAllocator = nullptr;

    AkUInt32 m_uBlockSize        = 0; // Partition size B (power of 2, >= 16)
    AkUInt32 m_uFftSize          = 0; // FFT size N = 2 * B (power of 2, >= 32 for PFFFT_REAL)
    AkUInt32 m_uNumAudioChannels = 0;
    AkUInt32 m_uSampleRate       = 48000;
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

    // Dry/wet smoothing state (avoids clicks when automating DryWetMix)
    AkReal32 m_fPrevDryGain = 0.0f;
    AkReal32 m_fPrevWetGain = 1.0f;
    bool     m_bHasPrevGains = false;

    IRData m_ir;
};