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

#include "../SoundEnginePlugin/OKConvolutionFXParams.h"

#include <catch_amalgamated.hpp>
#include <cstring>

TEST_CASE("OKConvolutionFXParams default init matches XML defaults")
{
    OKConvolutionFXParams fxParams;
    REQUIRE(fxParams.Init(nullptr, nullptr, 0) == AK_Success);
    REQUIRE(fxParams.RTPC.fActiveIR == 0);
    REQUIRE(fxParams.RTPC.fDryWet == Catch::Approx(100.0f));
    REQUIRE(fxParams.RTPC.fFadeTime == Catch::Approx(100.0f));
}

TEST_CASE("OKConvolutionFXParams SetParamsBlock parses 10-byte bank layout")
{
    // Matches GetBankParameters(): WriteUInt16(ActiveIRIndex), WriteReal32(DryWetMix), WriteReal32(CrossFadeTime)
    AkUInt8 bankData[sizeof(AkUInt16) + sizeof(AkReal32) + sizeof(AkReal32)] = {};
    const AkUInt16 expectedSlot = 2;
    const AkReal32 expectedDryWet = 42.5f;
    const AkReal32 expectedFadeTime = 250.0f;

    std::memcpy(bankData, &expectedSlot, sizeof(expectedSlot));
    std::memcpy(bankData + sizeof(AkUInt16), &expectedDryWet, sizeof(expectedDryWet));
    std::memcpy(bankData + sizeof(AkUInt16) + sizeof(AkReal32), &expectedFadeTime, sizeof(expectedFadeTime));

    OKConvolutionFXParams fxParams;
    REQUIRE(fxParams.SetParamsBlock(bankData, static_cast<AkUInt32>(sizeof(bankData))) == AK_Success);
    REQUIRE(fxParams.RTPC.fActiveIR == expectedSlot);
    REQUIRE(fxParams.RTPC.fDryWet == Catch::Approx(expectedDryWet));
    REQUIRE(fxParams.RTPC.fFadeTime == Catch::Approx(expectedFadeTime));
}

TEST_CASE("OKConvolutionFXParams SetParam updates ActiveIRIndex from both AkUInt16 and AkReal32 RTPC")
{
    OKConvolutionFXParams fxParams;
    REQUIRE(fxParams.Init(nullptr, nullptr, 0) == AK_Success);

    const AkUInt16 slotU16 = 1;
    REQUIRE(fxParams.SetParam(PARAM_ACTIVEIR_ID, &slotU16, sizeof(slotU16)) == AK_Success);
    REQUIRE(fxParams.RTPC.fActiveIR == 1);

    const AkReal32 slotF32 = 3.0f;
    REQUIRE(fxParams.SetParam(PARAM_ACTIVEIR_ID, &slotF32, sizeof(slotF32)) == AK_Success);
    REQUIRE(fxParams.RTPC.fActiveIR == 3);
}
