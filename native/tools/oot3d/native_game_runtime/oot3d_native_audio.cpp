// DSP-ADPCM decoding arithmetic is derived from Azahar/Citra audio_core/codec.cpp.
// Copyright 2016 Citra Emulator Project. Licensed under GPLv2 or any later version.
// Native audio: BCWAV, BCSTM and BCSAR decoding and sequence resolution. Topics:
// oot3d_native_audio_profiles.cpp (envelope, modulation, mix and filter profiles from code.bin),
// oot3d_native_audio_mixer.cpp (the sequence mixer).
#include "oot3d_native_audio_internal.h"

namespace Oot3dNativeGame {

namespace {

class Reader {
  public:
    explicit Reader(std::span<const uint8_t> bytes) : mBytes(bytes) {}

    void Require(size_t offset, size_t size, const char* role) const {
        if (offset > mBytes.size() || size > mBytes.size() - offset) {
            throw std::runtime_error(std::string("truncated native audio ") + role);
        }
    }

    uint8_t U8(size_t offset) const {
        Require(offset, 1, "u8");
        return mBytes[offset];
    }

    uint16_t U16(size_t offset) const {
        Require(offset, 2, "u16");
        return static_cast<uint16_t>(mBytes[offset]) |
               static_cast<uint16_t>(mBytes[offset + 1] << 8);
    }

    int16_t S16(size_t offset) const {
        return static_cast<int16_t>(U16(offset));
    }

    uint32_t U32(size_t offset) const {
        Require(offset, 4, "u32");
        return static_cast<uint32_t>(mBytes[offset]) |
               (static_cast<uint32_t>(mBytes[offset + 1]) << 8) |
               (static_cast<uint32_t>(mBytes[offset + 2]) << 16) |
               (static_cast<uint32_t>(mBytes[offset + 3]) << 24);
    }

    float F32(size_t offset) const {
        return std::bit_cast<float>(U32(offset));
    }

    std::span<const uint8_t> Slice(size_t offset, size_t size) const {
        Require(offset, size, "slice");
        return mBytes.subspan(offset, size);
    }

    bool Magic(size_t offset, const char (&magic)[5]) const {
        Require(offset, 4, "magic");
        return std::memcmp(mBytes.data() + offset, magic, 4) == 0;
    }

  private:
    std::span<const uint8_t> mBytes;
};

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("failed to open native audio file: " + path.string());
    }
    const auto length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("failed to size native audio file: " + path.string());
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), length)) {
        throw std::runtime_error("failed to read native audio file: " + path.string());
    }
    return bytes;
}

size_t CheckedAdd(size_t lhs, uint32_t rhs, const char* role) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        throw std::runtime_error(std::string("native audio offset overflow: ") + role);
    }
    return lhs + rhs;
}

std::string ReadCString(const Reader& reader, size_t offset, uint32_t size) {
    const auto bytes = reader.Slice(offset, size);
    const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0});
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<size_t>(end - bytes.begin()));
}

struct BcwavChannel {
    size_t SampleOffset = 0;
    std::array<int16_t, 16> Coefficients{};
    int16_t History1 = 0;
    int16_t History2 = 0;
    int16_t ImaStepIndex = 0;
};

struct BankNote {
    uint32_t WaveArchive = 0;
    uint32_t Wave = 0;
    uint32_t RootKey = 60;
    uint32_t Volume = 127;
    uint32_t Pan = 64;
    float Pitch = 1.0f;
    int8_t SurroundPan = 0;
    uint8_t InterpolationType = 0;
    bool IgnoreNoteOff = false;
    uint8_t Attack = 127;
    uint8_t Decay = 127;
    uint8_t Sustain = 127;
    uint8_t Hold = 127;
    uint8_t Release = 127;
};

std::optional<size_t> ResolveBankVariant(const Reader& reader, size_t reference,
                                         uint8_t selector) {
    const uint32_t type = reader.U32(reference);
    const size_t data = CheckedAdd(reference, reader.U32(reference + 4),
                                   "CBNK variant data");
    if (type == 0x6000) {
        return CheckedAdd(data, reader.U32(data + 4), "CBNK direct region");
    }
    if (type == 0x6001) {
        const uint32_t rangeCount = reader.U32(data);
        const size_t rangeEnds = data + 4;
        for (uint32_t index = 0; index < rangeCount; ++index) {
            if (selector <= reader.U8(rangeEnds + index)) {
                const size_t references = rangeEnds + ((rangeCount + 3) & ~uint32_t{3});
                return CheckedAdd(data, reader.U32(references + index * 8 + 4),
                                  "CBNK range region");
            }
        }
        return std::nullopt;
    }
    if (type == 0x6002) {
        const uint8_t start = reader.U8(data);
        const uint8_t end = reader.U8(data + 1);
        if (selector < start || selector > end) {
            return std::nullopt;
        }
        const size_t referenceOffset = data + 4 +
            static_cast<size_t>(selector - start) * 8;
        return CheckedAdd(data, reader.U32(referenceOffset + 4),
                          "CBNK indexed region");
    }
    throw std::runtime_error("unsupported native CBNK resource variant");
}

std::optional<BankNote> ResolveBankNote(std::span<const uint8_t> bytes,
                                        uint8_t program, uint8_t note,
                                        uint8_t velocity) {
    Reader reader(bytes);
    if (!reader.Magic(0, "CBNK") || reader.U16(4) != 0xfeff) {
        throw std::runtime_error("unsupported native CBNK header");
    }
    const size_t info = reader.U32(24);
    if (!reader.Magic(info, "INFO") || reader.U32(info + 8) != 0x100 ||
        reader.U32(info + 16) != 0x101) {
        throw std::runtime_error("invalid native CBNK INFO section");
    }
    const size_t waveTable = CheckedAdd(info + 8, reader.U32(info + 12), "CBNK waves");
    const size_t instrumentTable = CheckedAdd(info + 8, reader.U32(info + 20),
                                              "CBNK instruments");
    const uint32_t instrumentCount = reader.U32(instrumentTable);
    if (program >= instrumentCount) {
        return std::nullopt;
    }
    const size_t instrumentRef = instrumentTable + 4 + static_cast<size_t>(program) * 8;
    if (reader.U32(instrumentRef) != 0x5900) {
        return std::nullopt;
    }
    const size_t instrument = CheckedAdd(info + 24, reader.U32(instrumentRef + 4),
                                         "CBNK instrument");
    const auto noteOffset = ResolveBankVariant(reader, instrument, note);
    if (!noteOffset.has_value()) {
        return std::nullopt;
    }
    const auto waveMapField = ResolveBankVariant(reader, *noteOffset, velocity);
    if (!waveMapField.has_value()) {
        return std::nullopt;
    }
    const uint32_t waveMap = reader.U32(*waveMapField);
    const uint32_t waveCount = reader.U32(waveTable);
    if (waveMap >= waveCount) {
        throw std::runtime_error("native CBNK wave map is out of range");
    }
    const size_t waveReference = waveTable + 4 + static_cast<size_t>(waveMap) * 8;

    BankNote result;
    const uint32_t packedArchive = reader.U32(waveReference);
    if ((packedArchive & 0xff000000) != 0x05000000) {
        throw std::runtime_error("invalid native CBNK wave archive reference");
    }
    result.WaveArchive = packedArchive & 0x00ffffff;
    result.Wave = reader.U32(waveReference + 4);
    const uint32_t flagMask = reader.U32(*waveMapField + 4);
    std::array<std::optional<uint32_t>, 32> flags;
    size_t flagCursor = *waveMapField + 8;
    for (uint8_t bit = 0; bit < flags.size(); ++bit) {
        if ((flagMask & (uint32_t{1} << bit)) != 0) {
            flags[bit] = reader.U32(flagCursor);
            flagCursor += 4;
        }
    }
    result.RootKey = flags[0].value_or(60) & 0xff;
    result.Volume = flags[1].value_or(127) & 0xff;
    const uint32_t packedPan = flags[2].value_or(64);
    result.Pan = packedPan & 0xff;
    result.SurroundPan = static_cast<int8_t>((packedPan >> 8) & 0xff);
    result.Pitch = std::bit_cast<float>(flags[3].value_or(0x3f800000));
    const uint32_t noteParameters = flags[4].value_or(0);
    result.IgnoreNoteOff = (noteParameters & 0xff) != 0;
    result.InterpolationType = static_cast<uint8_t>(
        (noteParameters >> 16) & 0xff);
    if (flags[9].has_value() && *flags[9] != 0xffffffff) {
        const size_t envelopeReference = CheckedAdd(
            *waveMapField, *flags[9], "CBNK ADSHR reference");
        const size_t envelope = CheckedAdd(
            envelopeReference, reader.U32(envelopeReference + 4),
            "CBNK ADSHR values");
        result.Attack = reader.U8(envelope);
        result.Decay = reader.U8(envelope + 1);
        result.Sustain = reader.U8(envelope + 2);
        result.Hold = reader.U8(envelope + 3);
        result.Release = reader.U8(envelope + 4);
    }
    return result;
}

std::span<const uint8_t> ResolveWaveFromArchive(std::span<const uint8_t> bytes,
                                                uint32_t waveIndex) {
    Reader reader(bytes);
    if (!reader.Magic(0, "CWAR") || reader.U16(4) != 0xfeff) {
        throw std::runtime_error("unsupported native BCWAR header");
    }
    size_t info = 0;
    size_t files = 0;
    for (size_t index = 0; index < reader.U32(16); ++index) {
        const size_t reference = 20 + index * 12;
        if (reader.U16(reference) == 0x6800) {
            info = reader.U32(reference + 4);
        } else if (reader.U16(reference) == 0x6801) {
            files = reader.U32(reference + 4);
        }
    }
    if (info == 0 || files == 0 || !reader.Magic(info, "INFO") ||
        !reader.Magic(files, "FILE")) {
        throw std::runtime_error("native BCWAR has incomplete sections");
    }
    const uint32_t count = reader.U32(info + 8);
    if (waveIndex >= count) {
        throw std::runtime_error("native BCWAR wave index is out of range");
    }
    const size_t reference = info + 12 + static_cast<size_t>(waveIndex) * 12;
    if (reader.U16(reference) != 0x1f00) {
        throw std::runtime_error("invalid native BCWAR wave reference");
    }
    return reader.Slice(CheckedAdd(files + 8, reader.U32(reference + 4), "BCWAR wave"),
                        reader.U32(reference + 8));
}

std::vector<int16_t> DecodeDspAdpcm(const Reader& reader, const BcwavChannel& channel,
                                    uint32_t sampleCount) {
    constexpr size_t kFrameBytes = 8;
    constexpr uint32_t kSamplesPerFrame = 14;
    constexpr std::array<int, 16> kSignedNibbles{
        0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1,
    };
    std::vector<int16_t> result;
    result.reserve(sampleCount);
    int history1 = channel.History1;
    int history2 = channel.History2;
    const uint32_t frameCount = (sampleCount + kSamplesPerFrame - 1) / kSamplesPerFrame;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const size_t frameOffset = channel.SampleOffset + frame * kFrameBytes;
        const uint8_t header = reader.U8(frameOffset);
        const int scale = 1 << (header & 0x0f);
        const size_t coefficient = static_cast<size_t>((header >> 4) & 7) * 2;
        const int coefficient1 = channel.Coefficients[coefficient];
        const int coefficient2 = channel.Coefficients[coefficient + 1];
        for (uint32_t sample = 0; sample < kSamplesPerFrame && result.size() < sampleCount;
             ++sample) {
            const uint8_t packed = reader.U8(frameOffset + 1 + sample / 2);
            const uint8_t nibble = sample % 2 == 0 ? packed >> 4 : packed & 0x0f;
            const int input = kSignedNibbles[nibble] * scale;
            int value = ((input << 11) + 0x400 + coefficient1 * history1 +
                         coefficient2 * history2) >> 11;
            value = std::clamp(value, -32768, 32767);
            history2 = history1;
            history1 = value;
            result.push_back(static_cast<int16_t>(value));
        }
    }
    return result;
}

std::vector<int16_t> DecodeImaAdpcm(const Reader& reader, const BcwavChannel& channel,
                                    uint32_t sampleCount) {
    constexpr std::array<int, 89> kStepSizes{
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
        143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
        494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
        4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767,
    };
    constexpr std::array<int, 16> kIndexAdjustments{
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    };
    if (channel.ImaStepIndex < 0 || channel.ImaStepIndex >= kStepSizes.size()) {
        throw std::runtime_error("invalid native BCWAV IMA step index");
    }

    std::vector<int16_t> result;
    result.reserve(sampleCount);
    int history = channel.History1;
    int stepIndex = channel.ImaStepIndex;
    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
        const uint8_t packed = reader.U8(channel.SampleOffset + sample / 2);
        const uint8_t code = sample % 2 == 0 ? packed & 0x0f : packed >> 4;
        const int step = kStepSizes[stepIndex];
        int delta = step >> 3;
        if ((code & 1) != 0) {
            delta += step >> 2;
        }
        if ((code & 2) != 0) {
            delta += step >> 1;
        }
        if ((code & 4) != 0) {
            delta += step;
        }
        history = std::clamp(history + ((code & 8) != 0 ? -delta : delta),
                             -32768, 32767);
        stepIndex = std::clamp(stepIndex + kIndexAdjustments[code], 0, 88);
        result.push_back(static_cast<int16_t>(history));
    }
    return result;
}

std::vector<int16_t> DecodeDspAdpcmBlock(
    const Reader& reader, size_t sampleOffset, uint32_t sampleCount,
    const std::array<int16_t, 16>& coefficients, int16_t initialHistory1,
    int16_t initialHistory2) {
    BcwavChannel channel;
    channel.SampleOffset = sampleOffset;
    channel.Coefficients = coefficients;
    channel.History1 = initialHistory1;
    channel.History2 = initialHistory2;
    return DecodeDspAdpcm(reader, channel, sampleCount);
}

} // namespace

NativeBcwav DecodeNativeBcwav(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    if (!reader.Magic(0, "CWAV") || reader.U16(4) != 0xfeff || reader.U16(6) != 0x40) {
        throw std::runtime_error("unsupported native BCWAV header");
    }
    if (reader.U32(12) != bytes.size() || reader.U32(16) != 2) {
        throw std::runtime_error("inconsistent native BCWAV size or section count");
    }

    size_t infoOffset = 0;
    size_t dataOffset = 0;
    for (size_t index = 0; index < 2; ++index) {
        const size_t reference = 20 + index * 12;
        const uint16_t type = reader.U16(reference);
        const uint32_t offset = reader.U32(reference + 4);
        if (type == 0x7000) {
            infoOffset = offset;
        } else if (type == 0x7001) {
            dataOffset = offset;
        }
    }
    if (infoOffset == 0 || dataOffset == 0 || !reader.Magic(infoOffset, "INFO") ||
        !reader.Magic(dataOffset, "DATA")) {
        throw std::runtime_error("native BCWAV has no INFO/DATA sections");
    }

    NativeBcwav wave;
    wave.Codec = static_cast<NativeAudioCodec>(reader.U8(infoOffset + 8));
    wave.Looping = (reader.U8(infoOffset + 9) & 1) != 0;
    wave.SampleRate = reader.U32(infoOffset + 12);
    wave.LoopStart = reader.U32(infoOffset + 16);
    wave.SampleCount = reader.U32(infoOffset + 20);
    wave.ChannelCount = reader.U16(infoOffset + 28);
    if (wave.SampleRate == 0 || wave.SampleCount == 0 || wave.ChannelCount == 0 ||
        wave.ChannelCount > 2 || wave.LoopStart > wave.SampleCount) {
        throw std::runtime_error("invalid native BCWAV stream metadata");
    }
    std::vector<BcwavChannel> channels;
    channels.reserve(wave.ChannelCount);
    for (uint16_t index = 0; index < wave.ChannelCount; ++index) {
        const size_t reference = infoOffset + 32 + index * 8;
        if (reader.U16(reference) != 0x7100) {
            throw std::runtime_error("invalid native BCWAV channel reference");
        }
        const size_t channelOffset = CheckedAdd(infoOffset + 28, reader.U32(reference + 4),
                                                "BCWAV channel");
        if (reader.U16(channelOffset) != 0x1f00) {
            throw std::runtime_error("invalid native BCWAV sample reference");
        }
        BcwavChannel channel;
        channel.SampleOffset = CheckedAdd(dataOffset + 8, reader.U32(channelOffset + 4),
                                          "BCWAV samples");
        if (wave.Codec == NativeAudioCodec::DspAdpcm) {
            if (reader.U16(channelOffset + 8) != 0x0300) {
                throw std::runtime_error("invalid native BCWAV DSP-ADPCM reference");
            }
            const size_t adpcmOffset = CheckedAdd(channelOffset, reader.U32(channelOffset + 12),
                                                  "BCWAV ADPCM info");
            for (size_t coefficient = 0; coefficient < channel.Coefficients.size(); ++coefficient) {
                channel.Coefficients[coefficient] = reader.S16(adpcmOffset + coefficient * 2);
            }
            channel.History1 = reader.S16(adpcmOffset + 34);
            channel.History2 = reader.S16(adpcmOffset + 36);
        } else if (wave.Codec == NativeAudioCodec::ImaAdpcm) {
            if (reader.U16(channelOffset + 8) != 0x0301) {
                throw std::runtime_error("invalid native BCWAV IMA-ADPCM reference");
            }
            const size_t adpcmOffset = CheckedAdd(channelOffset, reader.U32(channelOffset + 12),
                                                  "BCWAV IMA-ADPCM info");
            channel.History1 = reader.S16(adpcmOffset);
            channel.ImaStepIndex = reader.S16(adpcmOffset + 2);
        }
        channels.push_back(channel);
    }

    std::vector<std::vector<int16_t>> decoded;
    decoded.reserve(channels.size());
    for (const auto& channel : channels) {
        std::vector<int16_t> samples;
        samples.reserve(wave.SampleCount);
        if (wave.Codec == NativeAudioCodec::DspAdpcm) {
            samples = DecodeDspAdpcm(reader, channel, wave.SampleCount);
        } else if (wave.Codec == NativeAudioCodec::ImaAdpcm) {
            samples = DecodeImaAdpcm(reader, channel, wave.SampleCount);
        } else if (wave.Codec == NativeAudioCodec::Pcm16) {
            for (uint32_t index = 0; index < wave.SampleCount; ++index) {
                samples.push_back(reader.S16(channel.SampleOffset + index * 2));
            }
        } else if (wave.Codec == NativeAudioCodec::Pcm8) {
            for (uint32_t index = 0; index < wave.SampleCount; ++index) {
                samples.push_back(static_cast<int16_t>(
                    static_cast<int8_t>(reader.U8(channel.SampleOffset + index))) * 256);
            }
        } else {
            throw std::runtime_error("unknown native BCWAV codec");
        }
        decoded.push_back(std::move(samples));
    }

    wave.InterleavedSamples.reserve(static_cast<size_t>(wave.SampleCount) * wave.ChannelCount);
    for (uint32_t sample = 0; sample < wave.SampleCount; ++sample) {
        for (const auto& channel : decoded) {
            wave.InterleavedSamples.push_back(channel[sample]);
        }
    }
    return wave;
}

NativeBcwav DecodeNativeBcwavFile(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path);
    return DecodeNativeBcwav(bytes);
}

NativeBcwav DecodeNativeBcstm(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    if (!reader.Magic(0, "CSTM") || reader.U16(4) != 0xfeff || reader.U16(6) != 0x40 ||
        reader.U32(12) != bytes.size()) {
        throw std::runtime_error("unsupported native BCSTM header");
    }
    size_t info = 0;
    size_t seek = 0;
    size_t data = 0;
    for (size_t index = 0; index < reader.U16(16); ++index) {
        const size_t reference = 20 + index * 12;
        const uint16_t type = reader.U16(reference);
        if (type == 0x4000) {
            info = reader.U32(reference + 4);
        } else if (type == 0x4001) {
            seek = reader.U32(reference + 4);
        } else if (type == 0x4002) {
            data = reader.U32(reference + 4);
        }
    }
    if (info == 0 || seek == 0 || data == 0 || !reader.Magic(info, "INFO") ||
        !reader.Magic(seek, "SEEK") || !reader.Magic(data, "DATA")) {
        throw std::runtime_error("native BCSTM has incomplete sections");
    }
    const size_t infoBase = info + 8;
    const size_t streamInfo = CheckedAdd(infoBase, reader.U32(infoBase + 4),
                                         "BCSTM stream info");
    const size_t channelTable = CheckedAdd(infoBase, reader.U32(infoBase + 20),
                                           "BCSTM channel table");

    NativeBcwav stream;
    stream.Codec = static_cast<NativeAudioCodec>(reader.U8(streamInfo));
    stream.Looping = reader.U8(streamInfo + 1) != 0;
    stream.ChannelCount = reader.U8(streamInfo + 2);
    stream.SampleRate = reader.U32(streamInfo + 4);
    stream.LoopStart = reader.U32(streamInfo + 8);
    const uint32_t declaredLoopEnd = reader.U32(streamInfo + 12);
    const uint32_t blockCount = reader.U32(streamInfo + 16);
    const uint32_t blockSize = reader.U32(streamInfo + 20);
    const uint32_t blockSamples = reader.U32(streamInfo + 24);
    const uint32_t lastBlockSize = reader.U32(streamInfo + 28);
    const uint32_t lastBlockSamples = reader.U32(streamInfo + 32);
    const uint32_t lastBlockPaddedSize = reader.U32(streamInfo + 36);
    const uint32_t seekEntrySize = reader.U32(streamInfo + 40);
    const uint32_t sampleDataOffset = reader.U32(streamInfo + 52);
    if (stream.Codec != NativeAudioCodec::DspAdpcm || stream.ChannelCount == 0 ||
        stream.ChannelCount > 2 || stream.SampleRate == 0 || blockCount == 0 ||
        blockSize == 0 || blockSamples == 0 || lastBlockSize == 0 ||
        lastBlockSamples == 0 || seekEntrySize < 4) {
        throw std::runtime_error("unsupported native BCSTM stream metadata");
    }
    const uint64_t expectedSamples = static_cast<uint64_t>(blockCount - 1) * blockSamples +
                                     lastBlockSamples;
    if (expectedSamples > std::numeric_limits<uint32_t>::max() ||
        stream.LoopStart >= expectedSamples || declaredLoopEnd < expectedSamples) {
        throw std::runtime_error("native BCSTM loop/block sample range is inconsistent");
    }
    stream.SampleCount = static_cast<uint32_t>(expectedSamples);

    std::vector<std::array<int16_t, 16>> coefficients(stream.ChannelCount);
    if (reader.U32(channelTable) != stream.ChannelCount) {
        throw std::runtime_error("native BCSTM channel table count is inconsistent");
    }
    for (uint16_t channel = 0; channel < stream.ChannelCount; ++channel) {
        const size_t channelReference = channelTable + 4 + static_cast<size_t>(channel) * 8;
        const size_t channelInfo = CheckedAdd(channelTable,
            reader.U32(channelReference + 4), "BCSTM channel info");
        const size_t codecInfo = CheckedAdd(channelInfo,
            reader.U32(channelInfo + 4), "BCSTM DSP info");
        for (size_t coefficient = 0; coefficient < 16; ++coefficient) {
            coefficients[channel][coefficient] = reader.S16(codecInfo + coefficient * 2);
        }
    }

    std::vector<std::vector<int16_t>> decoded(stream.ChannelCount);
    for (auto& channel : decoded) {
        channel.reserve(stream.SampleCount);
    }
    size_t payload = CheckedAdd(data + 8, sampleDataOffset, "BCSTM sample payload");
    for (uint32_t block = 0; block < blockCount; ++block) {
        const bool last = block + 1 == blockCount;
        const uint32_t samples = last ? lastBlockSamples : blockSamples;
        const uint32_t storedSize = last ? lastBlockPaddedSize : blockSize;
        for (uint16_t channel = 0; channel < stream.ChannelCount; ++channel) {
            const size_t seekEntry = seek + 8 +
                (static_cast<size_t>(block) * stream.ChannelCount + channel) * seekEntrySize;
            auto blockSamplesDecoded = DecodeDspAdpcmBlock(
                reader, payload, samples, coefficients[channel],
                reader.S16(seekEntry), reader.S16(seekEntry + 2));
            decoded[channel].insert(decoded[channel].end(),
                                    blockSamplesDecoded.begin(), blockSamplesDecoded.end());
            payload += storedSize;
        }
    }
    stream.InterleavedSamples.reserve(static_cast<size_t>(stream.SampleCount) *
                                      stream.ChannelCount);
    for (uint32_t sample = 0; sample < stream.SampleCount; ++sample) {
        for (const auto& channel : decoded) {
            stream.InterleavedSamples.push_back(channel[sample]);
        }
    }
    return stream;
}

NativeBcwav DecodeNativeBcstmFile(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path);
    return DecodeNativeBcstm(bytes);
}

NativeBcsarCatalog ParseNativeBcsar(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    if (!reader.Magic(0, "CSAR") || reader.U16(4) != 0xfeff || reader.U16(6) != 0x40) {
        throw std::runtime_error("unsupported native BCSAR header");
    }
    if (reader.U32(12) != bytes.size() || reader.U32(16) < 3) {
        throw std::runtime_error("inconsistent native BCSAR size or section count");
    }

    size_t stringSection = 0;
    size_t infoSection = 0;
    NativeBcsarCatalog catalog;
    for (size_t index = 0; index < reader.U32(16); ++index) {
        const size_t reference = 20 + index * 12;
        const uint16_t type = reader.U16(reference);
        const uint32_t offset = reader.U32(reference + 4);
        if (type == 0x2000) {
            stringSection = offset;
        } else if (type == 0x2001) {
            infoSection = offset;
        } else if (type == 0x2002) {
            catalog.FileSectionOffset = offset;
        }
    }
    if (stringSection == 0 || infoSection == 0 || catalog.FileSectionOffset == 0 ||
        !reader.Magic(stringSection, "STRG") || !reader.Magic(infoSection, "INFO") ||
        !reader.Magic(catalog.FileSectionOffset, "FILE")) {
        throw std::runtime_error("native BCSAR has incomplete sections");
    }

    size_t stringTable = 0;
    for (size_t index = 0; index < 2; ++index) {
        const size_t reference = stringSection + 8 + index * 8;
        if (reader.U16(reference) == 0x2400) {
            stringTable = CheckedAdd(reference, reader.U32(reference + 4), "BCSAR strings");
        }
    }
    if (stringTable == 0) {
        throw std::runtime_error("native BCSAR has no string table");
    }
    const uint32_t stringCount = reader.U32(stringTable);
    catalog.Strings.reserve(stringCount);
    for (uint32_t index = 0; index < stringCount; ++index) {
        const size_t reference = stringTable + 4 + index * 12;
        if (reader.U16(reference) != 0x1f01) {
            throw std::runtime_error("invalid native BCSAR string reference");
        }
        const uint32_t size = reader.U32(reference + 8);
        const size_t offset = CheckedAdd(stringTable, reader.U32(reference + 4), "BCSAR string");
        catalog.Strings.push_back(ReadCString(reader, offset, size));
    }

    size_t soundTable = 0;
    size_t bankTable = 0;
    size_t waveArchiveTable = 0;
    size_t playerTable = 0;
    size_t fileTable = 0;
    for (size_t index = 0; index < 8; ++index) {
        const size_t reference = infoSection + 8 + index * 8;
        const uint16_t type = reader.U16(reference);
        const size_t offset = CheckedAdd(infoSection + 8, reader.U32(reference + 4),
                                         "BCSAR INFO table");
        if (type == 0x2100) {
            soundTable = offset;
        } else if (type == 0x2101) {
            bankTable = offset;
        } else if (type == 0x2103) {
            waveArchiveTable = offset;
        } else if (type == 0x2102) {
            playerTable = offset;
        } else if (type == 0x2106) {
            fileTable = offset;
        }
    }
    const auto parseOffsets = [&reader](size_t table, uint16_t expectedType) {
        std::vector<size_t> offsets;
        const uint32_t count = reader.U32(table);
        offsets.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            const size_t reference = table + 4 + index * 8;
            if (reader.U16(reference) != expectedType) {
                throw std::runtime_error("invalid native BCSAR INFO entry reference");
            }
            offsets.push_back(CheckedAdd(table, reader.U32(reference + 4), "BCSAR INFO entry"));
        }
        return offsets;
    };

    if (fileTable == 0 || soundTable == 0 || bankTable == 0 ||
        waveArchiveTable == 0 || playerTable == 0) {
        throw std::runtime_error("native BCSAR has incomplete sound resource tables");
    }
    const auto fileOffsets = parseOffsets(fileTable, 0x220a);
    catalog.Files.reserve(fileOffsets.size());
    for (const size_t offset : fileOffsets) {
        NativeBcsarFileEntry file;
        const uint16_t type = reader.U16(offset);
        if (type == 0x220c) {
            file.Internal = true;
            if (reader.U16(offset + 12) != 0xffff) {
                file.Offset = reader.U32(offset + 16);
                file.Size = reader.U32(offset + 20);
            }
        } else if (type != 0x220d) {
            throw std::runtime_error("unknown native BCSAR file entry type");
        } else {
            file.Path = ReadCString(reader, offset + 12,
                                    static_cast<uint32_t>(bytes.size() - offset - 12));
        }
        catalog.Files.push_back(file);
    }

    const auto soundOffsets = parseOffsets(soundTable, 0x2200);
    catalog.Sounds.reserve(soundOffsets.size());
    for (uint32_t index = 0; index < soundOffsets.size(); ++index) {
        const size_t offset = soundOffsets[index];
        NativeBcsarSoundEntry sound;
        sound.Index = index;
        sound.FileId = reader.U32(offset);
        sound.PlayerReference = reader.U32(offset + 4);
        sound.ArchiveVolume = reader.U8(offset + 8);
        sound.RemoteFilter = reader.U8(offset + 9);
        sound.Type = reader.U16(offset + 12);
        const size_t detailOffset = CheckedAdd(
            offset + 16, reader.U32(offset + 16), "BCSAR sound detail");
        sound.OptionMask = reader.U32(offset + 20);
        size_t optionOffset = offset + 24;
        for (uint8_t bit = 0; bit < 32; ++bit) {
            if ((sound.OptionMask & (uint32_t{1} << bit)) == 0) {
                continue;
            }
            if (optionOffset + sizeof(uint32_t) > detailOffset) {
                throw std::runtime_error(
                    "native BCSAR sound options overlap its detail record");
            }
            sound.Options[bit] = reader.U32(optionOffset);
            optionOffset += sizeof(uint32_t);
        }
        sound.NameId = sound.Option(0).value_or(0xffffffff);
        if (sound.NameId != 0xffffffff) {
            if (sound.NameId >= catalog.Strings.size()) {
                throw std::runtime_error("native BCSAR sound name is out of range");
            }
            sound.Name = catalog.Strings[sound.NameId];
        }
        if (const auto relative = sound.Option(8); relative.has_value()) {
            const size_t sound3dOffset = CheckedAdd(
                offset, *relative, "BCSAR Sound3DInfo");
            reader.Require(sound3dOffset, 10, "BCSAR Sound3DInfo");
            NativeBcsarSound3dInfo sound3d;
            sound3d.Flags = reader.U32(sound3dOffset);
            sound3d.DecayRatio = reader.F32(sound3dOffset + 4);
            sound3d.DecayCurve = reader.U8(sound3dOffset + 8);
            sound3d.DopplerFactor = reader.U8(sound3dOffset + 9);
            if (!std::isfinite(sound3d.DecayRatio)) {
                throw std::runtime_error(
                    "native BCSAR Sound3DInfo decay ratio is not finite");
            }
            sound.Sound3d = sound3d;
        }
        if (sound.Type == 0x2203) {
            sound.SequenceOffset = reader.U32(detailOffset);
            const uint32_t bankCount = reader.U32(detailOffset + 8);
            if (bankCount > 4) {
                throw std::runtime_error("native BCSAR sequence bank list is too large");
            }
            for (uint32_t bank = 0; bank < bankCount; ++bank) {
                const uint32_t reference = reader.U32(detailOffset + 12 + bank * 4);
                if ((reference & 0xff000000) != 0x03000000 ||
                    (reference & 0x00ffffff) > 0xff) {
                    throw std::runtime_error("invalid native BCSAR sequence bank reference");
                }
                sound.BankIds.push_back(static_cast<uint8_t>(reference & 0x00ffffff));
            }
        }
        catalog.Sounds.push_back(std::move(sound));
    }

    const auto parseNamedFileTable = [&](size_t table, uint16_t expectedType,
                                         size_t nameOffset) {
        std::vector<NativeBcsarNamedFileEntry> entries;
        const auto offsets = parseOffsets(table, expectedType);
        entries.reserve(offsets.size());
        for (const size_t offset : offsets) {
            NativeBcsarNamedFileEntry entry;
            entry.FileId = reader.U32(offset);
            entry.NameId = reader.U32(offset + nameOffset);
            if (entry.NameId != 0xffffffff) {
                if (entry.NameId >= catalog.Strings.size()) {
                    throw std::runtime_error("native BCSAR resource name is out of range");
                }
                entry.Name = catalog.Strings[entry.NameId];
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    };
    catalog.Banks = parseNamedFileTable(bankTable, 0x2206, 16);
    catalog.WaveArchives = parseNamedFileTable(waveArchiveTable, 0x2207, 12);

    const auto playerOffsets = parseOffsets(playerTable, 0x2209);
    catalog.Players.reserve(playerOffsets.size());
    for (uint32_t index = 0; index < playerOffsets.size(); ++index) {
        const size_t offset = playerOffsets[index];
        NativeBcsarPlayerEntry player;
        player.Index = index;
        player.PlayableSoundLimit = reader.U32(offset);
        player.OptionMask = reader.U32(offset + 4);
        size_t optionOffset = offset + 8;
        for (uint8_t bit = 0; bit < 32; ++bit) {
            if ((player.OptionMask & (uint32_t{1} << bit)) == 0) {
                continue;
            }
            player.Options[bit] = reader.U32(optionOffset);
            optionOffset += sizeof(uint32_t);
        }
        player.NameId = player.Option(0).value_or(0xffffffff);
        if (player.PlayableSoundLimit == 0) {
            throw std::runtime_error(
                "native BCSAR sound player has a zero voice limit");
        }
        if (player.NameId != 0xffffffff) {
            if (player.NameId >= catalog.Strings.size()) {
                throw std::runtime_error(
                    "native BCSAR sound-player name is out of range");
            }
            player.Name = catalog.Strings[player.NameId];
        }
        catalog.Players.push_back(std::move(player));
    }

    for (const auto& sound : catalog.Sounds) {
        if (catalog.ResolvePlayerReference(sound.PlayerReference) == nullptr) {
            throw std::runtime_error(
                "native BCSAR sound has an invalid player reference");
        }
    }
    return catalog;
}

NativeBcsarCatalog ParseNativeBcsarFile(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path);
    return ParseNativeBcsar(bytes);
}

const NativeBcsarSoundEntry* NativeBcsarCatalog::ResolveSoundId(uint32_t soundId) const {
    const uint32_t index = soundId & 0x00ffffff;
    if (index >= Sounds.size()) {
        return nullptr;
    }
    return &Sounds[index];
}

const NativeBcsarPlayerEntry* NativeBcsarCatalog::ResolvePlayerReference(
    uint32_t playerReference) const {
    if ((playerReference & 0xff000000u) != 0x04000000u) {
        return nullptr;
    }
    const uint32_t index = playerReference & 0x00ffffffu;
    return index < Players.size() ? &Players[index] : nullptr;
}

std::optional<uint32_t> NativeBcsarSoundEntry::Option(uint8_t bit) const {
    if (bit >= Options.size()) {
        return {};
    }
    return Options[bit];
}

std::optional<uint32_t> NativeBcsarSoundEntry::PlayerIndex() const {
    if ((PlayerReference & 0xff000000u) != 0x04000000u) {
        return {};
    }
    return PlayerReference & 0x00ffffffu;
}

std::optional<uint32_t> NativeBcsarPlayerEntry::Option(uint8_t bit) const {
    if (bit >= Options.size()) {
        return {};
    }
    return Options[bit];
}

uint8_t NativeBcsarSoundEntry::PanMode() const {
    return static_cast<uint8_t>(Option(1).value_or(0) & 0xff);
}

uint8_t NativeBcsarSoundEntry::PanCurve() const {
    return static_cast<uint8_t>((Option(1).value_or(0) >> 8) & 0xff);
}

uint8_t NativeBcsarSoundEntry::Priority() const {
    return static_cast<uint8_t>(Option(2).value_or(0x40) & 0xff);
}

bool NativeBcsarSoundEntry::Persistent() const {
    return (Option(31).value_or(0) & 0x80000000u) != 0;
}

std::span<const uint8_t> NativeBcsarCatalog::EmbeddedFile(
    std::span<const uint8_t> archive, uint32_t fileId) const {
    if (fileId >= Files.size() || !Files[fileId].Internal || Files[fileId].Size == 0) {
        return {};
    }
    const size_t offset = CheckedAdd(static_cast<size_t>(FileSectionOffset) + 8,
                                     Files[fileId].Offset, "BCSAR embedded file");
    if (offset > archive.size() || Files[fileId].Size > archive.size() - offset) {
        throw std::runtime_error("native BCSAR embedded file is truncated");
    }
    return archive.subspan(offset, Files[fileId].Size);
}

NativeAudioSequence ResolveNativeAudioSequence(std::span<const uint8_t> archive,
                                               const NativeBcsarCatalog& catalog,
                                               uint32_t soundId,
                                               uint32_t randomSeed,
                                               const NativeCseqInitialState* initialState) {
    const NativeBcsarSoundEntry* sound = catalog.ResolveSoundId(soundId);
    if (sound == nullptr || sound->Type != 0x2203 || sound->BankIds.empty()) {
        throw std::runtime_error("native sound ID is not a supported sequence cue");
    }

    const auto sequenceFile = catalog.EmbeddedFile(archive, sound->FileId);
    if (sequenceFile.empty()) {
        throw std::runtime_error("native sequence cue has no embedded CSEQ file");
    }
    const NativeCseqSequence parsed = ParseNativeCseq(
        sequenceFile, sound->SequenceOffset,
        randomSeed == 0 ? soundId : randomSeed, initialState);

    NativeAudioSequence sequence;
    sequence.SoundId = soundId;
    sequence.Name = sound->Name;
    sequence.Timebase = parsed.Timebase;
    sequence.InitialTempo = parsed.InitialTempo;
    sequence.EndTick = parsed.EndTick;
    sequence.LoopStartTick = parsed.LoopStartTick;
    sequence.Looping = parsed.Looping;
    sequence.ControlOnly = parsed.ControlOnly;
    sequence.CseqCommandUsage = parsed.CommandUsage;
    sequence.CseqExtendedCommandUsage = parsed.ExtendedCommandUsage;
    sequence.CseqTimeModifierUsage = parsed.TimeModifierUsage;
    sequence.CseqTimedCommandUsage = parsed.TimedCommandUsage;
    sequence.CseqTieCommandCount = parsed.TieCommandCount;
    sequence.TempoEvents = parsed.TempoEvents;
    sequence.ControlEvents = parsed.ControlEvents;
    sequence.Events.reserve(parsed.Notes.size());
    std::unordered_map<uint64_t, std::shared_ptr<const NativeBcwav>> decodedWaves;

    for (const NativeCseqNoteEvent& note : parsed.Notes) {
        uint8_t bankId = 0;
        if (note.BankSlot < sound->BankIds.size()) {
            bankId = sound->BankIds[note.BankSlot];
        } else {
            const auto directBank = std::find(sound->BankIds.begin(), sound->BankIds.end(),
                                              note.BankSlot);
            if (directBank == sound->BankIds.end()) {
                if (note.BankSlot >= catalog.Banks.size()) {
                    throw std::runtime_error("native sequence bank selector is unresolved");
                }
                bankId = note.BankSlot;
            } else {
                bankId = *directBank;
            }
        }
        if (bankId >= catalog.Banks.size()) {
            throw std::runtime_error("native sequence bank ID is out of range");
        }
        const auto bankFile = catalog.EmbeddedFile(
            archive, catalog.Banks[bankId].FileId);
        if (bankFile.empty()) {
            throw std::runtime_error("native sequence bank has no embedded CBNK file");
        }
        std::optional<BankNote> bank;
        try {
            bank = ResolveBankNote(bankFile, note.Program, note.Note, note.Velocity);
        } catch (const std::exception& ex) {
            throw std::runtime_error(std::string(ex.what()) + " program=" +
                                     std::to_string(note.Program) + " note=" +
                                     std::to_string(note.Note) + " bank=" +
                                     std::to_string(bankId));
        }
        if (!bank.has_value()) {
            NativeAudioCue cue;
            cue.SoundId = soundId;
            cue.Name = sound->Name;
            cue.Program = note.Program;
            cue.Note = note.Note;
            cue.Velocity = note.Velocity;
            cue.DurationTicks = note.DurationTicks;
            cue.BankId = bankId;
            cue.BankSlot = note.BankSlot;
            cue.Transpose = note.Transpose;
            sequence.Events.push_back({note.StartTick, note.DurationTicks,
                                       note.Track, true, note.Tie,
                                       std::move(cue)});
            continue;
        }
        if (bank->WaveArchive >= catalog.WaveArchives.size()) {
            throw std::runtime_error("native bank wave archive ID is out of range");
        }
        const auto waveArchive = catalog.EmbeddedFile(
            archive, catalog.WaveArchives[bank->WaveArchive].FileId);
        if (waveArchive.empty()) {
            throw std::runtime_error("native wave archive has no embedded BCWAR file");
        }

        NativeAudioCue cue;
        cue.SoundId = soundId;
        cue.Name = sound->Name;
        cue.Program = note.Program;
        cue.Note = note.Note;
        cue.Velocity = note.Velocity;
        cue.DurationTicks = note.DurationTicks;
        cue.BankId = bankId;
        cue.WaveArchiveIndex = bank->WaveArchive;
        cue.WaveIndex = bank->Wave;
        cue.RootKey = bank->RootKey;
        cue.Volume = bank->Volume;
        cue.Pitch = bank->Pitch;
        cue.SurroundPan = bank->SurroundPan;
        cue.InterpolationType = bank->InterpolationType;
        cue.IgnoreNoteOff = bank->IgnoreNoteOff;
        cue.ArchiveVolume = sound->ArchiveVolume;
        cue.RemoteFilter = sound->RemoteFilter;
        cue.PanMode = sound->PanMode();
        cue.PanCurve = sound->PanCurve();
        cue.Pan = bank->Pan;
        cue.BankSlot = note.BankSlot;
        cue.TrackVolume = note.Volume;
        cue.Expression = note.Expression;
        cue.TrackPan = note.Pan;
        cue.MasterVolume = note.MasterVolume;
        cue.Transpose = note.Transpose;
        cue.PitchBend = note.PitchBend;
        cue.PitchBendRange = note.PitchBendRange;
        cue.VelocityRange = note.VelocityRange;
        cue.BiquadType = note.BiquadType;
        cue.BiquadValue = note.BiquadValue;
        cue.ModPhase = note.ModPhase;
        cue.ModCurve = note.ModCurve;
        cue.ModDepth = note.ModDepth;
        cue.ModSpeed = note.ModSpeed;
        cue.ModType = note.ModType;
        cue.ModRange = note.ModRange;
        cue.ModDelay = note.ModDelay;
        cue.ModPeriod = note.ModPeriod;
        cue.PortamentoKey = note.PortamentoKey;
        cue.PortamentoEnabled = note.PortamentoEnabled;
        cue.PortamentoTime = note.PortamentoTime;
        cue.SweepPitch = note.SweepPitch;
        cue.TrackSurroundPan = note.SurroundPan;
        cue.LpfCutoff = note.LpfCutoff;
        cue.FxSendA = note.FxSendA;
        cue.FxSendB = note.FxSendB;
        cue.FxSendC = note.FxSendC;
        cue.MainSend = note.MainSend;
        cue.InitialPan = note.InitialPan;
        cue.Damper = note.Damper;
        cue.Attack = note.Attack < 0 ? bank->Attack : static_cast<uint8_t>(note.Attack);
        cue.Decay = note.Decay < 0 ? bank->Decay : static_cast<uint8_t>(note.Decay);
        cue.Sustain = note.Sustain < 0 ? bank->Sustain : static_cast<uint8_t>(note.Sustain);
        cue.Hold = note.Hold < 0 ? bank->Hold : static_cast<uint8_t>(note.Hold);
        cue.Release = note.Release < 0 ? bank->Release : static_cast<uint8_t>(note.Release);
        const uint64_t waveKey = (static_cast<uint64_t>(bank->WaveArchive) << 32) |
                                 bank->Wave;
        auto decoded = decodedWaves.find(waveKey);
        if (decoded == decodedWaves.end()) {
            auto wave = std::make_shared<NativeBcwav>(
                DecodeNativeBcwav(ResolveWaveFromArchive(waveArchive, bank->Wave)));
            decoded = decodedWaves.emplace(waveKey, std::move(wave)).first;
        }
        cue.Wave = decoded->second;
        sequence.Events.push_back({note.StartTick, note.DurationTicks, note.Track,
                                   false, note.Tie, std::move(cue)});
    }
    sequence.ControlOnly = std::none_of(
        sequence.Events.begin(), sequence.Events.end(),
        [](const NativeAudioSequenceEvent& event) { return !event.Silent; });
    return sequence;
}

NativeAudioCue ResolveNativeAudioCue(std::span<const uint8_t> archive,
                                     const NativeBcsarCatalog& catalog,
                                     uint32_t soundId) {
    NativeAudioSequence sequence = ResolveNativeAudioSequence(
        archive, catalog, soundId, soundId);
    const auto audible = std::find_if(
        sequence.Events.begin(), sequence.Events.end(),
        [](const NativeAudioSequenceEvent& event) { return !event.Silent; });
    if (audible == sequence.Events.end()) {
        throw std::runtime_error("native CSEQ is a control-only sequence");
    }
    return std::move(audible->Cue);
}

NativeBcwav ResolveNativeAudioStream(const std::filesystem::path& archivePath,
                                     const NativeBcsarCatalog& catalog,
                                     uint32_t soundId) {
    const NativeBcsarSoundEntry* sound = catalog.ResolveSoundId(soundId);
    if (sound == nullptr || sound->Type != 0x2201 || sound->FileId >= catalog.Files.size()) {
        throw std::runtime_error("native sound ID is not an external stream");
    }
    const NativeBcsarFileEntry& file = catalog.Files[sound->FileId];
    if (file.Internal || file.Path.empty()) {
        throw std::runtime_error("native stream has no external BCSTM path");
    }
    const std::filesystem::path root = archivePath.parent_path().lexically_normal();
    const std::filesystem::path streamPath =
        (root / std::filesystem::path(file.Path)).lexically_normal();
    const auto relative = streamPath.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("native stream path escapes its RomFS sound root");
    }
    return DecodeNativeBcstmFile(streamPath);
}

uint32_t ResolveNativeCodeU32TableEntry(std::span<const uint8_t> codeBin,
                                        uint32_t codeBaseAddress,
                                        uint32_t tableAddress,
                                        uint32_t index) {
    if (codeBaseAddress == 0 || tableAddress < codeBaseAddress) {
        throw std::runtime_error("native code table address precedes its image base");
    }
    const uint64_t offset = static_cast<uint64_t>(tableAddress - codeBaseAddress) +
                            static_cast<uint64_t>(index) * sizeof(uint32_t);
    if (offset + sizeof(uint32_t) > codeBin.size()) {
        throw std::runtime_error("native code table entry is out of range");
    }
    return static_cast<uint32_t>(codeBin[offset]) |
           (static_cast<uint32_t>(codeBin[offset + 1]) << 8) |
           (static_cast<uint32_t>(codeBin[offset + 2]) << 16) |
           (static_cast<uint32_t>(codeBin[offset + 3]) << 24);
}

} // namespace Oot3dNativeGame
