// Native audio: the sequence mixer (NativeAudioMixer).
#include "oot3d_native_audio_internal.h"

namespace Oot3dNativeGame {

namespace {

double SequenceTickToSeconds(const NativeAudioSequence& sequence,
                             uint64_t tick) {
    uint64_t cursor = 0;
    uint16_t tempo = sequence.InitialTempo == 0 ? 120 : sequence.InitialTempo;
    double seconds = 0.0;
    for (const auto& change : sequence.TempoEvents) {
        if (change.Tick > tick) {
            break;
        }
        if (change.Tick > cursor) {
            seconds += static_cast<double>(change.Tick - cursor) * 60.0 /
                       (static_cast<double>(tempo) * sequence.Timebase);
        }
        cursor = change.Tick;
        if (change.Tempo != 0) {
            tempo = change.Tempo;
        }
    }
    if (tick > cursor) {
        seconds += static_cast<double>(tick - cursor) * 60.0 /
                   (static_cast<double>(tempo) * sequence.Timebase);
    }
    return seconds;
}

uint64_t SequenceTickToFrame(const NativeAudioSequence& sequence,
                             uint64_t tick, uint32_t sampleRate) {
    return static_cast<uint64_t>(std::max(
        std::llround(SequenceTickToSeconds(sequence, tick) * sampleRate),
        0ll));
}

uint64_t SequenceFrameToTick(const NativeAudioSequence& sequence,
                             uint64_t frame, uint32_t sampleRate) {
    uint64_t low = 0;
    uint64_t high = sequence.EndTick;
    while (low < high) {
        const uint64_t middle = low + (high - low + 1) / 2;
        if (SequenceTickToFrame(sequence, middle, sampleRate) <= frame) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return low;
}

} // namespace

NativeAudioMixer::NativeAudioMixer(uint32_t outputSampleRate)
    : mOutputSampleRate(outputSampleRate) {
    if (mOutputSampleRate == 0) {
        throw std::runtime_error("native audio mixer output rate is zero");
    }
}

void NativeAudioMixer::SetEnvelopeProfile(
    std::shared_ptr<const NativeAudioEnvelopeProfile> profile) {
    if (!profile || profile->GainCurve.empty()) {
        throw std::runtime_error("native audio envelope profile is empty");
    }
    mEnvelopeProfile = std::move(profile);
}

void NativeAudioMixer::SetModulationProfile(
    std::shared_ptr<const NativeAudioModulationProfile> profile) {
    if (!profile || profile->SineQuarterTable.size() < 33 ||
        profile->UpdateMilliseconds == 0) {
        throw std::runtime_error("native audio modulation profile is empty");
    }
    mModulationProfile = std::move(profile);
}

void NativeAudioMixer::SetMixProfile(
    std::shared_ptr<const NativeAudioMixProfile> profile) {
    if (!profile || profile->PanScale <= 0.0f ||
        profile->SurroundLowerScale <= 0.0f ||
        profile->SurroundUpperScale <= 0.0f) {
        throw std::runtime_error("native audio mix profile is empty");
    }
    mMixProfile = std::move(profile);
}

void NativeAudioMixer::SetFilterProfile(
    std::shared_ptr<const NativeAudioFilterProfile> profile) {
    if (!profile || profile->CutoffFrequencies.empty() ||
        profile->CosineLookup.size() != 256) {
        throw std::runtime_error("native audio filter profile is empty");
    }
    mFilterProfile = std::move(profile);
}

void NativeAudioMixer::SetResamplerProfile(
    std::shared_ptr<const NativeAudioResamplerProfile> profile) {
    if (!profile) {
        throw std::runtime_error("native audio resampler profile is empty");
    }
    mResamplerProfile = std::move(profile);
}

void NativeAudioMixer::SetOutputMode(NativeAudioOutputMode outputMode) {
    mOutputMode = outputMode;
}

void NativeAudioMixer::ConfigureAuxBus(
    size_t bus, const NativeAudioAuxBusSettings& settings) {
    if (const auto* reverb =
            std::get_if<NativeAudioReverbEffect>(&settings.Effect);
        reverb != nullptr && reverb->Enabled &&
        reverb->NativeSampleRate != 0) {
        mDspEffects.SetSampleRates(
            reverb->NativeSampleRate, mOutputSampleRate);
    }
    mDspEffects.ConfigureBus(bus, settings);
}

const NativeAudioAuxBusSettings& NativeAudioMixer::AuxBusSettings(
    size_t bus) const {
    return mDspEffects.BusSettings(bus);
}

void NativeAudioMixer::InitializeEnvelope(Voice& voice) const {
    if (!voice.Cue || !mEnvelopeProfile) {
        return;
    }
    const NativeAudioCue& cue = *voice.Cue;
    auto& envelope = voice.Envelope;
    envelope.Phase = EnvelopePhase::Attack;
    envelope.CurrentLevel =
        mEnvelopeProfile->FloorDb / mEnvelopeProfile->LevelToDbScale;
    envelope.DecayStep = mEnvelopeProfile->DecayStep(cue.Decay);
    envelope.ReleaseStep = mEnvelopeProfile->ReleaseStep(cue.Release);
    envelope.AttackMultiplier = mEnvelopeProfile->AttackMultipliers[cue.Attack];
    envelope.HoldTicks = mEnvelopeProfile->HoldTicks(cue.Hold);
    envelope.HoldRemaining = 0;
    envelope.Sustain = cue.Sustain;
    envelope.FramesUntilAdvance = 0.0;
    envelope.Enabled = true;
}

void NativeAudioMixer::InitializeModulation(Voice& voice) const {
    if (!voice.Cue || !mModulationProfile) {
        return;
    }
    const NativeAudioCue& cue = *voice.Cue;
    auto& modulation = voice.Modulation;
    modulation.Enabled = cue.ModDepth != 0 && cue.ModRange != 0 &&
                         cue.ModCurve == 0 && cue.ModType <= 2;
    modulation.Phase = static_cast<float>(cue.ModPhase) /
                       mModulationProfile->PhaseScale;
    modulation.Phase -= std::floor(modulation.Phase);
    modulation.DelayMilliseconds = static_cast<uint32_t>(
        std::max<int>(cue.ModDelay, 0));
    modulation.ElapsedMilliseconds = 0;
    modulation.FramesUntilAdvance = 0.0;
    if (modulation.Enabled && modulation.DelayMilliseconds == 0) {
        modulation.Value = static_cast<float>(cue.ModRange) *
            mModulationProfile->SampleSine(modulation.Phase) *
            static_cast<float>(static_cast<uint8_t>(cue.ModDepth));
    }

    auto& portamento = voice.Portamento;
    const float sweepPitch =
        static_cast<float>(cue.PortamentoKey) - cue.Note +
        static_cast<float>(cue.SweepPitch) / 256.0f;
    if (cue.PortamentoEnabled && cue.PortamentoTime != 0 &&
        std::abs(sweepPitch) > 0.0f) {
        const float scaledDuration = std::abs(sweepPitch) *
            cue.PortamentoTime * cue.PortamentoTime;
        const uint32_t durationUnits =
            static_cast<uint32_t>(scaledDuration) >> 5;
        portamento.StartPitch = sweepPitch;
        portamento.CurrentPitch = sweepPitch;
        portamento.DurationMilliseconds = durationUnits *
            mModulationProfile->UpdateMilliseconds;
        portamento.ElapsedMilliseconds = 0;
        portamento.Enabled = portamento.DurationMilliseconds != 0;
    }
}

void NativeAudioMixer::AdvanceModulation(Voice& voice, int8_t depth,
                                         int8_t speed, uint8_t type,
                                         int8_t range) const {
    if (!mModulationProfile || !voice.Cue) {
        return;
    }
    const uint32_t elapsed = mModulationProfile->UpdateMilliseconds;
    auto& modulation = voice.Modulation;
    modulation.Enabled = depth != 0 && range != 0 &&
                         voice.Cue->ModCurve == 0 && type <= 2;
    if (modulation.Enabled) {
        modulation.ElapsedMilliseconds += elapsed;
        modulation.Phase += static_cast<float>(speed) * elapsed *
                            mModulationProfile->PhaseAdvanceScale;
        modulation.Phase -= std::floor(modulation.Phase);
        modulation.Value = modulation.ElapsedMilliseconds <
                                   modulation.DelayMilliseconds
            ? 0.0f
            : static_cast<float>(range) *
                  mModulationProfile->SampleSine(modulation.Phase) *
                  static_cast<float>(static_cast<uint8_t>(depth));
    } else {
        modulation.Value = 0.0f;
    }

    auto& portamento = voice.Portamento;
    if (portamento.Enabled) {
        portamento.ElapsedMilliseconds = std::min(
            portamento.ElapsedMilliseconds + elapsed,
            portamento.DurationMilliseconds);
        const float remaining = static_cast<float>(
            portamento.DurationMilliseconds - portamento.ElapsedMilliseconds);
        portamento.CurrentPitch = portamento.StartPitch * remaining /
                                  portamento.DurationMilliseconds;
        if (portamento.ElapsedMilliseconds ==
            portamento.DurationMilliseconds) {
            portamento.Enabled = false;
        }
    }
}

bool NativeAudioMixer::AdvanceEnvelope(Voice& voice, uint32_t ticks) const {
    auto& envelope = voice.Envelope;
    if (!envelope.Enabled || !mEnvelopeProfile) {
        return false;
    }
    switch (envelope.Phase) {
        case EnvelopePhase::Attack:
            for (uint32_t tick = 0; tick < ticks; ++tick) {
                envelope.CurrentLevel *= envelope.AttackMultiplier;
                if (envelope.CurrentLevel >= -0.03125f) {
                    envelope.CurrentLevel = 0.0f;
                    envelope.Phase = EnvelopePhase::Hold;
                    envelope.HoldRemaining = envelope.HoldTicks;
                    return false;
                }
            }
            return false;
        case EnvelopePhase::Hold:
            if (ticks < envelope.HoldRemaining) {
                envelope.HoldRemaining = static_cast<uint16_t>(
                    envelope.HoldRemaining - ticks);
                return false;
            }
            ticks -= envelope.HoldRemaining;
            envelope.HoldRemaining = 0;
            envelope.Phase = EnvelopePhase::Decay;
            [[fallthrough]];
        case EnvelopePhase::Decay: {
            const float sustain = static_cast<float>(
                mEnvelopeProfile->SustainLevels[envelope.Sustain]);
            envelope.CurrentLevel -= envelope.DecayStep * ticks;
            if (envelope.CurrentLevel < sustain) {
                envelope.CurrentLevel = sustain;
                envelope.Phase = EnvelopePhase::Sustain;
            }
            return false;
        }
        case EnvelopePhase::Sustain:
            return false;
        case EnvelopePhase::Release:
            envelope.CurrentLevel -= envelope.ReleaseStep * ticks;
            return envelope.CurrentLevel <=
                   mEnvelopeProfile->FloorDb /
                       mEnvelopeProfile->LevelToDbScale;
    }
    return false;
}

double NativeAudioMixer::EnvelopeGain(const Voice& voice) const {
    if (!voice.Envelope.Enabled || !mEnvelopeProfile) {
        return 1.0;
    }
    return mEnvelopeProfile->GainForLevel(voice.Envelope.CurrentLevel);
}

uint64_t NativeAudioMixer::ControlFramesToSamples(
    uint32_t controlFrames) const {
    return static_cast<uint64_t>(std::llround(
        static_cast<double>(controlFrames) * mOutputSampleRate *
        kNativeAudioControlFrameMilliseconds / 1000.0));
}

void NativeAudioMixer::ConfigureVoiceGainFade(
    Voice& voice, double target, uint64_t sampleFrames, bool stopAfter) {
    target = std::clamp(target, 0.0, 1.0);
    voice.PlayerGainTarget = target;
    voice.StopAfterPlayerFade = stopAfter;
    if (sampleFrames == 0) {
        voice.PlayerGain = target;
        voice.PlayerGainStep = 0.0;
        voice.PlayerGainFramesRemaining = 0;
        return;
    }
    voice.PlayerGainStep = (target - voice.PlayerGain) /
                           static_cast<double>(sampleFrames);
    voice.PlayerGainFramesRemaining = sampleFrames;
}

double NativeAudioMixer::SequencePlayerGainAt(
    const SequencePlayback& playback, uint64_t frame) {
    double gain = std::clamp(
        static_cast<double>(playback.HandleGain.Value(frame)), 0.0, 1.0);
    for (const auto& volume : playback.PlayerVolumeControls) {
        gain *= std::clamp(static_cast<double>(volume.Value(frame)), 0.0, 1.0);
    }
    return gain;
}

float NativeAudioMixer::SequenceControlState::Value(uint64_t frame) const {
    if (!Active || EndFrame <= StartFrame || frame >= EndFrame) {
        return TargetValue;
    }
    if (frame <= StartFrame) {
        return StartValue;
    }
    const float progress = static_cast<float>(frame - StartFrame) /
        static_cast<float>(EndFrame - StartFrame);
    return StartValue + (TargetValue - StartValue) * progress;
}

void NativeAudioMixer::StartOrRefreshVoice(
    uint64_t ownerId, std::shared_ptr<const NativeAudioCue> cue) {
    if (!cue || !cue->Wave || cue->Wave->SampleRate == 0 ||
        cue->Wave->SampleCount == 0 || cue->Wave->ChannelCount == 0 ||
        cue->Wave->InterleavedSamples.empty()) {
        throw std::runtime_error("native audio voice has no decoded wave");
    }
    const auto found = mVoices.find(ownerId);
    if (found != mVoices.end() && found->second.Cue &&
        found->second.Cue->SoundId == cue->SoundId) {
        found->second.Cue = std::move(cue);
        return;
    }
    Voice voice{std::move(cue), {}, 0.0, 127, 64};
    InitializeEnvelope(voice);
    InitializeModulation(voice);
    mVoices[ownerId] = std::move(voice);
}

void NativeAudioMixer::StartOrRefreshStream(
    uint64_t ownerId, std::shared_ptr<const NativeBcwav> stream,
    uint8_t volume, uint8_t pan) {
    if (!stream || stream->SampleRate == 0 || stream->SampleCount == 0 ||
        stream->ChannelCount == 0 || stream->InterleavedSamples.empty()) {
        throw std::runtime_error("native audio stream has no decoded samples");
    }
    const auto found = mVoices.find(ownerId);
    if (found != mVoices.end() && found->second.Stream == stream) {
        found->second.Volume = volume;
        found->second.Pan = pan;
        return;
    }
    mVoices[ownerId] = Voice{{}, std::move(stream), 0.0, volume, pan};
}

void NativeAudioMixer::StartOrRefreshSequence(
    uint64_t ownerId, std::shared_ptr<const NativeAudioSequence> sequence,
    double pitchScale, double gainScale, double spatialPan,
    double spatialSurroundPan, double distanceFilter,
    double spatialAuxSend, uint32_t fadeInFrames) {
    if (!sequence) {
        throw std::runtime_error("native audio sequence is null");
    }
    const auto found = mSequences.find(ownerId);
    if (found != mSequences.end() && found->second.Sequence &&
        found->second.Sequence->SoundId == sequence->SoundId) {
        found->second.TrackEnableMask = sequence->InitialTrackEnableMask;
        found->second.Sequence = std::move(sequence);
        found->second.PitchScale = pitchScale;
        found->second.GainScale = std::max(0.0, gainScale);
        found->second.SpatialPan = std::clamp(spatialPan, -1.0, 1.0);
        found->second.SpatialSurroundPan =
            std::clamp(spatialSurroundPan, 0.0, 2.0);
        found->second.DistanceFilter = std::clamp(distanceFilter, 0.0, 1.0);
        found->second.SpatialAuxSend = std::clamp(spatialAuxSend, 0.0, 1.0);
        for (auto& voice : found->second.Voices) {
            voice.PitchScale = pitchScale;
            voice.GainScale = found->second.GainScale;
            voice.SpatialPan = found->second.SpatialPan;
            voice.SpatialSurroundPan = found->second.SpatialSurroundPan;
            voice.DistanceFilter = found->second.DistanceFilter;
            voice.SpatialAuxSend = found->second.SpatialAuxSend;
        }
        std::erase_if(found->second.Voices, [&](const Voice& voice) {
            return (found->second.TrackEnableMask &
                    (uint16_t{1} << voice.SequenceTrack)) == 0;
        });
        return;
    }
    mVoices.erase(ownerId);
    SequencePlayback playback;
    playback.Sequence = std::move(sequence);
    playback.TrackEnableMask = playback.Sequence->InitialTrackEnableMask;
    playback.PitchScale = pitchScale;
    playback.GainScale = std::max(0.0, gainScale);
    playback.SpatialPan = std::clamp(spatialPan, -1.0, 1.0);
    playback.SpatialSurroundPan = std::clamp(spatialSurroundPan, 0.0, 2.0);
    playback.DistanceFilter = std::clamp(distanceFilter, 0.0, 1.0);
    playback.SpatialAuxSend = std::clamp(spatialAuxSend, 0.0, 1.0);
    playback.HandleGain.StartValue = fadeInFrames == 0 ? 1.0f : 0.0f;
    playback.HandleGain.TargetValue = 1.0f;
    playback.HandleGain.StartFrame = 0;
    playback.HandleGain.EndFrame = ControlFramesToSamples(fadeInFrames);
    playback.HandleGain.Active = fadeInFrames != 0;
    for (auto& volume : playback.PlayerVolumeControls) {
        volume.StartValue = 1.0f;
        volume.TargetValue = 1.0f;
    }
    mSequences[ownerId] = std::move(playback);
}

bool NativeAudioMixer::SetSequencePlayerVolume(
    uint64_t ownerId, uint8_t layer, uint8_t volume, uint32_t fadeFrames) {
    const auto found = mSequences.find(ownerId);
    if (found == mSequences.end() ||
        layer >= found->second.PlayerVolumeControls.size()) {
        return false;
    }
    SequencePlayback& playback = found->second;
    SequenceControlState& control = playback.PlayerVolumeControls[layer];
    const float current = control.Value(playback.OutputFrame);
    const float normalized = static_cast<float>(volume) / 127.0f;
    const float target = normalized * normalized;
    control.StartValue = fadeFrames == 0 ? target : current;
    control.TargetValue = target;
    control.StartFrame = playback.OutputFrame;
    control.EndFrame = playback.OutputFrame +
                       ControlFramesToSamples(fadeFrames);
    control.Active = fadeFrames != 0;
    return true;
}

bool NativeAudioMixer::SetSequenceTrackEnabled(
    uint64_t ownerId, uint8_t firstTrack, uint8_t lastTrack, bool enabled) {
    const auto found = mSequences.find(ownerId);
    if (found == mSequences.end() || firstTrack > lastTrack || lastTrack >= 16) {
        return false;
    }
    SequencePlayback& playback = found->second;
    for (uint8_t track = firstTrack; track <= lastTrack; ++track) {
        const uint16_t bit = uint16_t{1} << track;
        if (enabled) {
            playback.TrackEnableMask |= bit;
        } else {
            playback.TrackEnableMask &= static_cast<uint16_t>(~bit);
        }
    }
    if (!enabled) {
        std::erase_if(playback.Voices, [&](const Voice& voice) {
            return voice.SequenceTrack >= firstTrack &&
                   voice.SequenceTrack <= lastTrack;
        });
    }
    return true;
}

bool NativeAudioMixer::ReconfigureSequence(
    uint64_t ownerId,
    std::shared_ptr<const NativeAudioSequence> currentCycle,
    std::shared_ptr<const NativeAudioSequence> nextLoop) {
    const auto found = mSequences.find(ownerId);
    if (found == mSequences.end() || !currentCycle || !nextLoop ||
        !found->second.Sequence ||
        currentCycle->SoundId != found->second.Sequence->SoundId ||
        nextLoop->SoundId != found->second.Sequence->SoundId) {
        return false;
    }
    SequencePlayback& playback = found->second;
    const uint64_t currentTick = SequenceFrameToTick(
        *playback.Sequence, playback.TimelineFrame, mOutputSampleRate);
    playback.Sequence = std::move(currentCycle);
    playback.NextLoopSequence = std::move(nextLoop);
    playback.TimelineFrame = SequenceTickToFrame(
        *playback.Sequence,
        std::min(currentTick, playback.Sequence->EndTick),
        mOutputSampleRate);
    playback.NextEvent = static_cast<size_t>(std::upper_bound(
        playback.Sequence->Events.begin(), playback.Sequence->Events.end(),
        currentTick,
        [](uint64_t tick, const NativeAudioSequenceEvent& event) {
            return tick < event.StartTick;
        }) - playback.Sequence->Events.begin());
    playback.NextControlEvent = 0;
    playback.TrackControls = {};
    playback.MasterVolumeControl = {};
    for (const auto& control : playback.Sequence->ControlEvents) {
        if (control.StartTick > currentTick) {
            break;
        }
        ++playback.NextControlEvent;
        SequenceControlState& state =
            control.Kind == NativeCseqControlKind::MasterVolume
                ? playback.MasterVolumeControl
                : playback.TrackControls[
                      static_cast<size_t>(control.Kind)][control.Track];
        state.StartValue = control.StartValue;
        state.TargetValue = control.TargetValue;
        state.StartFrame = SequenceTickToFrame(
            *playback.Sequence, control.StartTick, mOutputSampleRate);
        state.EndFrame = SequenceTickToFrame(
            *playback.Sequence,
            control.StartTick + control.DurationTicks,
            mOutputSampleRate);
        state.Active = true;
    }
    return true;
}

std::optional<double> NativeAudioMixer::SequencePlayerGain(
    uint64_t ownerId) const {
    const auto found = mSequences.find(ownerId);
    if (found == mSequences.end()) {
        return {};
    }
    return SequencePlayerGainAt(found->second, found->second.OutputFrame);
}

std::optional<uint64_t> NativeAudioMixer::SequenceTimelineTick(
    uint64_t ownerId) const {
    const auto found = mSequences.find(ownerId);
    if (found == mSequences.end() || !found->second.Sequence) {
        return {};
    }
    return SequenceFrameToTick(*found->second.Sequence,
                               found->second.TimelineFrame,
                               mOutputSampleRate);
}

std::optional<uint64_t> NativeAudioMixer::SequenceLoopCount(
    uint64_t ownerId) const {
    const auto found = mSequences.find(ownerId);
    return found == mSequences.end()
        ? std::optional<uint64_t>{}
        : std::optional<uint64_t>{found->second.LoopCount};
}

void NativeAudioMixer::StopVoice(uint64_t ownerId, uint32_t fadeOutFrames) {
    if (fadeOutFrames == 0) {
        mVoices.erase(ownerId);
        mSequences.erase(ownerId);
        return;
    }
    if (const auto voice = mVoices.find(ownerId); voice != mVoices.end()) {
        ConfigureVoiceGainFade(voice->second, 0.0,
                               ControlFramesToSamples(fadeOutFrames), true);
    }
    if (const auto sequence = mSequences.find(ownerId);
        sequence != mSequences.end()) {
        SequencePlayback& playback = sequence->second;
        const float current = playback.HandleGain.Value(playback.OutputFrame);
        playback.HandleGain.StartValue = current;
        playback.HandleGain.TargetValue = 0.0f;
        playback.HandleGain.StartFrame = playback.OutputFrame;
        playback.HandleGain.EndFrame = playback.OutputFrame +
            ControlFramesToSamples(fadeOutFrames);
        playback.HandleGain.Active = true;
        playback.Stopping = true;
    }
}

std::vector<int16_t> NativeAudioMixer::MixStereo(size_t frameCount) {
    constexpr size_t kMixChannels = kNativeAudioDspChannelCount;
    std::vector<double> accumulator(frameCount * kMixChannels, 0.0);
    std::array<std::vector<int32_t>, 2> auxiliary{
        std::vector<int32_t>(frameCount * kMixChannels, 0),
        std::vector<int32_t>(frameCount * kMixChannels, 0),
    };
    const auto mixVoice = [&](Voice& voice,
                              const SequencePlayback* sequencePlayback,
                              size_t mixFrameCount,
                              size_t destinationFrame,
                              uint64_t sequenceStartFrame) {
        const NativeAudioCue* cue = voice.Cue.get();
        const auto& wave = cue != nullptr ? *cue->Wave : *voice.Stream;
        const double basePitchSemitones = cue != nullptr
            ? static_cast<double>(cue->Note) + cue->Transpose - cue->RootKey
            : 0.0;
        const double velocity = cue != nullptr
            ? static_cast<double>(cue->Velocity) * cue->VelocityRange /
                  (127.0 * 127.0)
            : 1.0;
        const auto controlValue = [&](NativeCseqControlKind kind,
                                      float fallback,
                                      uint64_t frame) {
            if (sequencePlayback == nullptr || !voice.SequenceVoice) {
                return fallback;
            }
            const SequenceControlState& state =
                kind == NativeCseqControlKind::MasterVolume
                    ? sequencePlayback->MasterVolumeControl
                    : sequencePlayback->TrackControls[
                          static_cast<size_t>(kind)][voice.SequenceTrack];
            return state.Active ? state.Value(frame) : fallback;
        };
        std::optional<NativeAudioSimpleFilterCoefficients> simpleFilter;
        if (cue != nullptr && mFilterProfile) {
            const float cutoff = std::clamp(
                static_cast<float>(cue->LpfCutoff) / 64.0f -
                    static_cast<float>(voice.DistanceFilter),
                0.0f, 1.0f);
            const uint16_t frequency = CalculateNativeAudioCutoffFrequency(
                *mFilterProfile, cutoff);
            if (frequency < 16000) {
                simpleFilter = CalculateNativeAudioSimpleFilter(
                    *mFilterProfile, frequency);
            } else {
                voice.SimpleFilterHistory = {};
            }
        }

        size_t outputFrame = 0;
        if (voice.DelayFrames >= mixFrameCount) {
            voice.DelayFrames -= mixFrameCount;
            return false;
        }
        outputFrame = voice.DelayFrames;
        voice.DelayFrames = 0;
        mMixedVoiceSampleCount += mixFrameCount - outputFrame;
        for (; outputFrame < mixFrameCount; ++outputFrame) {
            const uint64_t sequenceFrame = sequencePlayback != nullptr
                ? sequenceStartFrame + outputFrame
                : 0;
            if (voice.NoteOnFramesRemaining == 0) {
                if (!voice.Envelope.Enabled) {
                    return true;
                }
                voice.Envelope.Phase = EnvelopePhase::Release;
            }
            if (voice.Envelope.Enabled) {
                while (voice.Envelope.FramesUntilAdvance <= 0.0) {
                    if (AdvanceEnvelope(voice, 5)) {
                        return true;
                    }
                    voice.Envelope.FramesUntilAdvance +=
                        static_cast<double>(mOutputSampleRate) * 0.005;
                }
                voice.Envelope.FramesUntilAdvance -= 1.0;
            }
            if (mModulationProfile &&
                (voice.SequenceVoice || voice.Modulation.Enabled ||
                 voice.Portamento.Enabled)) {
                while (voice.Modulation.FramesUntilAdvance <= 0.0) {
                    const int8_t modDepth = static_cast<int8_t>(std::clamp(
                        std::lround(controlValue(
                            NativeCseqControlKind::ModDepth,
                            static_cast<float>(cue->ModDepth), sequenceFrame)),
                        -128l, 127l));
                    const int8_t modSpeed = static_cast<int8_t>(std::clamp(
                        std::lround(controlValue(
                            NativeCseqControlKind::ModSpeed,
                            static_cast<float>(cue->ModSpeed), sequenceFrame)),
                        -128l, 127l));
                    const uint8_t modType = static_cast<uint8_t>(std::clamp(
                        std::lround(controlValue(
                            NativeCseqControlKind::ModType,
                            static_cast<float>(cue->ModType), sequenceFrame)),
                        0l, 255l));
                    const int8_t modRange = static_cast<int8_t>(std::clamp(
                        std::lround(controlValue(
                            NativeCseqControlKind::ModRange,
                            static_cast<float>(cue->ModRange), sequenceFrame)),
                        -128l, 127l));
                    AdvanceModulation(voice, modDepth, modSpeed, modType,
                                      modRange);
                    voice.Modulation.FramesUntilAdvance +=
                        static_cast<double>(mOutputSampleRate) *
                        mModulationProfile->UpdateMilliseconds / 1000.0;
                }
                voice.Modulation.FramesUntilAdvance -= 1.0;
            }
            if (voice.SourceFrame >= wave.SampleCount) {
                if (!wave.Looping || wave.LoopStart >= wave.SampleCount) {
                    return true;
                }
                const double loopLength = wave.SampleCount - wave.LoopStart;
                voice.SourceFrame = wave.LoopStart +
                    std::fmod(voice.SourceFrame - wave.LoopStart, loopLength);
                voice.SourceLooped = true;
            }
            const uint32_t frame0 = static_cast<uint32_t>(voice.SourceFrame);
            const double fraction = voice.SourceFrame - frame0;
            const auto source = [&](int64_t frame, uint16_t channel) {
                if (wave.Looping && wave.LoopStart < wave.SampleCount) {
                    const int64_t loopStart = wave.LoopStart;
                    const int64_t loopLength = wave.SampleCount - wave.LoopStart;
                    if (voice.SourceLooped && frame < loopStart &&
                        loopStart - frame <= loopLength) {
                        frame = static_cast<int64_t>(wave.SampleCount) -
                                (loopStart - frame);
                    } else if (frame >=
                               static_cast<int64_t>(wave.SampleCount)) {
                        frame = loopStart +
                            (frame - static_cast<int64_t>(wave.SampleCount)) %
                                loopLength;
                    }
                }
                if (frame < 0 ||
                    frame >= static_cast<int64_t>(wave.SampleCount)) {
                    return int16_t{0};
                }
                const uint16_t selected = wave.ChannelCount == 1 ? 0 : channel;
                return wave.InterleavedSamples[
                    static_cast<size_t>(frame) * wave.ChannelCount + selected];
            };
            std::array<double, 2> sourceSamples{};
            const uint16_t sourceChannelCount = std::min<uint16_t>(
                wave.ChannelCount, 2);
            const uint8_t interpolationType = cue != nullptr
                ? cue->InterpolationType
                : 0;
            for (uint16_t sourceChannel = 0;
                 sourceChannel < sourceChannelCount; ++sourceChannel) {
                double sample = 0.0;
                if (interpolationType == 0 && mResamplerProfile) {
                    const size_t phase = std::min<size_t>(
                        static_cast<size_t>(fraction *
                            NativeAudioResamplerProfile::PhaseCount),
                        NativeAudioResamplerProfile::PhaseCount - 1);
                    const auto& coefficients =
                        mResamplerProfile->PolyphaseCoefficients[phase];
                    int64_t filtered = 0;
                    for (size_t tap = 0;
                         tap < NativeAudioResamplerProfile::TapCount; ++tap) {
                        const int64_t sourceFrame =
                            static_cast<int64_t>(frame0) +
                            static_cast<int64_t>(tap) - 1;
                        filtered += static_cast<int64_t>(coefficients[tap]) *
                                    source(sourceFrame, sourceChannel);
                    }
                    sample = static_cast<double>(std::clamp<int64_t>(
                        filtered >> 15, -32768, 32767));
                } else if (interpolationType <= 1) {
                    const int32_t first = source(frame0, sourceChannel);
                    const int32_t second = source(
                        static_cast<int64_t>(frame0) + 1, sourceChannel);
                    const int32_t delta = std::clamp(
                        second - first, -32768, 32767);
                    sample = first + delta * fraction;
                } else if (interpolationType == 2) {
                    sample = source(frame0, sourceChannel);
                } else {
                    throw std::runtime_error(
                        "unsupported native audio interpolation type");
                }
                if (simpleFilter.has_value()) {
                    const int32_t input = static_cast<int32_t>(std::clamp(
                        std::lround(sample), -32768l, 32767l));
                    const int64_t filtered =
                        static_cast<int64_t>(simpleFilter->B0) * input +
                        static_cast<int64_t>(simpleFilter->A1) *
                            voice.SimpleFilterHistory[sourceChannel];
                    const int32_t output = static_cast<int32_t>(std::clamp<int64_t>(
                        filtered >> 15, -32768, 32767));
                    voice.SimpleFilterHistory[sourceChannel] = output;
                    sample = output;
                }
                sourceSamples[sourceChannel] = sample;
            }
            const float modulationValue = voice.Modulation.Value;
            const uint8_t modulationType = cue != nullptr
                ? static_cast<uint8_t>(std::clamp(
                      std::lround(controlValue(
                          NativeCseqControlKind::ModType,
                          static_cast<float>(cue->ModType), sequenceFrame)),
                      0l, 255l))
                : 0;
            const double modulationGain =
                cue != nullptr && modulationType == 1 && mEnvelopeProfile
                ? mEnvelopeProfile->GainForDb(modulationValue * 6.0f)
                : 1.0;
            const double trackVolume = cue != nullptr
                ? std::clamp(static_cast<double>(controlValue(
                      NativeCseqControlKind::Volume,
                      static_cast<float>(cue->TrackVolume), sequenceFrame)),
                      0.0, 127.0)
                : 127.0;
            const double expression = cue != nullptr
                ? std::clamp(static_cast<double>(controlValue(
                      NativeCseqControlKind::Expression,
                      static_cast<float>(cue->Expression), sequenceFrame)),
                      0.0, 127.0)
                : 127.0;
            const double masterVolume = cue != nullptr
                ? std::clamp(static_cast<double>(controlValue(
                      NativeCseqControlKind::MasterVolume,
                      static_cast<float>(cue->MasterVolume), sequenceFrame)),
                      0.0, 127.0)
                : 127.0;
            const double baseAmplitude = cue != nullptr
                ? velocity * velocity *
                      (static_cast<double>(cue->Volume) / 127.0) *
                      (static_cast<double>(cue->ArchiveVolume) / 127.0) *
                      (trackVolume / 127.0) * (expression / 127.0) *
                      (masterVolume / 127.0)
                : static_cast<double>(voice.Volume) / 127.0;
            const double playerGain = sequencePlayback != nullptr
                ? SequencePlayerGainAt(
                      *sequencePlayback,
                      sequencePlayback->OutputFrame + destinationFrame +
                          outputFrame)
                : voice.PlayerGain;
            const double amplitude = baseAmplitude * EnvelopeGain(voice) *
                                     modulationGain * voice.GainScale *
                                     playerGain;
            const double trackPan = cue != nullptr
                ? controlValue(NativeCseqControlKind::Pan,
                               static_cast<float>(cue->TrackPan), sequenceFrame)
                : 64.0;
            const double basePan = std::clamp(
                cue != nullptr
                    ? static_cast<double>(cue->Pan) + trackPan - 64.0
                    : static_cast<double>(voice.Pan),
                0.0, 127.0);
            const double nativePanByte = std::clamp(
                basePan + (cue != nullptr && modulationType == 2
                               ? modulationValue
                               : 0.0),
                0.0, 127.0);
            std::array<double, kMixChannels> sample{};
            if (mMixProfile) {
                const uint8_t panByte = static_cast<uint8_t>(std::clamp(
                    std::lround(nativePanByte), 0l, 127l));
                const float nativePan = NormalizeNativeAudioPan(
                    panByte, *mMixProfile) +
                    static_cast<float>(voice.SpatialPan);
                const double trackSurroundPan = cue != nullptr
                    ? controlValue(
                          NativeCseqControlKind::SurroundPan,
                          static_cast<float>(cue->TrackSurroundPan),
                          sequenceFrame)
                    : 0.0;
                const int surroundValue = cue != nullptr
                    ? static_cast<int>(cue->SurroundPan) +
                          static_cast<int>(std::lround(trackSurroundPan))
                    : 0;
                const uint8_t surroundByte = static_cast<uint8_t>(
                    std::clamp(surroundValue, 0, 127));
                const float nativeSurroundPan = std::clamp(
                    NormalizeNativeAudioSurroundPan(
                        surroundByte, *mMixProfile) +
                        static_cast<float>(voice.SpatialSurroundPan),
                    0.0f, 2.0f);
                for (uint16_t sourceChannel = 0;
                     sourceChannel < sourceChannelCount; ++sourceChannel) {
                    const double sourceSample = sourceSamples[sourceChannel];
                    const auto gains = CalculateNativeAudioChannelGains(
                        *mMixProfile, mOutputMode, sourceChannelCount,
                        sourceChannel, cue != nullptr ? cue->PanMode : 0,
                        cue != nullptr ? cue->PanCurve : 0, nativePan,
                        nativeSurroundPan);
                    sample[0] += sourceSample * amplitude * gains.FrontLeft;
                    sample[1] += sourceSample * amplitude * gains.FrontRight;
                    sample[2] += sourceSample * amplitude * gains.RearLeft;
                    sample[3] += sourceSample * amplitude * gains.RearRight;
                }
            } else {
                const double fallbackPanByte = std::clamp(
                    nativePanByte + voice.SpatialPan * 64.0, 0.0, 127.0);
                const double left = sourceSamples[0];
                const double right = sourceChannelCount == 1
                    ? sourceSamples[0] : sourceSamples[1];
                const double leftGain = fallbackPanByte <= 64.0
                    ? 1.0 : (127.0 - fallbackPanByte) / 63.0;
                const double rightGain = fallbackPanByte >= 64.0
                    ? 1.0 : fallbackPanByte / 64.0;
                sample[0] = left * amplitude * leftGain;
                sample[1] = right * amplitude * rightGain;
            }
            const double mainSend = cue != nullptr
                ? std::clamp(static_cast<double>(controlValue(
                      NativeCseqControlKind::MainSend,
                      static_cast<float>(cue->MainSend), sequenceFrame)),
                      0.0, 127.0) / 127.0
                : 1.0;
            std::array<double, 2> effectSends = cue != nullptr
                ? std::array<double, 2>{
                      std::clamp(static_cast<double>(controlValue(
                          NativeCseqControlKind::FxSendA,
                          static_cast<float>(cue->FxSendA), sequenceFrame)),
                          0.0, 127.0) / 127.0,
                      std::clamp(static_cast<double>(controlValue(
                          NativeCseqControlKind::FxSendB,
                          static_cast<float>(cue->FxSendB), sequenceFrame)),
                          0.0, 127.0) / 127.0,
                  }
                : std::array<double, 2>{0.0, 0.0};
            effectSends[0] = std::clamp(
                effectSends[0] + voice.SpatialAuxSend, 0.0, 1.0);
            for (size_t channel = 0; channel < sample.size(); ++channel) {
                const size_t outputIndex =
                    (destinationFrame + outputFrame) * kMixChannels + channel;
                accumulator[outputIndex] += sample[channel] * mainSend;
                for (size_t bus = 0; bus < auxiliary.size(); ++bus) {
                    const long routed = std::lround(
                        sample[channel] * effectSends[bus]);
                    const int32_t routedSample = static_cast<int32_t>(
                        std::clamp(
                            routed,
                            static_cast<long>(std::numeric_limits<int32_t>::min()),
                            static_cast<long>(std::numeric_limits<int32_t>::max())));
                    auxiliary[bus][outputIndex] = static_cast<int32_t>(
                        static_cast<uint32_t>(auxiliary[bus][outputIndex]) +
                        static_cast<uint32_t>(routedSample));
                }
            }
            const double pitchModulation =
                voice.Portamento.CurrentPitch +
                (cue != nullptr && modulationType == 0
                     ? modulationValue
                     : 0.0);
            const double pitchBend = cue != nullptr
                ? controlValue(NativeCseqControlKind::PitchBend,
                               static_cast<float>(cue->PitchBend), sequenceFrame)
                : 0.0;
            const double pitchBendRange = cue != nullptr
                ? controlValue(NativeCseqControlKind::PitchBendRange,
                               static_cast<float>(cue->PitchBendRange),
                               sequenceFrame)
                : 0.0;
            const double pitch = cue != nullptr
                ? std::exp2((basePitchSemitones +
                             pitchBend * pitchBendRange / 128.0 +
                             pitchModulation) / 12.0) *
                      cue->Pitch * voice.PitchScale
                : 1.0;
            voice.SourceFrame += static_cast<double>(wave.SampleRate) /
                                 static_cast<double>(mOutputSampleRate) * pitch;
            if (voice.NoteOnFramesRemaining != ~uint64_t{0} &&
                voice.NoteOnFramesRemaining != 0) {
                --voice.NoteOnFramesRemaining;
            }
            if (sequencePlayback == nullptr &&
                voice.PlayerGainFramesRemaining != 0) {
                voice.PlayerGain += voice.PlayerGainStep;
                --voice.PlayerGainFramesRemaining;
                if (voice.PlayerGainFramesRemaining == 0) {
                    voice.PlayerGain = voice.PlayerGainTarget;
                    if (voice.StopAfterPlayerFade) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    std::vector<uint64_t> completed;
    mMaximumMixedVoiceCount = std::max(
        mMaximumMixedVoiceCount, ActiveMixedVoiceCount());
    for (auto& [ownerId, voice] : mVoices) {
        if (mixVoice(voice, nullptr, frameCount, 0, 0)) {
            completed.push_back(ownerId);
        }
    }
    for (const uint64_t ownerId : completed) {
        mVoices.erase(ownerId);
    }

    const auto tickToFrame = [&](const NativeAudioSequence& sequence, uint64_t tick) {
        return SequenceTickToFrame(sequence, tick, mOutputSampleRate);
    };
    const auto advanceSequenceControls = [&](SequencePlayback& playback,
                                             uint64_t frame) {
        const NativeAudioSequence& sequence = *playback.Sequence;
        while (playback.NextControlEvent < sequence.ControlEvents.size()) {
            const NativeCseqControlEvent& control =
                sequence.ControlEvents[playback.NextControlEvent];
            const uint64_t start = tickToFrame(sequence, control.StartTick);
            if (start > frame) {
                break;
            }
            ++playback.NextControlEvent;
            SequenceControlState& state =
                control.Kind == NativeCseqControlKind::MasterVolume
                    ? playback.MasterVolumeControl
                    : playback.TrackControls[
                          static_cast<size_t>(control.Kind)][control.Track];
            state.StartValue = control.StartValue;
            state.TargetValue = control.TargetValue;
            state.StartFrame = start;
            state.EndFrame = tickToFrame(
                sequence, control.StartTick + control.DurationTicks);
            state.Active = true;
        }
    };

    std::vector<uint64_t> completedSequences;
    for (auto& [ownerId, playback] : mSequences) {
        size_t destinationFrame = 0;
        while (destinationFrame < frameCount) {
            const NativeAudioSequence& sequence = *playback.Sequence;
            const uint64_t endFrame = tickToFrame(
                sequence, sequence.EndTick);
            const uint64_t loopFrame = tickToFrame(
                sequence, sequence.LoopStartTick);
            const bool validLoop =
                sequence.Looping && endFrame > loopFrame;
            if (validLoop && playback.TimelineFrame >= endFrame) {
                if (playback.NextLoopSequence) {
                    playback.Sequence = std::move(
                        playback.NextLoopSequence);
                }
                const NativeAudioSequence& next = *playback.Sequence;
                const uint64_t nextLoopFrame = tickToFrame(
                    next, next.LoopStartTick);
                playback.TimelineFrame = nextLoopFrame;
                playback.NextControlEvent = 0;
                playback.TrackControls = {};
                playback.MasterVolumeControl = {};
                advanceSequenceControls(playback, nextLoopFrame);
                playback.NextEvent = static_cast<size_t>(std::lower_bound(
                    next.Events.begin(), next.Events.end(),
                    next.LoopStartTick,
                    [](const NativeAudioSequenceEvent& event, uint64_t tick) {
                        return event.StartTick < tick;
                    }) - next.Events.begin());
                ++playback.LoopCount;
                continue;
            }
            const uint64_t cursor = playback.TimelineFrame;
            advanceSequenceControls(playback, cursor);
            while (playback.NextEvent < sequence.Events.size()) {
                const NativeAudioSequenceEvent& event =
                    sequence.Events[playback.NextEvent];
                const uint64_t startFrame = tickToFrame(
                    sequence, event.StartTick);
                if (startFrame > cursor) {
                    break;
                }
                ++playback.NextEvent;
                if ((playback.TrackEnableMask &
                     (uint16_t{1} << event.Track)) == 0 ||
                    event.Silent || !event.Cue.Wave ||
                    event.Cue.Wave->InterleavedSamples.empty()) {
                    continue;
                }
                std::shared_ptr<const NativeAudioCue> cue(
                    playback.Sequence, &event.Cue);
                if (event.Tie) {
                    const auto tied = std::find_if(
                        playback.Voices.rbegin(), playback.Voices.rend(),
                        [&](const Voice& voice) {
                            return voice.SequenceVoice &&
                                   voice.SequenceTrack == event.Track &&
                                   voice.NoteOnFramesRemaining != 0;
                        });
                    if (tied != playback.Voices.rend()) {
                        const auto previousWave = tied->Cue->Wave;
                        if (previousWave != cue->Wave) {
                            auto tiedCue =
                                std::make_shared<NativeAudioCue>(event.Cue);
                            tiedCue->Wave = previousWave;
                            cue = std::move(tiedCue);
                        }
                        tied->Cue = std::move(cue);
                        tied->PitchScale = playback.PitchScale;
                        tied->GainScale = playback.GainScale;
                        tied->SpatialPan = playback.SpatialPan;
                        tied->SpatialSurroundPan =
                            playback.SpatialSurroundPan;
                        tied->DistanceFilter = playback.DistanceFilter;
                        tied->SpatialAuxSend = playback.SpatialAuxSend;
                        const ModulationState modulation = tied->Modulation;
                        InitializeModulation(*tied);
                        tied->Modulation = modulation;
                        if (event.DurationTicks != 0 &&
                            !event.Cue.IgnoreNoteOff) {
                            const uint64_t endFrame = tickToFrame(
                                sequence,
                                event.StartTick + event.DurationTicks);
                            tied->NoteOnFramesRemaining =
                                std::max<uint64_t>(endFrame - startFrame, 1);
                        }
                        continue;
                    }
                }
                Voice voice{std::move(cue), {}, 0.0, 127, 64};
                voice.PitchScale = playback.PitchScale;
                voice.GainScale = playback.GainScale;
                voice.SpatialPan = playback.SpatialPan;
                voice.SpatialSurroundPan = playback.SpatialSurroundPan;
                voice.DistanceFilter = playback.DistanceFilter;
                voice.SpatialAuxSend = playback.SpatialAuxSend;
                voice.SequenceTrack = event.Track;
                voice.SequenceVoice = true;
                InitializeEnvelope(voice);
                InitializeModulation(voice);
                if (event.DurationTicks != 0 && !event.Cue.IgnoreNoteOff) {
                    const uint64_t endFrame = tickToFrame(
                        sequence, event.StartTick + event.DurationTicks);
                    voice.NoteOnFramesRemaining =
                        std::max<uint64_t>(endFrame - startFrame, 1);
                }
                playback.Voices.push_back(std::move(voice));
            }

            uint64_t boundary = cursor + (frameCount - destinationFrame);
            if (playback.NextEvent < sequence.Events.size()) {
                boundary = std::min(
                    boundary, tickToFrame(
                                  sequence,
                                  sequence.Events[playback.NextEvent].StartTick));
            }
            if (playback.NextControlEvent < sequence.ControlEvents.size()) {
                boundary = std::min(
                    boundary,
                    tickToFrame(sequence,
                                sequence.ControlEvents[
                                    playback.NextControlEvent].StartTick));
            }
            if (validLoop) {
                boundary = std::min(boundary, endFrame);
            }
            if (boundary <= cursor) {
                throw std::runtime_error(
                    "native CSEQ scheduler failed to advance");
            }
            const size_t segmentFrames = static_cast<size_t>(
                boundary - cursor);
            playback.Voices.erase(
                std::remove_if(playback.Voices.begin(), playback.Voices.end(),
                               [&](Voice& voice) {
                                   return mixVoice(
                                       voice, &playback, segmentFrames,
                                       destinationFrame, cursor);
                               }),
                playback.Voices.end());
            playback.TimelineFrame = boundary;
            destinationFrame += segmentFrames;
        }
        playback.OutputFrame += frameCount;

        if (playback.Stopping &&
            playback.OutputFrame >= playback.HandleGain.EndFrame) {
            completedSequences.push_back(ownerId);
            continue;
        }

        const NativeAudioSequence& sequence = *playback.Sequence;
        const uint64_t endFrame = tickToFrame(sequence, sequence.EndTick);
        const uint64_t loopFrame = tickToFrame(
            sequence, sequence.LoopStartTick);
        const bool validLoop = sequence.Looping && endFrame > loopFrame;
        if (!validLoop && playback.NextEvent == sequence.Events.size() &&
            playback.Voices.empty()) {
            completedSequences.push_back(ownerId);
        }
    }
    for (const uint64_t ownerId : completedSequences) {
        mSequences.erase(ownerId);
    }

    for (size_t bus = 0; bus < auxiliary.size(); ++bus) {
        const auto returned = mDspEffects.ProcessBus(
            bus, auxiliary[bus], kMixChannels);
        const double returnVolume = mDspEffects.BusSettings(bus).ReturnVolume;
        for (size_t sample = 0; sample < returned.size(); ++sample) {
            accumulator[sample] += returned[sample] * returnVolume;
        }
    }

    std::vector<int16_t> output(frameCount * 2);
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t source = frame * kMixChannels;
        const size_t destination = frame * 2;
        output[destination] = static_cast<int16_t>(std::clamp(
            std::lround(accumulator[source] + accumulator[source + 2]),
            -32768l, 32767l));
        output[destination + 1] = static_cast<int16_t>(std::clamp(
            std::lround(accumulator[source + 1] + accumulator[source + 3]),
            -32768l, 32767l));
    }
    return output;
}

size_t NativeAudioMixer::ActiveVoiceCount() const {
    return mVoices.size() + mSequences.size();
}

size_t NativeAudioMixer::ActiveMixedVoiceCount() const {
    size_t count = mVoices.size();
    for (const auto& [ownerId, playback] : mSequences) {
        (void)ownerId;
        count += playback.Voices.size();
    }
    return count;
}

uint64_t NativeAudioMixer::MixedVoiceSampleCount() const {
    return mMixedVoiceSampleCount;
}

size_t NativeAudioMixer::MaximumMixedVoiceCount() const {
    return mMaximumMixedVoiceCount;
}

bool NativeAudioMixer::IsVoiceActive(uint64_t ownerId) const {
    return mVoices.contains(ownerId) || mSequences.contains(ownerId);
}

void NativeAudioMixer::StopAll() {
    mVoices.clear();
    mSequences.clear();
    mDspEffects.Reset();
    mMixedVoiceSampleCount = 0;
    mMaximumMixedVoiceCount = 0;
}

} // namespace Oot3dNativeGame
