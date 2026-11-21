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

#include "OKConvolutionPlugin.h"
#include "../SoundEnginePlugin/OKConvolutionMediaFormat.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint16_t kWavFormatPCM        = 0x0001;
    constexpr uint16_t kWavFormatIEEEFloat  = 0x0003;
    constexpr uint16_t kWavFormatExtensible = 0xFFFE;

    // Standard KSDATAFORMAT_SUBTYPE_PCM and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT GUIDs
    constexpr uint8_t kSubFormatPCM[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
    };
    constexpr uint8_t kSubFormatIEEEFloat[16] = {
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
    };

    struct DecodedWav
    {
        uint32_t sampleRate = 0;
        uint32_t numChannels = 0;
        uint32_t numFrames = 0;
        std::vector<float> planarSamples; // [ch * numFrames + frame]
    };

    void ReportError(AK::Wwise::Plugin::IWriteString* io_pError, const std::string& in_msg)
    {
        if (io_pError && !in_msg.empty())
        {
            io_pError->WriteString(in_msg.c_str(), static_cast<int>(in_msg.size()));
        }
    }

    uint16_t ReadLE16(const uint8_t* p)
    {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    uint32_t ReadLE32(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0])
            | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16)
            | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool ParseWavFile(const std::filesystem::path& in_path, DecodedWav& out_wav, std::string& out_error)
    {
        std::ifstream file(in_path, std::ios::binary);
        if (!file.is_open())
        {
            out_error = "OKConvolution: Failed to open source IR file.";
            return false;
        }

        uint8_t riffHeader[12];
        if (!file.read(reinterpret_cast<char*>(riffHeader), sizeof(riffHeader)))
        {
            out_error = "OKConvolution: Source file is too small to be a valid WAV.";
            return false;
        }

        if (std::memcmp(riffHeader, "RIFF", 4) != 0 || std::memcmp(riffHeader + 8, "WAVE", 4) != 0)
        {
            out_error = "OKConvolution: Only standard RIFF/WAVE (.wav) IR files are supported.";
            return false;
        }

        bool hasFmt = false;
        bool hasData = false;
        uint16_t audioFormat = 0;
        uint16_t numChannels = 0;
        uint32_t sampleRate = 0;
        uint16_t blockAlign = 0;
        uint16_t bitsPerSample = 0;
        std::vector<uint8_t> rawData;

        uint8_t chunkHeader[8];
        while (file.read(reinterpret_cast<char*>(chunkHeader), sizeof(chunkHeader)))
        {
            const uint32_t chunkSize = ReadLE32(chunkHeader + 4);

            if (std::memcmp(chunkHeader, "fmt ", 4) == 0)
            {
                if (chunkSize < 16)
                {
                    out_error = "OKConvolution: Invalid WAV 'fmt ' chunk size.";
                    return false;
                }

                std::vector<uint8_t> fmtBuf(chunkSize);
                if (!file.read(reinterpret_cast<char*>(fmtBuf.data()), chunkSize))
                {
                    out_error = "OKConvolution: Unexpected EOF while reading 'fmt ' chunk.";
                    return false;
                }

                audioFormat   = ReadLE16(fmtBuf.data() + 0);
                numChannels   = ReadLE16(fmtBuf.data() + 2);
                sampleRate    = ReadLE32(fmtBuf.data() + 4);
                blockAlign    = ReadLE16(fmtBuf.data() + 12);
                bitsPerSample = ReadLE16(fmtBuf.data() + 14);

                if (audioFormat == kWavFormatExtensible)
                {
                    if (chunkSize < 40)
                    {
                        out_error = "OKConvolution: Truncated WAVE_FORMAT_EXTENSIBLE header.";
                        return false;
                    }
                    const uint8_t* subFormat = fmtBuf.data() + 24;
                    if (std::memcmp(subFormat, kSubFormatPCM, 16) == 0)
                    {
                        audioFormat = kWavFormatPCM;
                    }
                    else if (std::memcmp(subFormat, kSubFormatIEEEFloat, 16) == 0)
                    {
                        audioFormat = kWavFormatIEEEFloat;
                    }
                    else
                    {
                        out_error = "OKConvolution: Unsupported WAVE_FORMAT_EXTENSIBLE subformat.";
                        return false;
                    }
                }

                hasFmt = true;
            }
            else if (std::memcmp(chunkHeader, "data", 4) == 0)
            {
                rawData.resize(chunkSize);
                if (chunkSize > 0 && !file.read(reinterpret_cast<char*>(rawData.data()), chunkSize))
                {
                    out_error = "OKConvolution: Unexpected EOF while reading 'data' chunk.";
                    return false;
                }
                hasData = true;
            }
            else
            {
                file.seekg(chunkSize, std::ios::cur);
            }

            // RIFF chunks are word-aligned (2 bytes)
            if (chunkSize & 1u)
            {
                file.seekg(1, std::ios::cur);
            }

            if (hasFmt && hasData)
                break;
        }

        if (!hasFmt || !hasData)
        {
            out_error = "OKConvolution: WAV file is missing a 'fmt ' or 'data' chunk.";
            return false;
        }

        if (numChannels == 0 || sampleRate == 0 || bitsPerSample == 0 || (bitsPerSample % 8) != 0)
        {
            out_error = "OKConvolution: Invalid WAV channel count, sample rate, or bit depth.";
            return false;
        }

        const uint32_t bytesPerSample = bitsPerSample / 8u;
        const uint32_t frameStride = (blockAlign >= numChannels * bytesPerSample)
            ? blockAlign
            : (numChannels * bytesPerSample);

        const uint32_t numFrames = static_cast<uint32_t>(rawData.size() / frameStride);
        if (numFrames == 0)
        {
            out_error = "OKConvolution: Source IR WAV contains zero audio frames.";
            return false;
        }

        out_wav.sampleRate = sampleRate;
        out_wav.numChannels = numChannels;
        out_wav.numFrames = numFrames;
        out_wav.planarSamples.resize(static_cast<size_t>(numChannels) * numFrames);

        const uint8_t* src = rawData.data();

        for (uint32_t frame = 0; frame < numFrames; ++frame)
        {
            const uint8_t* framePtr = src + static_cast<size_t>(frame) * frameStride;
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const uint8_t* s = framePtr + static_cast<size_t>(ch) * bytesPerSample;
                float sample = 0.0f;

                if (audioFormat == kWavFormatPCM)
                {
                    switch (bitsPerSample)
                    {
                    case 8:
                        sample = (static_cast<float>(s[0]) - 128.0f) / 128.0f;
                        break;
                    case 16:
                    {
                        const int16_t v = static_cast<int16_t>(ReadLE16(s));
                        sample = static_cast<float>(v) / 32768.0f;
                        break;
                    }
                    case 24:
                    {
                        int32_t v = static_cast<int32_t>(s[0])
                                  | (static_cast<int32_t>(s[1]) << 8)
                                  | (static_cast<int32_t>(s[2]) << 16);
                        if (v & 0x00800000) v |= ~0x00FFFFFF; // Sign-extend 24-bit
                        sample = static_cast<float>(v) / 8388608.0f;
                        break;
                    }
                    case 32:
                    {
                        const int32_t v = static_cast<int32_t>(ReadLE32(s));
                        sample = static_cast<float>(static_cast<double>(v) / 2147483648.0);
                        break;
                    }
                    default:
                        out_error = "OKConvolution: Unsupported PCM bit depth (supported: 8, 16, 24, 32).";
                        return false;
                    }
                }
                else if (audioFormat == kWavFormatIEEEFloat)
                {
                    if (bitsPerSample == 32)
                    {
                        std::memcpy(&sample, s, sizeof(float));
                    }
                    else if (bitsPerSample == 64)
                    {
                        double d = 0.0;
                        std::memcpy(&d, s, sizeof(double));
                        sample = static_cast<float>(d);
                    }
                    else
                    {
                        out_error = "OKConvolution: Unsupported IEEE Float bit depth (supported: 32, 64).";
                        return false;
                    }
                }
                else
                {
                    out_error = "OKConvolution: Unsupported WAV encoding (only PCM and IEEE Float supported).";
                    return false;
                }

                out_wav.planarSamples[static_cast<size_t>(ch) * numFrames + frame] = sample;
            }
        }

        return true;
    }

    // Lightweight linear resampler for matching target platform sample rate if specified
    void ResampleLinearIfNeeded(DecodedWav& io_wav, uint32_t in_targetSampleRate)
    {
        if (in_targetSampleRate == 0 || in_targetSampleRate == io_wav.sampleRate)
            return;

        const double ratio = static_cast<double>(io_wav.sampleRate) / static_cast<double>(in_targetSampleRate);
        const uint32_t dstFrames = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(std::llround(static_cast<double>(io_wav.numFrames) / ratio))
        );

        std::vector<float> resampled(static_cast<size_t>(io_wav.numChannels) * dstFrames);

        for (uint32_t ch = 0; ch < io_wav.numChannels; ++ch)
        {
            const float* srcCh = io_wav.planarSamples.data() + static_cast<size_t>(ch) * io_wav.numFrames;
            float* dstCh = resampled.data() + static_cast<size_t>(ch) * dstFrames;

            for (uint32_t i = 0; i < dstFrames; ++i)
            {
                const double srcPos = static_cast<double>(i) * ratio;
                const uint32_t idx0 = static_cast<uint32_t>(srcPos);
                const uint32_t idx1 = (std::min)(idx0 + 1u, io_wav.numFrames - 1u);
                const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));

                dstCh[i] = srcCh[idx0] + (srcCh[idx1] - srcCh[idx0]) * frac;
            }
        }

        io_wav.sampleRate = in_targetSampleRate;
        io_wav.numFrames = dstFrames;
        io_wav.planarSamples = std::move(resampled);
    }

    uint32_t HashFNV1a(uint32_t in_hash, uint32_t in_value)
    {
        for (int i = 0; i < 4; ++i)
        {
            in_hash ^= (in_value >> (i * 8)) & 0xFFu;
            in_hash *= 16777619u;
        }
        return in_hash;
    }
    
    constexpr uint32_t kIRNormalizationVersion = 1u;

    size_t NextPowerOfTwo(size_t in_value)
    {
        size_t p = 1;
        while (p < in_value)
        {
            p <<= 1;
        }
        return p;
    }

    // In-place Radix-2 Cooley-Tukey forward FFT (unnormalized: |H[k]| equals linear steady-state gain at bin k)
    void ComputeForwardFFT(
        std::vector<std::complex<double>>& io_data,
        const std::vector<std::complex<double>>& in_twiddles)
    {
        const size_t n = io_data.size();

        // Bit-reversal permutation
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
            {
                j ^= bit;
            }
            j ^= bit;
            if (i < j)
            {
                std::swap(io_data[i], io_data[j]);
            }
        }

        // Butterfly stages
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const size_t halfLen = len >> 1;
            const size_t twiddleStep = n / len;

            for (size_t i = 0; i < n; i += len)
            {
                for (size_t j = 0; j < halfLen; ++j)
                {
                    const std::complex<double> u = io_data[i + j];
                    const std::complex<double> v = io_data[i + j + halfLen] * in_twiddles[j * twiddleStep];
                    io_data[i + j]           = u + v;
                    io_data[i + j + halfLen] = u - v;
                }
            }
        }
    }

    // Equalizes perceived broadband loudness across short and long IRs via L2 energy normalization,
    // while clamping total gain if any narrow room mode exceeds the spectral peak ceiling.
    void NormalizeIRWithSpectralGuard(DecodedWav& io_wav)
    {
        if (io_wav.numChannels == 0 || io_wav.numFrames == 0 || io_wav.planarSamples.empty())
            return;

        // Tuning parameters:
        // - kTargetBroadbandGain = 1.0 (0 dB): unity RMS gain for uncorrelated broadband signals (and exact unity for a Dirac delta).
        // - kMaxSpectralPeakDb = 12.0 (+12 dB): allows normal diffuse statistical FFT peaks (~9-10.5 dB)
        //   to remain purely energy-normalized while pulling down narrow resonant room modes.
        constexpr double kTargetBroadbandGain = 1.0;
        constexpr double kMaxSpectralPeakDb   = 12.0;
        constexpr double kMinEnergyFloor      = 1e-12;
        constexpr double kTwoPi               = 6.283185307179586476925286766559;

        const double maxSpectralCeiling = std::pow(10.0, kMaxSpectralPeakDb / 20.0);

        // 1. Compute average per-channel L2 energy (and sanitize any non-finite float samples)
        double totalEnergy = 0.0;
        for (float& s : io_wav.planarSamples)
        {
            if (!std::isfinite(s))
            {
                s = 0.0f;
                continue;
            }
            const double d = static_cast<double>(s);
            totalEnergy += d * d;
        }

        const double avgChannelEnergy = totalEnergy / static_cast<double>(io_wav.numChannels);
        if (avgChannelEnergy <= kMinEnergyFloor)
            return; // Avoid amplifying digital silence

        const double energyGain = kTargetBroadbandGain / std::sqrt(avgChannelEnergy);

        // 2. Compute worst-case spectral magnitude across all channels and frequency bins.
        //    2x zero-padding for short/medium IRs reduces inter-bin scalloping loss on narrow resonances.
        constexpr size_t kMinFftSize = 4096;
        constexpr size_t kOversampleThresholdFrames = 262144; // ~5.4s @ 48 kHz
        const size_t minRequired = (io_wav.numFrames <= kOversampleThresholdFrames)
            ? (static_cast<size_t>(io_wav.numFrames) * 2u)
            : static_cast<size_t>(io_wav.numFrames);
        const size_t fftSize = (std::max)(kMinFftSize, NextPowerOfTwo(minRequired));

        std::vector<std::complex<double>> twiddles(fftSize / 2);
        for (size_t m = 0; m < fftSize / 2; ++m)
        {
            const double angle = -kTwoPi * static_cast<double>(m) / static_cast<double>(fftSize);
            twiddles[m] = std::complex<double>(std::cos(angle), std::sin(angle));
        }

        std::vector<std::complex<double>> fftBuf(fftSize);
        double maxSpectralMagSq = 0.0;

        for (uint32_t ch = 0; ch < io_wav.numChannels; ++ch)
        {
            const float* srcCh = io_wav.planarSamples.data() + static_cast<size_t>(ch) * io_wav.numFrames;

            for (uint32_t i = 0; i < io_wav.numFrames; ++i)
            {
                fftBuf[i] = std::complex<double>(static_cast<double>(srcCh[i]), 0.0);
            }
            std::fill(fftBuf.begin() + io_wav.numFrames, fftBuf.end(), std::complex<double>(0.0, 0.0));

            ComputeForwardFFT(fftBuf, twiddles);

            // Real input has Hermitian symmetry, so bins [0 .. fftSize/2] cover the entire spectrum
            for (size_t k = 0; k <= fftSize / 2; ++k)
            {
                maxSpectralMagSq = (std::max)(maxSpectralMagSq, std::norm(fftBuf[k]));
            }
        }

        // 3. Combine both targets conservatively using a single scalar gain across all channels
        double finalGain = energyGain;
        if (maxSpectralMagSq > kMinEnergyFloor)
        {
            const double maxSpectralMag = std::sqrt(maxSpectralMagSq);
            const double spectralGuardGain = maxSpectralCeiling / maxSpectralMag;
            finalGain = (std::min)(energyGain, spectralGuardGain);
        }

        const float gain = static_cast<float>(finalGain);
        for (float& s : io_wav.planarSamples)
        {
            s *= gain;
        }
    }
}

OKConvolutionPlugin::OKConvolutionPlugin()
{
}

OKConvolutionPlugin::~OKConvolutionPlugin()
{
}

bool OKConvolutionPlugin::GetBankParameters(const GUID& in_guidPlatform, AK::Wwise::Plugin::DataWriter& in_dataWriter) const
{
    // Write bank data here
    in_dataWriter.WriteInt32(m_propertySet.GetUInt16(in_guidPlatform, "ActiveIRIndex"));
    in_dataWriter.WriteReal32(m_propertySet.GetReal32(in_guidPlatform, "DryWetMix"));
    in_dataWriter.WriteReal32(m_propertySet.GetReal32(in_guidPlatform, "CrossFadeTime"));

    return true;
}

void OKConvolutionPlugin::NotifyPluginMediaChanged()
{
    m_host.NotifyInternalDataChanged(AK::IAkPluginParam::ALL_PLUGIN_DATA_ID, true);
}

AK::Wwise::Plugin::ConversionResult OKConvolutionPlugin::ConvertFile(
    const GUID& in_guidPlatform,
    const BasePlatformID& in_basePlatform,
    const AkOSChar* in_szSourceFile,
    const AkOSChar* in_szDestFile,
    AkUInt32 in_uSampleRate,
    AkUInt32 in_uBlockLength,
    AK::Wwise::Plugin::IProgress* in_pProgress,
    AK::Wwise::Plugin::IWriteString* io_pError) const
{
    if (!in_szSourceFile || !in_szDestFile)
    {
        ReportError(io_pError, "OKConvolution: Source or destination file path is null.");
        return AK::Wwise::Plugin::ConversionFailed;
    }

    if (in_pProgress && in_pProgress->IsCancelled())
    {
        return AK::Wwise::Plugin::ConversionFailed;
    }

    DecodedWav wav;
    std::string errorMsg;
    if (!ParseWavFile(std::filesystem::path(in_szSourceFile), wav, errorMsg))
    {
        ReportError(io_pError, errorMsg);
        return AK::Wwise::Plugin::ConversionFailed;
    }

    if (in_pProgress && in_pProgress->IsCancelled())
    {
        return AK::Wwise::Plugin::ConversionFailed;
    }

    // If Wwise specifies a target sample rate for the platform, resample to match it
    ResampleLinearIfNeeded(wav, in_uSampleRate);
    
    // Normalize IR energy across short/long IRs while guarding against narrow modal peaks
    NormalizeIRWithSpectralGuard(wav);

    OKIRMediaHeader header{};
    header.magic       = kOKIRMediaMagic;
    header.version     = kOKIRMediaVersion;
    header.sampleRate  = wav.sampleRate;
    header.numChannels = wav.numChannels;
    header.numFrames   = wav.numFrames;

    std::ofstream outFile(std::filesystem::path(in_szDestFile), std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
    {
        ReportError(io_pError, "OKConvolution: Failed to open destination file for writing.");
        return AK::Wwise::Plugin::ConversionFailed;
    }

    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    outFile.write(
        reinterpret_cast<const char*>(wav.planarSamples.data()),
        static_cast<std::streamsize>(wav.planarSamples.size() * sizeof(float))
    );

    if (!outFile.good())
    {
        ReportError(io_pError, "OKConvolution: Failed while writing converted IR binary.");
        return AK::Wwise::Plugin::ConversionFailed;
    }

    return AK::Wwise::Plugin::ConversionSuccess;
}

uint32_t OKConvolutionPlugin::GetCurrentConversionSettingsHash(
    const GUID& in_guidPlatform,
    AkUInt32 in_uSampleRate,
    AkUInt32 in_uBlockLength) const
{
    uint32_t hash = 2166136261u; // FNV-1a offset basis
    hash = HashFNV1a(hash, kOKIRMediaVersion);
    hash = HashFNV1a(hash, kIRNormalizationVersion);
    hash = HashFNV1a(hash, in_uSampleRate);
    hash = HashFNV1a(hash, in_uBlockLength);
    return hash;
}

AK_DEFINE_PLUGIN_CONTAINER(OKConvolution);
AK_EXPORT_PLUGIN_CONTAINER(OKConvolution);
AK_ADD_PLUGIN_CLASS_TO_CONTAINER(
    OKConvolution,
    OKConvolutionPlugin,
    OKConvolutionFX
);
DEFINE_PLUGIN_REGISTER_HOOK

DEFINE_PLUGIN_ASSERT_HOOK;