#pragma once

#include <AK/SoundEngine/Common/AkCommonDefs.h>
#include <AK/SoundEngine/Common/IAkPluginMemAlloc.h>
#include "../OKConvolutionConfig.h"
#include "OKConvolutionMediaFormat.h"
#include "pffft.h"

struct IRSlotInfo
{
    AkUInt32 uSampleRate    = 0;
    AkUInt32 uNumChannels   = 0;
    AkUInt32 uNumFrames     = 0;
    AkUInt32 uNumPartitions = 0;

    bool IsLoaded() const
    {
        return uNumChannels > 0 && uNumFrames > 0 && uNumPartitions > 0;
    }
};

class ConvolutionEngine
{
public:
    static constexpr AkUInt32 kMaxIRSlots = OKConvolutionConfig::MaxIRSlots;

    ConvolutionEngine() = default;
    ~ConvolutionEngine() = default;

    // Initializes FFT plans, per-channel Overlap-Save state, and reusable scratch buffers.
    // in_uBlockSize: partition size B (pass in_pContext->GlobalContext()->GetMaxBufferLength())
    // in_uNumChannels: audio bus/voice channel count (pass in_rFormat.GetNumChannels())
    // in_uSampleRate: audio bus/voice sample rate (pass in_rFormat.uSampleRate)
    AKRESULT Init(
        AK::IAkPluginMemAlloc* in_pAllocator,
        AkUInt32 in_uBlockSize = 512,
        AkUInt32 in_uNumChannels = 2,
        AkUInt32 in_uSampleRate = 48000);

    void Term(AK::IAkPluginMemAlloc* in_pAllocator);

    // Clears per-channel Overlap-Save input history, shared FDL spectra, and dry/wet smoothing state.
    void Reset();

    // Parses a converted OKIR media blob, slices each channel into uniform B-sample partitions,
    // transforms them into the frequency domain, and stores them in slot [0 .. kMaxIRSlots - 1].
    // Passing null or zero size clears the slot.
    AKRESULT SetIRSlot(AkUInt32 in_uSlotIndex, const AkUInt8* in_pData, AkUInt32 in_uDataSize);

    // Releases the precomputed FFT partitions stored in a single IR slot.
    void ClearIRSlot(AkUInt32 in_uSlotIndex);

    // Loads or clears IR slot 0.
    AKRESULT SetIR(const AkUInt8* in_pData, AkUInt32 in_uDataSize);

    // Selects which preloaded IR slot [0 .. kMaxIRSlots - 1] is active, transitioning smoothly
    // over in_fFadeTimeMs milliseconds using a per-sample equal-power crossfade.
    // Passing in_fFadeTimeMs <= 0.0f snaps immediately with no crossfade.
    void SetActiveIR(AkUInt32 in_uSlotIndex, AkReal32 in_fFadeTimeMs);

    // Immediately snaps the active IR slot [0 .. kMaxIRSlots - 1] without crossfading.
    void SetActiveSlot(AkUInt32 in_uSlotIndex);
    AkUInt32 GetActiveSlot() const { return m_uActiveSlot; }
    bool IsCrossfading() const { return m_bIsCrossfading; }

    // Convolves audio in-place via Overlap-Save uniform partitioned convolution, applies any
    // active equal-power IR crossfade per sample, and blends dry/wet gains with linear smoothing.
    void Process(AkAudioBuffer* io_pBuffer, float in_fWetGain, float in_fDryGain);

    bool IsSlotLoaded(AkUInt32 in_uSlotIndex) const;
    IRSlotInfo GetIRSlotInfo(AkUInt32 in_uSlotIndex) const;
    AkUInt32 GetFdlCapacity() const { return m_uFdlCapacity; }

private:
    struct IRSlot
    {
        IRSlotInfo info{};

        // Precomputed FFT spectra of all uniform B-sample IR partitions for this slot.
        // Each partition is zero-padded to N = 2B ([h_p(0..B-1), 0(B..2B-1)]) before the
        // forward FFT so linear convolution holds in the saved half of the Overlap-Save IFFT.
        // Layout: [irChannel][partition][N floats in PFFFT ordered real format]
        AkReal32* pPartitions = nullptr;

        bool IsLoaded() const
        {
            return pPartitions != nullptr && info.IsLoaded();
        }
    };

    struct ChannelState
    {
        // Overlap-Save sliding-window history holding the previous B input samples.
        // Together with the incoming B samples, this forms the 2B analysis window fed
        // into the forward FFT. Because this history depends only on the input audio,
        // it is shared across all IR slots with zero per-IR overlap state.
        AkReal32* pPrevInput = nullptr;

        // Shared circular Frequency-Domain Delay Line storing the last m_uFdlCapacity
        // overlapping input block spectra: [m_uFdlCapacity * m_uFftSize].
        // Because all IR slots convolve against the same input history, only one forward
        // FFT per audio channel per block is required regardless of how many IRs are loaded.
        AkReal32* pFdl       = nullptr;

        // Next write slot in the circular FDL [0 .. m_uFdlCapacity - 1].
        AkUInt32  uFdlHead   = 0;
    };

    // Ensures each audio channel's shared FDL holds at least in_uRequiredPartitions blocks.
    // Growing preserves existing circular FDL history in chronological order.
    AKRESULT EnsureFdlCapacity(AkUInt32 in_uRequiredPartitions);

    // Convolves one audio channel's current shared FDL state against a single IR slot
    // and writes the B alias-free Overlap-Save output samples into out_pWetBlock.
    void ConvolveSlotToTime(
        const ChannelState& in_channel,
        const IRSlot& in_slot,
        AkUInt32 in_uAudioChannel,
        AkReal32* out_pWetBlock) const;

    void ProcessChannelBlock(
        ChannelState& io_channel,
        AkUInt32 in_uAudioChannel,
        AkReal32* io_pAudio,
        AkUInt32 in_uValidFrames,
        AkReal32 in_fStartDryGain,
        AkReal32 in_fDryStep,
        AkReal32 in_fStartWetGain,
        AkReal32 in_fWetStep,
        AkUInt32 in_uFadeStartSample) const;

    void FreeAllIRSlots();
    void FreeChannelStates();

    AK::IAkPluginMemAlloc* m_pAllocator = nullptr;

    AkUInt32 m_uBlockSize        = 0; // Partition size B (power of 2, >= 16)
    AkUInt32 m_uFftSize          = 0; // FFT size N = 2 * B (power of 2, >= 32 for PFFFT_REAL)
    AkUInt32 m_uNumAudioChannels = 0;
    AkUInt32 m_uSampleRate       = 48000;
    AkUInt32 m_uFdlCapacity      = 0; // Shared FDL ring depth: max uNumPartitions across loaded slots
    AkUInt32 m_uLoadedSlotCount  = 0;

    // Equal-power crossfade state machine shared across all audio channels.
    // m_uFadeSamplesElapsed advances once per Process() call after all channels finish so
    // multi-channel busses stay sample-accurate and phase-locked throughout every transition.
    AkUInt32 m_uActiveSlot         = 0;     // Target (or steady-state) IR slot
    AkUInt32 m_uOutgoingSlot       = 0;     // Outgoing IR slot fading out during a crossfade
    bool     m_bIsCrossfading      = false;
    AkUInt32 m_uFadeSamplesElapsed = 0;     // Samples elapsed in the current fade [0 .. m_uFadeTotalSamples]
    AkUInt32 m_uFadeTotalSamples   = 0;     // Total length of the current fade in samples

    // Pending target slot queued if a 3rd distinct slot is requested while already crossfading.
    // Keeping at most two active convolutions (outgoing + target) bounds peak CPU to 2x.
    bool     m_bHasPendingSlot     = false;
    AkUInt32 m_uPendingSlot        = 0;
    AkReal32 m_fPendingFadeTimeMs  = 100.0f;

    PFFFT_Setup* m_pFftSetup = nullptr;

    // Aligned scratch buffers reused across channels and blocks (zero audio-thread allocations)
    AkReal32* m_pTimeBuf  = nullptr; // Size N: Overlap-Save 2B time window & 2B IFFT time output
    AkReal32* m_pAccumFft = nullptr; // Size N: frequency-domain complex multiply-accumulate buffer
    AkReal32* m_pWorkBuf  = nullptr; // Size N: PFFFT internal scratch workspace
    AkReal32* m_pWetBufA  = nullptr; // Size B: time-domain wet block for active / outgoing IR slot
    AkReal32* m_pWetBufB  = nullptr; // Size B: time-domain wet block for target / incoming IR slot

    // Precomputed multi-slot IR bank sharing one per-channel FDL
    IRSlot m_slots[kMaxIRSlots]{};

    // Per-audio-channel Overlap-Save state
    ChannelState* m_pChannels = nullptr;

    // Dry/wet smoothing state (avoids clicks when automating DryWetMix)
    AkReal32 m_fPrevDryGain  = 0.0f;
    AkReal32 m_fPrevWetGain  = 1.0f;
    bool     m_bHasPrevGains = false;
};