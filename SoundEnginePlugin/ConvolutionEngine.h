#pragma once
#include <AK/SoundEngine/Common/AkCommonDefs.h>
#include <AK/SoundEngine/Common/IAkPluginMemAlloc.h>
#include <AK/Tools/Common/AkArray.h>
#include "pffft.h"


class ConvolutionEngine
{
public:
    AKRESULT Init(AK::IAkPluginMemAlloc* in_pAllocator);
    void Term(AK::IAkPluginMemAlloc* in_pAllocator);
    
    // Clears internal delay lines and overlap buffers (e.g., when voice/bus restarts)
    void Reset();
    
    AKRESULT SetIR();
    void Process(AkAudioBuffer* io_pBuffer, float in_fWetGain, float in_fDryGain);
    
private:
};
