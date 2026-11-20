// Native audio: envelope, modulation, mix, pan and filter profiles read from code.bin.
#include "oot3d_native_audio_internal.h"

namespace Oot3dNativeGame {

NativeAudioEnvelopeLayout Oot3dEurRev0AudioEnvelopeLayout() {
    return {
        0x00100000,
        0x004eb7d0,
        0x004eb7d4,
        0x004eb8d4,
        0x004e7960,
        0x00309f7c,
        0x00309f80,
        0x00309f84,
        0x00309f88,
        0x00309f8c,
        0x0030a0d4,
        0x0030a0d8,
        0x0030a0dc,
        0x0030a0e0,
        0x0030a0e4,
        0x002cdec0,
        0x002cddc4,
        0x002cddc8,
        0x002cddcc,
        965,
    };
}

NativeAudioModulationLayout Oot3dEurRev0AudioModulationLayout() {
    return {
        0x00100000,
        0x002cde84,
        0x002cde88,
        0x002cde8c,
        0x00487a28,
        0x0047fbb0,
        33,
    };
}

NativeAudioMixLayout Oot3dEurRev0AudioMixLayout() {
    return {
        0x00100000,
        0x00558b18,
        0x00558b24,
        0x00309b54,
        0x00309b54,
        0x00309b58,
    };
}

NativeAudioFilterLayout Oot3dEurRev0AudioFilterLayout() {
    return {
        0x00100000,
        0x00486868,
        0x0048686c,
        0x00486870,
        0x00486874,
        0x00486878,
        0x0048687c,
        0x00486880,
        0x004859a4,
        0x004859a8,
        0x004859ac,
        0x004859b0,
        0x003722f4,
        0x003722fc,
        23,
        256,
        16,
    };
}

namespace {

size_t NativeCodeOffsetForBase(std::span<const uint8_t> codeBin,
                               uint32_t codeBaseAddress,
                               uint32_t address, size_t size,
                               const char* role) {
    if (codeBaseAddress == 0 || address < codeBaseAddress) {
        throw std::runtime_error(std::string("native audio ") + role +
                                 " address precedes code.bin");
    }
    const size_t offset = static_cast<size_t>(address - codeBaseAddress);
    if (offset > codeBin.size() || size > codeBin.size() - offset) {
        throw std::runtime_error(std::string("native audio ") + role +
                                 " address exceeds code.bin");
    }
    return offset;
}

uint32_t NativeCodeU32ForBase(std::span<const uint8_t> codeBin,
                              uint32_t codeBaseAddress,
                              uint32_t address, const char* role) {
    const size_t offset = NativeCodeOffsetForBase(
        codeBin, codeBaseAddress, address, 4, role);
    return static_cast<uint32_t>(codeBin[offset]) |
           (static_cast<uint32_t>(codeBin[offset + 1]) << 8) |
           (static_cast<uint32_t>(codeBin[offset + 2]) << 16) |
           (static_cast<uint32_t>(codeBin[offset + 3]) << 24);
}

float NativeCodeFloatForBase(std::span<const uint8_t> codeBin,
                             uint32_t codeBaseAddress,
                             uint32_t address, const char* role) {
    return std::bit_cast<float>(NativeCodeU32ForBase(
        codeBin, codeBaseAddress, address, role));
}

size_t NativeCodeOffset(std::span<const uint8_t> codeBin,
                        const NativeAudioEnvelopeLayout& layout,
                        uint32_t address, size_t size) {
    return NativeCodeOffsetForBase(
        codeBin, layout.CodeBaseAddress, address, size, "envelope");
}

uint32_t NativeCodeU32(std::span<const uint8_t> codeBin,
                       const NativeAudioEnvelopeLayout& layout,
                       uint32_t address) {
    return NativeCodeU32ForBase(
        codeBin, layout.CodeBaseAddress, address, "envelope");
}

float NativeCodeFloat(std::span<const uint8_t> codeBin,
                      const NativeAudioEnvelopeLayout& layout,
                      uint32_t address) {
    return std::bit_cast<float>(NativeCodeU32(codeBin, layout, address));
}

NativeAudioEnvelopeRateProfile ParseEnvelopeRate(
    std::span<const uint8_t> codeBin,
    const NativeAudioEnvelopeLayout& layout,
    uint32_t immediateAddress, uint32_t nearImmediateAddress,
    uint32_t scaleAddress, uint32_t lowSlopeAddress,
    uint32_t highNumeratorAddress) {
    return {
        NativeCodeFloat(codeBin, layout, immediateAddress),
        NativeCodeFloat(codeBin, layout, nearImmediateAddress),
        NativeCodeFloat(codeBin, layout, scaleAddress),
        NativeCodeFloat(codeBin, layout, lowSlopeAddress),
        NativeCodeFloat(codeBin, layout, highNumeratorAddress),
    };
}

float EnvelopeRateForCode(const NativeAudioEnvelopeRateProfile& rate,
                          uint8_t code) {
    if (code == 127) {
        return rate.Immediate;
    }
    if (code == 126) {
        return rate.NearImmediate;
    }
    const float raw = code < 50
        ? static_cast<float>(code * 2 + 1) * rate.LowSlope
        : rate.HighNumerator / static_cast<float>(126 - code);
    return raw * rate.Scale;
}

} // namespace

float NativeAudioModulationProfile::SampleSine(float phase) const {
    if (SineQuarterTable.size() < 33 || PhaseScale <= 0.0f) {
        return 0.0f;
    }
    phase -= std::floor(phase);
    const int index = std::clamp(
        static_cast<int>(phase * PhaseScale), 0, 127);
    int sample = 0;
    if (index < 32) {
        sample = SineQuarterTable[static_cast<size_t>(index)];
    } else if (index < 64) {
        sample = SineQuarterTable[static_cast<size_t>(64 - index)];
    } else if (index < 96) {
        sample = -SineQuarterTable[static_cast<size_t>(index - 64)];
    } else {
        sample = -SineQuarterTable[static_cast<size_t>(128 - index)];
    }
    return static_cast<float>(sample) * OutputScale;
}

NativeAudioModulationProfile ParseNativeAudioModulationProfile(
    std::span<const uint8_t> codeBin,
    const NativeAudioModulationLayout& layout) {
    if (layout.CodeBaseAddress == 0 || layout.SineQuarterTableCount < 33) {
        throw std::runtime_error("native audio modulation layout is incomplete");
    }
    NativeAudioModulationProfile profile;
    profile.PhaseScale = NativeCodeFloatForBase(
        codeBin, layout.CodeBaseAddress, layout.PhaseScaleAddress,
        "modulation");
    profile.OutputScale = NativeCodeFloatForBase(
        codeBin, layout.CodeBaseAddress, layout.OutputScaleAddress,
        "modulation");
    profile.PhaseAdvanceScale = NativeCodeFloatForBase(
        codeBin, layout.CodeBaseAddress, layout.PhaseAdvanceScaleAddress,
        "modulation");
    const uint32_t sineAddress = NativeCodeU32ForBase(
        codeBin, layout.CodeBaseAddress, layout.SineTablePointerAddress,
        "modulation");
    const size_t sineOffset = NativeCodeOffsetForBase(
        codeBin, layout.CodeBaseAddress, sineAddress,
        layout.SineQuarterTableCount, "modulation sine table");
    profile.SineQuarterTable.reserve(layout.SineQuarterTableCount);
    for (uint32_t index = 0; index < layout.SineQuarterTableCount; ++index) {
        profile.SineQuarterTable.push_back(
            static_cast<int8_t>(codeBin[sineOffset + index]));
    }

    const uint32_t updateInstruction = NativeCodeU32ForBase(
        codeBin, layout.CodeBaseAddress,
        layout.UpdateMillisecondsInstructionAddress, "modulation update");
    if ((updateInstruction & 0x0fe0f000u) != 0x03a01000u) {
        throw std::runtime_error(
            "native audio modulation cadence is not an ARM MOV r1 immediate");
    }
    const uint32_t immediate = updateInstruction & 0xffu;
    const uint32_t rotation = ((updateInstruction >> 8) & 0x0fu) * 2u;
    profile.UpdateMilliseconds = std::rotr(immediate, rotation);

    if (std::abs(profile.PhaseScale - 128.0f) > 0.001f ||
        profile.OutputScale <= 0.0f || profile.PhaseAdvanceScale <= 0.0f ||
        profile.UpdateMilliseconds == 0 ||
        profile.SineQuarterTable[0] != 0 ||
        profile.SineQuarterTable[32] <= 0) {
        throw std::runtime_error("native audio modulation profile is invalid");
    }
    return profile;
}

NativeAudioMixProfile ParseNativeAudioMixProfile(
    std::span<const uint8_t> codeBin,
    const NativeAudioMixLayout& layout) {
    if (layout.CodeBaseAddress == 0 ||
        layout.StandardPanCurvePointerTableAddress == 0 ||
        layout.AlternatePanCurvePointerTableAddress == 0) {
        throw std::runtime_error("native audio mix layout is incomplete");
    }

    NativeAudioMixProfile profile;
    profile.PanScale = NativeCodeFloatForBase(
        codeBin, layout.CodeBaseAddress, layout.PanScaleAddress, "mix pan scale");
    profile.SurroundLowerScale = NativeCodeFloatForBase(
        codeBin, layout.CodeBaseAddress, layout.SurroundLowerScaleAddress,
        "mix lower surround scale");
    profile.SurroundUpperScale = NativeCodeFloatForBase(
        codeBin, layout.CodeBaseAddress, layout.SurroundUpperScaleAddress,
        "mix upper surround scale");

    const auto readCurves = [&](uint32_t pointerTableAddress,
                                auto& destination, const char* role) {
        for (size_t family = 0; family < destination.size(); ++family) {
            const uint32_t curveAddress = NativeCodeU32ForBase(
                codeBin, layout.CodeBaseAddress,
                pointerTableAddress + static_cast<uint32_t>(family * 4), role);
            for (size_t index = 0; index < destination[family].size(); ++index) {
                destination[family][index] = NativeCodeFloatForBase(
                    codeBin, layout.CodeBaseAddress,
                    curveAddress + static_cast<uint32_t>(index * 4), role);
            }
        }
    };
    readCurves(layout.StandardPanCurvePointerTableAddress,
               profile.StandardPanCurves, "standard mix curve");
    readCurves(layout.AlternatePanCurvePointerTableAddress,
               profile.AlternatePanCurves, "alternate mix curve");

    const auto validCurves = [](const auto& curves) {
        for (const auto& curve : curves) {
            if (curve.back() != 0.0f) {
                return false;
            }
            for (size_t index = 0; index < curve.size(); ++index) {
                if (!std::isfinite(curve[index]) || curve[index] < 0.0f ||
                    curve[index] > 2.0f ||
                    (index != 0 && curve[index] > curve[index - 1])) {
                    return false;
                }
            }
        }
        return true;
    };
    if (std::abs(profile.PanScale - (1.0f / 63.0f)) > 0.000001f ||
        std::abs(profile.SurroundLowerScale - (1.0f / 63.0f)) > 0.000001f ||
        std::abs(profile.SurroundUpperScale - (1.0f / 64.0f)) > 0.000001f ||
        !validCurves(profile.StandardPanCurves) ||
        !validCurves(profile.AlternatePanCurves)) {
        throw std::runtime_error("native audio mix profile is invalid");
    }
    return profile;
}

float NormalizeNativeAudioPan(uint8_t pan,
                              const NativeAudioMixProfile& profile) {
    const int centered = pan <= 1 ? static_cast<int>(pan) - 63
                                  : static_cast<int>(pan) - 64;
    return static_cast<float>(centered) * profile.PanScale;
}

float NormalizeNativeAudioSurroundPan(
    uint8_t surroundPan, const NativeAudioMixProfile& profile) {
    if (surroundPan <= 63) {
        return static_cast<float>(surroundPan) * profile.SurroundLowerScale;
    }
    return static_cast<float>(static_cast<uint16_t>(surroundPan) + 1) *
           profile.SurroundUpperScale;
}

namespace {

struct NativePanCurveSelection {
    uint8_t Family = 0;
    bool Normalize = false;
    bool ClampToOne = false;
};

NativePanCurveSelection SelectNativePanCurve(uint8_t panCurve) {
    NativePanCurveSelection result;
    switch (panCurve) {
        case 1: result.Normalize = true; break;
        case 2: result.Normalize = true; result.ClampToOne = true; break;
        case 3: result.Family = 1; break;
        case 4: result.Family = 1; result.Normalize = true; break;
        case 5:
            result.Family = 1;
            result.Normalize = true;
            result.ClampToOne = true;
            break;
        case 6: result.Family = 2; break;
        case 7: result.Family = 2; result.Normalize = true; break;
        case 8:
            result.Family = 2;
            result.Normalize = true;
            result.ClampToOne = true;
            break;
        default: break;
    }
    return result;
}

size_t NativeMixCurveIndex(float value, float maximum) {
    const float clamped = std::clamp(value, 0.0f, maximum);
    return static_cast<size_t>(std::clamp(
        static_cast<int>(0.5f + clamped * (256.0f / maximum)), 0, 256));
}

} // namespace

float CalculateNativeAudioPanGain(const NativeAudioMixProfile& profile,
                                  uint8_t panCurve, float pan,
                                  bool alternateCurves) {
    const auto selection = SelectNativePanCurve(panCurve);
    const auto& curve = alternateCurves
        ? profile.AlternatePanCurves[selection.Family]
        : profile.StandardPanCurves[selection.Family];
    float gain = curve[NativeMixCurveIndex(pan + 1.0f, 2.0f)];
    if (selection.Normalize) {
        gain /= curve[128];
    }
    return std::clamp(gain, 0.0f, selection.ClampToOne ? 1.0f : 2.0f);
}

float CalculateNativeAudioSurroundGain(
    const NativeAudioMixProfile& profile, uint8_t panCurve,
    float surroundPan) {
    const auto selection = SelectNativePanCurve(panCurve);
    return std::clamp(
        profile.StandardPanCurves[selection.Family][
            NativeMixCurveIndex(surroundPan, 2.0f)],
        0.0f, 2.0f);
}

NativeAudioChannelGains CalculateNativeAudioChannelGains(
    const NativeAudioMixProfile& profile, NativeAudioOutputMode outputMode,
    uint16_t sourceChannelCount, uint16_t sourceChannel,
    uint8_t panMode, uint8_t panCurve, float pan, float surroundPan) {
    const bool alternateCurves = outputMode == NativeAudioOutputMode::Surround;
    float left = 0.0f;
    float right = 0.0f;
    if (outputMode == NativeAudioOutputMode::Mono) {
        left = CalculateNativeAudioPanGain(
            profile, panCurve, 0.0f, alternateCurves);
        right = left;
    } else if (sourceChannelCount > 1 && panMode == 1) {
        if (sourceChannel == 0) {
            left = CalculateNativeAudioPanGain(
                profile, panCurve, pan, alternateCurves);
        } else if (sourceChannel == 1) {
            right = CalculateNativeAudioPanGain(
                profile, panCurve, -pan, alternateCurves);
        }
    } else {
        float effectivePan = pan;
        if (sourceChannelCount == 2) {
            effectivePan += sourceChannel == 0 ? -1.0f : 1.0f;
        }
        left = CalculateNativeAudioPanGain(
            profile, panCurve, effectivePan, alternateCurves);
        right = CalculateNativeAudioPanGain(
            profile, panCurve, -effectivePan, alternateCurves);
    }

    float front = 0.0f;
    float rear = 0.0f;
    if (outputMode == NativeAudioOutputMode::Surround) {
        front = CalculateNativeAudioSurroundGain(
            profile, panCurve, surroundPan);
        rear = CalculateNativeAudioSurroundGain(
            profile, panCurve, 2.0f - surroundPan);
    } else {
        front = CalculateNativeAudioSurroundGain(profile, panCurve, 0.0f);
        rear = CalculateNativeAudioSurroundGain(profile, panCurve, 2.0f);
    }
    return {front * left, front * right, rear * left, rear * right};
}

NativeAudioFilterProfile ParseNativeAudioFilterProfile(
    std::span<const uint8_t> codeBin,
    const NativeAudioFilterLayout& layout) {
    if (layout.CodeBaseAddress == 0 || layout.CutoffLookupCount == 0 ||
        layout.CosineLookupCount == 0 || layout.CosineLookupStride < 16) {
        throw std::runtime_error("native audio filter layout is incomplete");
    }
    NativeAudioFilterProfile profile;
    const auto readFloat = [&](uint32_t address, const char* role) {
        return NativeCodeFloatForBase(
            codeBin, layout.CodeBaseAddress, address, role);
    };
    profile.InputMinimum = readFloat(
        layout.InputMinimumAddress, "filter input minimum");
    profile.InputMaximum = readFloat(
        layout.InputMaximumAddress, "filter input maximum");
    profile.MinimumCutoffThreshold = readFloat(
        layout.MinimumCutoffThresholdAddress, "filter minimum threshold");
    profile.MaximumCutoffThreshold = readFloat(
        layout.MaximumCutoffThresholdAddress, "filter maximum threshold");
    profile.CutoffLookupBase = readFloat(
        layout.CutoffLookupBaseAddress, "filter lookup base");
    profile.CutoffLookupScale = readFloat(
        layout.CutoffLookupScaleAddress, "filter lookup scale");
    profile.FrequencyScale = readFloat(
        layout.FrequencyScaleAddress, "filter frequency scale");
    profile.FilterOffset = readFloat(
        layout.FilterOffsetAddress, "filter offset");
    profile.FilterUnit = readFloat(
        layout.FilterUnitAddress, "filter unit");
    profile.CoefficientScale = readFloat(
        layout.CoefficientScaleAddress, "filter coefficient scale");
    profile.CosinePeriod = readFloat(
        layout.CosinePeriodAddress, "filter cosine period");

    const uint32_t cutoffAddress = NativeCodeU32ForBase(
        codeBin, layout.CodeBaseAddress, layout.CutoffLookupPointerAddress,
        "filter cutoff lookup");
    const size_t cutoffOffset = NativeCodeOffsetForBase(
        codeBin, layout.CodeBaseAddress, cutoffAddress,
        layout.CutoffLookupCount * sizeof(uint16_t), "filter cutoff lookup");
    profile.CutoffFrequencies.reserve(layout.CutoffLookupCount);
    for (uint32_t index = 0; index < layout.CutoffLookupCount; ++index) {
        const size_t offset = cutoffOffset + index * sizeof(uint16_t);
        profile.CutoffFrequencies.push_back(static_cast<uint16_t>(
            codeBin[offset] | static_cast<uint16_t>(codeBin[offset + 1] << 8)));
    }

    const uint32_t cosineAddress = NativeCodeU32ForBase(
        codeBin, layout.CodeBaseAddress, layout.CosineLookupPointerAddress,
        "filter cosine lookup");
    NativeCodeOffsetForBase(
        codeBin, layout.CodeBaseAddress, cosineAddress,
        static_cast<size_t>(layout.CosineLookupCount) *
            layout.CosineLookupStride,
        "filter cosine lookup");
    profile.CosineLookup.reserve(layout.CosineLookupCount);
    for (uint32_t index = 0; index < layout.CosineLookupCount; ++index) {
        const uint32_t entryAddress = cosineAddress +
            index * layout.CosineLookupStride;
        profile.CosineLookup.push_back({
            readFloat(entryAddress + 4, "filter cosine value"),
            readFloat(entryAddress + 12, "filter cosine slope"),
        });
    }

    const bool orderedCutoffs = std::is_sorted(
        profile.CutoffFrequencies.begin(), profile.CutoffFrequencies.end());
    if (profile.InputMinimum != 0.0f || profile.InputMaximum != 1.0f ||
        profile.MinimumCutoffThreshold <= profile.InputMinimum ||
        profile.MaximumCutoffThreshold >= profile.InputMaximum ||
        profile.MinimumCutoffThreshold >= profile.MaximumCutoffThreshold ||
        profile.CutoffLookupBase != profile.MinimumCutoffThreshold ||
        profile.CutoffLookupScale <= 0.0f ||
        profile.FrequencyScale <= 0.0f || profile.FilterOffset != 2.0f ||
        profile.FilterUnit != 1.0f || profile.CoefficientScale != 32768.0f ||
        profile.CosinePeriod <= 0.0f || !orderedCutoffs ||
        profile.CutoffFrequencies.front() != 80 ||
        profile.CutoffFrequencies.back() != 12800 ||
        std::abs(profile.CosineLookup.front().Value - 1.0f) > 0.000001f ||
        std::abs(profile.CosineLookup[128].Value + 1.0f) > 0.000001f) {
        throw std::runtime_error("native audio filter profile is invalid");
    }
    return profile;
}

uint16_t CalculateNativeAudioCutoffFrequency(
    const NativeAudioFilterProfile& profile, float cutoff) {
    cutoff = std::clamp(cutoff, profile.InputMinimum, profile.InputMaximum);
    if (cutoff < profile.MinimumCutoffThreshold) {
        return 80;
    }
    if (cutoff >= profile.MaximumCutoffThreshold) {
        return 16000;
    }
    const size_t index = static_cast<size_t>(std::clamp(
        static_cast<int>((cutoff - profile.CutoffLookupBase) *
                         profile.CutoffLookupScale),
        0, static_cast<int>(profile.CutoffFrequencies.size() - 1)));
    return profile.CutoffFrequencies[index];
}

NativeAudioSimpleFilterCoefficients CalculateNativeAudioSimpleFilter(
    const NativeAudioFilterProfile& profile, uint16_t frequency) {
    frequency = std::min<uint16_t>(frequency, 16000);
    float phase = static_cast<float>(frequency) * profile.FrequencyScale;
    phase = std::abs(phase);
    while (phase >= profile.CosinePeriod) {
        phase -= profile.CosinePeriod;
    }
    const uint32_t integerPhase = static_cast<uint32_t>(phase);
    const auto& cosineEntry = profile.CosineLookup[
        integerPhase & static_cast<uint32_t>(profile.CosineLookup.size() - 1)];
    const float cosine = cosineEntry.Value +
        (phase - static_cast<float>(integerPhase)) * cosineEntry.Slope;
    const float offset = profile.FilterOffset - cosine;
    const float feedback = std::sqrt(
        offset * offset - profile.FilterUnit) - offset;
    return {
        static_cast<int16_t>(static_cast<int32_t>(
            (feedback + profile.FilterUnit) * profile.CoefficientScale)),
        static_cast<int16_t>(-static_cast<int32_t>(
            feedback * profile.CoefficientScale)),
    };
}

uint16_t NativeAudioEnvelopeProfile::HoldTicks(uint8_t code) const {
    const uint32_t side = static_cast<uint32_t>(code) + 1;
    return static_cast<uint16_t>((side * side) / 4);
}

float NativeAudioEnvelopeProfile::DecayStep(uint8_t code) const {
    return EnvelopeRateForCode(Decay, code);
}

float NativeAudioEnvelopeProfile::ReleaseStep(uint8_t code) const {
    return EnvelopeRateForCode(Release, code);
}

float NativeAudioEnvelopeProfile::GainForLevel(float level) const {
    return GainForDb(level * LevelToDbScale);
}

float NativeAudioEnvelopeProfile::GainForDb(float db) const {
    if (GainCurve.empty() || GainIndexScale <= 0.0f) {
        return 0.0f;
    }
    db = std::clamp(db, GainMinDb, GainMaxDb);
    const long index = std::lround((db - GainMinDb) * GainIndexScale);
    return GainCurve[static_cast<size_t>(std::clamp(
        index, 0l, static_cast<long>(GainCurve.size() - 1)))];
}

NativeAudioEnvelopeProfile ParseNativeAudioEnvelopeProfile(
    std::span<const uint8_t> codeBin,
    const NativeAudioEnvelopeLayout& layout) {
    if (layout.CodeBaseAddress == 0 || layout.GainTableCount == 0) {
        throw std::runtime_error("native audio envelope layout is incomplete");
    }

    NativeAudioEnvelopeProfile profile;
    profile.FloorDb = NativeCodeFloat(codeBin, layout, layout.FloorDbAddress);
    profile.LevelToDbScale = NativeCodeFloat(
        codeBin, layout, layout.LevelToDbScaleAddress);
    profile.GainMinDb = NativeCodeFloat(
        codeBin, layout, layout.GainMinDbAddress);
    profile.GainMaxDb = NativeCodeFloat(
        codeBin, layout, layout.GainMaxDbAddress);
    profile.GainIndexScale = NativeCodeFloat(
        codeBin, layout, layout.GainIndexScaleAddress);
    profile.Decay = ParseEnvelopeRate(
        codeBin, layout, layout.DecayImmediateAddress,
        layout.DecayNearImmediateAddress, layout.DecayScaleAddress,
        layout.DecayLowSlopeAddress, layout.DecayHighNumeratorAddress);
    profile.Release = ParseEnvelopeRate(
        codeBin, layout, layout.ReleaseImmediateAddress,
        layout.ReleaseNearImmediateAddress, layout.ReleaseScaleAddress,
        layout.ReleaseLowSlopeAddress, layout.ReleaseHighNumeratorAddress);

    const size_t sustainOffset = NativeCodeOffset(
        codeBin, layout, layout.SustainTableAddress,
        profile.SustainLevels.size() * sizeof(int16_t));
    for (size_t index = 0; index < profile.SustainLevels.size(); ++index) {
        profile.SustainLevels[index] = static_cast<int16_t>(
            static_cast<uint16_t>(codeBin[sustainOffset + index * 2]) |
            static_cast<uint16_t>(codeBin[sustainOffset + index * 2 + 1] << 8));
    }
    for (size_t index = 0; index < profile.AttackMultipliers.size(); ++index) {
        profile.AttackMultipliers[index] = NativeCodeFloat(
            codeBin, layout,
            layout.AttackTableAddress + static_cast<uint32_t>(index * 4));
    }
    profile.GainCurve.reserve(layout.GainTableCount);
    for (uint32_t index = 0; index < layout.GainTableCount; ++index) {
        profile.GainCurve.push_back(NativeCodeFloat(
            codeBin, layout, layout.GainTableAddress + index * 4));
    }

    if (profile.LevelToDbScale <= 0.0f || profile.GainMinDb >= profile.GainMaxDb ||
        profile.GainIndexScale <= 0.0f ||
        std::abs(profile.FloorDb - profile.GainMinDb) > 0.001f) {
        throw std::runtime_error("native audio envelope profile is invalid");
    }
    return profile;
}

} // namespace Oot3dNativeGame
