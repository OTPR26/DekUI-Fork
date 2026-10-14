// SPDX-License-Identifier: GPL-2.0-or-later
// Link against audio_core and its normal dependencies, with assertions enabled.
#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>
#include "audio_core/codec.h"
#include "audio_core/interpolate.h"
using namespace AudioCore;

static std::vector<std::array<s16, 2>> Render(unsigned chunk, float rate) {
    AudioInterp::State state;
    std::vector<std::array<s16, 2>> result;
    StereoBuffer16 pending;
    // Deliberately discontinuous stereo data exercises signed accumulation.
    std::array<std::array<s16, 2>, 5000> input{};
    for (unsigned i = 0; i < input.size(); ++i)
        input[i] = {static_cast<s16>((i * 7907) % 60001 - 30000),
                    static_cast<s16>((i * 3511) % 60001 - 30000)};
    std::size_t cursor = 0;
    StereoFrame16 frame{};
    std::size_t output = 0;
    while (cursor < input.size() || !pending.empty()) {
        if (pending.empty()) {
            const auto end = std::min<std::size_t>(cursor + chunk, input.size());
            pending.insert(pending.end(), input.begin() + cursor, input.begin() + end);
            cursor = end;
        }
        AudioInterp::Polyphase(state, pending, rate, frame, output);
        if (output == frame.size()) {
            result.insert(result.end(), frame.begin(), frame.end());
            output = 0;
        }
    }
    result.insert(result.end(), frame.begin(), frame.begin() + output);
    return result;
}
int main() {
    // Buffer boundaries must not change the waveform, including tiny loops.
    for (float rate : {.125f, .77605f, 1.f, 1.0359f, 4.26825f}) {
        const auto whole = Render(5000, rate);
        for (unsigned chunk : {1u, 9u, 14u, 50u, 160u})
            assert(Render(chunk, rate) == whole);
    }
    // Independent LLE impulse measurements at phase zero, not computed from
    // the table under test. Firmware at rate 1.0: four nonzero taps.
    AudioInterp::State interp;
    StereoBuffer16 input(160, {0, 0});input[16] = {-32768, 32767};
    StereoFrame16 output{};std::size_t position = 0;
    AudioInterp::Polyphase(interp, input, 1, output, position);
    assert(position == 160);
    constexpr std::array<s16, 4> negative{-3, -6722, -19426, -6600};
    for (unsigned i = 0; i < 160; ++i) {
        const s16 expected = i >= 16 && i < 20 ? negative[i - 16] : 0;
        assert(output[i][0] == expected);
        assert(output[i][1] == (expected ? -expected - 1 : 0));
    }
    // ADPCM must neither emit nor decode the unused low nibble. Its history
    // must finish at the last requested sample, including across frame edges.
    const std::array<u8, 16> data{0,0x12,0x34,0x56,0x71,0x23,0x45,0x67,
                                 0,0x12,0x34,0x56,0x71,0x23,0x45,0x67};
    const std::array<s16, 16> coefficients{};
    for (std::size_t count = 0; count <= 28; ++count) {
        Codec::ADPCMState state{};
        const auto decoded = Codec::DecodeADPCM(data.data(), count, coefficients, state);
        assert(decoded.size() == count);
        Codec::ADPCMState legacy_state{};
        const auto legacy=Codec::DecodeADPCM(data.data(), count, coefficients, legacy_state, false);
        assert(legacy.size()==count+count%2);
        assert(legacy_state.yn1==(legacy.empty()?0:s16((legacy.size()-1)%7+1)));
        if(count%2)assert(legacy.back()[0]==s16(count%7+1));
        for (std::size_t i = 0; i < count; ++i)
            assert(decoded[i][0] == s16(i % 7 + 1) && decoded[i][0] == decoded[i][1]);
        assert(state.yn1 == (count ? s16((count - 1) % 7 + 1) : 0));
        assert(state.yn2 == (count > 1 ? s16((count - 2) % 7 + 1) : 0));
    }
    std::cout << "Polyphase impulse, buffer boundaries, stereo and exact ADPCM lengths passed.\n";
}
