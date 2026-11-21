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

#ifndef OKConvolutionFX_H
#define OKConvolutionFX_H

#include "ConvolutionEngine.h"
#include "OKConvolutionFXParams.h"

class OKConvolutionFX
    : public AK::IAkInPlaceEffectPlugin
{
public:
  OKConvolutionFX();
  ~OKConvolutionFX() override;

  AKRESULT Init(AK::IAkPluginMemAlloc* in_pAllocator, AK::IAkEffectPluginContext* in_pContext, AK::IAkPluginParam* in_pParams, AkAudioFormat& in_rFormat) override;
  AKRESULT Term(AK::IAkPluginMemAlloc* in_pAllocator) override;
  AKRESULT Reset() override;
  AKRESULT GetPluginInfo(AkPluginInfo& out_rPluginInfo) override;
  void Execute(AkAudioBuffer* io_pBuffer) override;
  AKRESULT TimeSkip(AkUInt32 in_uFrames) override;
  bool SupportMediaRelocation() const override;
  AKRESULT RelocateMedia(AkUInt8* in_pNewMedia, AkUInt8* in_pOldMedia) override;

private:
  AKRESULT UpdateIRFromMedia(AkUInt32 in_uMediaIndex);

  OKConvolutionFXParams* m_pParams;
  AK::IAkPluginMemAlloc* m_pAllocator;
  AK::IAkEffectPluginContext* m_pContext;
  ConvolutionEngine* m_pEngine;

  const AkUInt8* m_pLastMediaData;
  AkUInt32 m_uLastMediaSize;
  AkUInt32 m_uLastMediaSignature;
};

#endif // OKConvolutionFX_H