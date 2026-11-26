/*******************************************************************************
The content of this file includes portions of the AUDIOKINETIC Wwise Technology
released in source code form as part of the SDK installer package.

Commercial License Usage

Licensees holding valid commercial licenses to the AUDIOKINETIC Wwise Technology
may use this file in accordance with the end user license agreement provided
with the software or, alternatively, in accordance with the terms contained in a
written agreement between you and Audiokinetic Inc.

Apache License Usage

Alternatively, this file may be used under the Apache License, Version 2.0 (the
"Apache License"); you may not use this file except in compliance with the
Apache License. You may obtain a copy of the Apache License at
http://www.apache.org/licenses/LICENSE-2.0.

Unless required by applicable law or agreed to in writing, software distributed
under the Apache License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES
OR CONDITIONS OF ANY KIND, either express or implied. See the Apache License for
the specific language governing permissions and limitations under the License.

  Copyright (c) 2026 Audiokinetic Inc.
*******************************************************************************/

#include "OKConvolutionFX.h"
#include <algorithm>
#include "../OKConvolutionConfig.h"

#include <AK/AkWwiseSDKVersion.h>

namespace
{
    // Fast ~48-byte fingerprint so replacing an IR with another WAV of the same
    // size at the same address still triggers SetIR().
    AkUInt32 ComputeMediaSignature(const AkUInt8* in_pData, AkUInt32 in_uSize)
    {
        if (!in_pData || in_uSize == 0)
            return 0;

        AkUInt32 hash = 2166136261u;
        const AkUInt32 headerBytes = (in_uSize < sizeof(OKIRMediaHeader))
            ? in_uSize
            : static_cast<AkUInt32>(sizeof(OKIRMediaHeader));

        for (AkUInt32 i = 0; i < headerBytes; ++i)
        {
            hash ^= in_pData[i];
            hash *= 16777619u;
        }

        if (in_uSize > headerBytes)
        {
            const AkUInt32 payloadSize = in_uSize - headerBytes;
            const AkUInt32 stride = (payloadSize > 16u) ? (payloadSize / 16u) : 1u;
            for (AkUInt32 offset = 0; offset < payloadSize; offset += stride)
            {
                hash ^= in_pData[headerBytes + offset];
                hash *= 16777619u;
            }
        }

        return hash;
    }
}

AK::IAkPlugin* CreateOKConvolutionFX(AK::IAkPluginMemAlloc* in_pAllocator)
{
    return AK_PLUGIN_NEW(in_pAllocator, OKConvolutionFX());
}

AK::IAkPluginParam* CreateOKConvolutionFXParams(AK::IAkPluginMemAlloc* in_pAllocator)
{
    return AK_PLUGIN_NEW(in_pAllocator, OKConvolutionFXParams());
}

AK_IMPLEMENT_PLUGIN_FACTORY(OKConvolutionFX, AkPluginTypeEffect, OKConvolutionConfig::CompanyID, OKConvolutionConfig::PluginID)

OKConvolutionFX::OKConvolutionFX()
    : m_pParams(nullptr)
    , m_pAllocator(nullptr)
    , m_pContext(nullptr)
    , m_pEngine(nullptr)
    , m_mediaCache{}
    , m_uActiveIRSlot(0)
{
}

OKConvolutionFX::~OKConvolutionFX() = default;

AKRESULT OKConvolutionFX::Init(
    AK::IAkPluginMemAlloc* in_pAllocator,
    AK::IAkEffectPluginContext* in_pContext,
    AK::IAkPluginParam* in_pParams,
    AkAudioFormat& in_rFormat)
{
    m_pParams    = static_cast<OKConvolutionFXParams*>(in_pParams);
    m_pAllocator = in_pAllocator;
    m_pContext   = in_pContext;

    m_pEngine = AK_PLUGIN_NEW(in_pAllocator, ConvolutionEngine());
    if (!m_pEngine)
        return AK_InsufficientMemory;

    const AkUInt32 uBlockSize   = in_pContext->GlobalContext()->GetMaxBufferLength();
    const AkUInt32 uNumChannels = in_rFormat.GetNumChannels();
    const AkUInt32 uSampleRate  = in_rFormat.uSampleRate;

    AKRESULT res = m_pEngine->Init(in_pAllocator, uBlockSize, uNumChannels, uSampleRate);
    if (res != AK_Success)
        return res;

    // Preload and FFT-partition all populated media slots up front so runtime slot
    // switches perform zero heap allocations and zero FFT builds on the audio thread.
    const AKRESULT syncRes = SyncAllMediaSlots();
    if (syncRes != AK_Success)
        return syncRes;

    const AkUInt32 uRequestedSlot = m_pParams ? static_cast<AkUInt32>(m_pParams->RTPC.fActiveIR) : 0u;
    m_uActiveIRSlot = (std::min)(uRequestedSlot, OKConvolutionConfig::MaxIRSlots - 1u);
    // Snap directly to the initial IR slot without fading in on voice/bus start.
    m_pEngine->SetActiveIR(m_uActiveIRSlot, 0.0f);

    return AK_Success;
}

AKRESULT OKConvolutionFX::Term(AK::IAkPluginMemAlloc* in_pAllocator)
{
    if (m_pEngine)
    {
        m_pEngine->Term(in_pAllocator);
        AK_PLUGIN_DELETE(in_pAllocator, m_pEngine);
        m_pEngine = nullptr;
    }

    AK_PLUGIN_DELETE(in_pAllocator, this);
    return AK_Success;
}

AKRESULT OKConvolutionFX::Reset()
{
    if (m_pEngine)
        m_pEngine->Reset();

    return AK_Success;
}

AKRESULT OKConvolutionFX::GetPluginInfo(AkPluginInfo& out_rPluginInfo)
{
    out_rPluginInfo.eType = AkPluginTypeEffect;
    out_rPluginInfo.bIsInPlace = true;
    out_rPluginInfo.bCanProcessObjects = false;
    out_rPluginInfo.uBuildVersion = AK_WWISESDK_VERSION_COMBINED;
    return AK_Success;
}

void OKConvolutionFX::Execute(AkAudioBuffer* io_pBuffer)
{
    if (!m_pEngine || !m_pParams)
        return;

    // Poll all 4 media slots so IRs loaded, replaced, or cleared in inactive slots
    // via Wwise Authoring are immediately partitioned and ready before ActiveIRIndex
    // triggers a crossfade. When media is unchanged, this is a fast cache hit with
    // zero allocations.
    SyncAllMediaSlots();

    const AkUInt32 uTargetSlot = (std::min)(
        static_cast<AkUInt32>(m_pParams->RTPC.fActiveIR),
        OKConvolutionConfig::MaxIRSlots - 1u);

    if (uTargetSlot != m_uActiveIRSlot)
    {
        m_uActiveIRSlot = uTargetSlot;
        const AkReal32 fFadeTimeMs = std::clamp(m_pParams->RTPC.fFadeTime, 0.0f, 10000.0f);
        m_pEngine->SetActiveIR(uTargetSlot, fFadeTimeMs);
    }

    const AkReal32 fMix     = std::clamp(m_pParams->RTPC.fDryWet * 0.01f, 0.0f, 1.0f);
    const AkReal32 fWetGain = fMix;
    const AkReal32 fDryGain = 1.0f - fMix;

    m_pEngine->Process(io_pBuffer, fWetGain, fDryGain);
}

AKRESULT OKConvolutionFX::TimeSkip(AkUInt32 in_uFrames)
{
    return AK_DataReady;
}

bool OKConvolutionFX::SupportMediaRelocation() const
{
    // ConvolutionEngine transforms IR samples into its own aligned partition buffers,
    // so relocating Wwise's underlying bank memory does not invalidate engine state.
    return true;
}

AKRESULT OKConvolutionFX::RelocateMedia(AkUInt8* in_pNewMedia, AkUInt8* in_pOldMedia)
{
    if (in_pOldMedia == nullptr)
        return AK_Success;

    // Keep all cached slot pointers aligned with Wwise's relocated address so
    // SyncAllMediaSlots() does not falsely treat a memory move as a new IR.
    for (AkUInt32 slot = 0; slot < OKConvolutionConfig::MaxIRSlots; ++slot)
    {
        if (m_mediaCache[slot].pData == in_pOldMedia)
            m_mediaCache[slot].pData = in_pNewMedia;
    }

    return AK_Success;
}

bool OKConvolutionFX::RefreshMediaSlotCache(AkUInt32 in_uMediaIndex)
{
    if (!m_pContext || in_uMediaIndex >= OKConvolutionConfig::MaxIRSlots)
        return false;

    AkUInt8* pMediaData = nullptr;
    AkUInt32 uMediaSize = 0;
    m_pContext->GetPluginMedia(in_uMediaIndex, pMediaData, uMediaSize);

    const AkUInt32 uSignature = ComputeMediaSignature(pMediaData, uMediaSize);
    MediaSlotCache& cache = m_mediaCache[in_uMediaIndex];

    if (pMediaData == cache.pData
        && uMediaSize == cache.uSize
        && uSignature == cache.uSignature)
    {
        return false;
    }

    cache.pData      = pMediaData;
    cache.uSize      = uMediaSize;
    cache.uSignature = uSignature;
    return true;
}

AKRESULT OKConvolutionFX::UpdateIRFromMedia(AkUInt32 in_uMediaIndex)
{
    if (!m_pContext || !m_pEngine || in_uMediaIndex >= OKConvolutionConfig::MaxIRSlots)
        return AK_InvalidParameter;

    const MediaSlotCache prevCache = m_mediaCache[in_uMediaIndex];
    if (!RefreshMediaSlotCache(in_uMediaIndex))
        return AK_Success;

    const MediaSlotCache& cache = m_mediaCache[in_uMediaIndex];
    const AKRESULT res = m_pEngine->SetIRSlot(in_uMediaIndex, cache.pData, cache.uSize);

    // Keep the updated cache on AK_InvalidFile so a corrupt asset is not re-parsed
    // every audio block, but revert on transient errors (e.g., AK_InsufficientMemory)
    // so a subsequent sync can retry once memory becomes available.
    if (res != AK_Success && res != AK_InvalidFile)
        m_mediaCache[in_uMediaIndex] = prevCache;

    return res;
}

AKRESULT OKConvolutionFX::SyncAllMediaSlots()
{
    AKRESULT firstFatalError = AK_Success;

    for (AkUInt32 slot = 0; slot < OKConvolutionConfig::MaxIRSlots; ++slot)
    {
        const AKRESULT slotRes = UpdateIRFromMedia(slot);
        if (slotRes != AK_Success && slotRes != AK_InvalidFile && firstFatalError == AK_Success)
            firstFatalError = slotRes;
    }

    return firstFatalError;
}
