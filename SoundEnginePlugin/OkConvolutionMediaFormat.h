#pragma once

#include <cstdint>

static constexpr uint32_t kOKIRMediaMagic   = 0x52494B4Fu; // 'OKIR' (little-endian)
static constexpr uint32_t kOKIRMediaVersion = 1u;

// Binary layout written by OKConvolutionPlugin::ConvertFile()
// and parsed by ConvolutionEngine::SetIR():
//   [OKIRMediaHeader (32 bytes)]
//   [Channel 0: numFrames * float]
//   [Channel 1: numFrames * float]
//   ...
struct OKIRMediaHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t sampleRate;
    uint32_t numChannels;
    uint32_t numFrames;
    uint32_t reserved[3]; // Zero-padded to 32 bytes
};

static_assert(sizeof(OKIRMediaHeader) == 32, "OKIRMediaHeader must be 32 bytes.");
