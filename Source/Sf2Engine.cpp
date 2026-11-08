#include "Sf2Engine.h"
#include "LayerEffects.h"
#include <algorithm>
#include <cmath>

// FluidSynth exposes this helper in its synthesis library but omits it from
// the public API: unlike fluid_voice_noteoff(), it releases even under CC64.
extern "C" void fluid_voice_release(fluid_voice_t* voice);

#if JUCE_MAC
// FluidSynth 2.5.x keeps this helper exported on macOS for its JACK driver,
// but does not expose it in the public header. The public
// fluid_synth_set_sample_rate() is a documented no-op since 2.4.4. Our synths
// do not own audio drivers and JUCE guarantees that rendering is stopped while
// prepareToPlay() runs, so the immediate update is safe here and avoids
// rebuilding settings while Core Audio holds its device mutex.
extern "C" void fluid_synth_set_sample_rate_immediately(fluid_synth_t*, float);
#endif

Sf2Engine::Sf2Engine()
{
    for (auto& layer : layers)
        // Settings enumerate Core Audio devices: construct them before the
        // device callback starts, but allocate synths only for loaded layers.
        layer.settings.reset(new_fluid_settings());
}

Sf2Engine::~Sf2Engine() = default;

void Sf2Engine::createSynth(Layer& layer, double sampleRate)
{
    layer.synth.reset();
    if (!layer.settings)
        layer.settings.reset(new_fluid_settings());
    fluid_settings_setnum(layer.settings.get(), "synth.sample-rate", sampleRate);
    fluid_settings_setint(layer.settings.get(), "synth.threadsafe-api", 1);
    // A finite ceiling protects the real-time thread from runaway voice counts.
    // FluidSynth performs voice stealing when this ceiling is reached.
    fluid_settings_setint(layer.settings.get(), "synth.polyphony",
                          juce::jlimit(64, maxPolyphonyPerLayer, layer.config.polyphony));
    // FluidSynth's default headroom made SF2 layers noticeably quieter than
    // the native DX7 and Analog engines.  Raise only the synth's internal
    // calibration; the shared output limiter still prevents clipping.
    fluid_settings_setnum(layer.settings.get(), "synth.gain", 0.65);
    layer.synth.reset(new_fluid_synth(layer.settings.get()));
    if (!layer.synth) return;
    // Room processing belongs to the Classic layer reverb. Disable the unused
    // FluidSynth return as well as CC91 sends so its DSP cannot run in parallel.
    fluid_synth_reverb_on(layer.synth.get(), -1, 0);
    // Keep legacy SF2 patches dry until this layer explicitly requests chorus.
    fluid_synth_chorus_on(layer.synth.get(), -1, 0);
    layer.soundFontId = -1;
    layer.monoNote = -1;
    layer.monoChannel = 1;
    layer.sustainDown.fill(false);
    layer.heldNotes.fill(false);
    layer.heldVelocities.fill(0.0f);
    layer.heldNoteOrder.fill(0);
    layer.heldNoteSequence = 0;
    layer.modulationAmount = 0.0f;
    layer.modulationPhase = 0.0;
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
    layer.lastCutoff = -1;
    layer.lastReverb = -1;
    layer.lastAppliedAttackTimecents = -100000.0f;
    layer.lastAppliedDecayTimecents = -100000.0f;
    layer.lastAppliedSustainCentibels = -100000.0f;
    layer.lastPortamento = -1;
    layer.lastMono = -1;
    layer.lastAppliedReleaseTimecents = -100000.0f;
    layer.lastMuteVolumeChannels.fill(0);
    layer.filterState = { 0.0f, 0.0f };
    layer.highPassInput = { 0.0f, 0.0f };
    layer.highPassOutput = { 0.0f, 0.0f };
    layer.lowPassState = { 0.0f, 0.0f };
    layer.gain.reset(sampleRate, 0.02);
    layer.gain.setCurrentAndTargetValue(layer.config.gain);
    layer.compressorEnvelope = { 0.0f, 0.0f };
    layer.eq.reset();
    layer.nativeReverb.reset();
    layer.lastReverbParameters.fill(-1.0f);
    layer.lastChorusParameters.fill(-1.0f);
}

void Sf2Engine::prepare(double sampleRate, int maximumBlockSize)
{
    const juce::ScopedLock guard(lock);
    ++prepareGeneration;
    currentSampleRate = sampleRate;
    currentBlockSize = maximumBlockSize;
    scratch.setSize(2, maximumBlockSize, false, false, true);

    for (auto& layer : layers)
    {
        layer.nativeReverb.setSampleRate(currentSampleRate);
        layer.nativeReverb.reset();
        layer.effectTailPending = false;
        layer.effectQuietSamples = 0;
        layer.lastReverbParameters.fill(-1.0f);
        layer.compressorEnvelope = { 0.0f, 0.0f };
        layer.gain.reset(currentSampleRate, 0.02);
        layer.gain.setCurrentAndTargetValue(layer.config.gain);
#if JUCE_WINDOWS
        if (!layer.synth)
            continue;
        // The internal immediate-rate helper is not exported by the Windows
        // FluidSynth library. Rebuild the synth at the requested device rate
        // and restore its SoundFont/preset instead of linking a private symbol.
        const auto soundFontPath = layer.soundFontPath;
        const auto selectedBank = layer.selectedBank;
        const auto selectedProgram = layer.selectedProgram;
        createSynth(layer, currentSampleRate);

        if (soundFontPath.isNotEmpty())
        {
            const juce::File soundFont(soundFontPath);
            if (soundFont.existsAsFile())
            {
                const auto id = fluid_synth_sfload(layer.synth.get(),
                                                   soundFont.getFullPathName().toRawUTF8(), 1);
                if (id >= 0)
                {
                    layer.soundFontId = id;
                    layer.soundFontPath = soundFont.getFullPathName();
                    for (int channel = 0; channel < 16; ++channel)
                    {
                        fluid_synth_bank_select(layer.synth.get(), channel, selectedBank);
                        fluid_synth_program_change(layer.synth.get(), channel, selectedProgram);
                    }
                }
                else
                    layer.soundFontPath.clear();
            }
            else
                layer.soundFontPath.clear();
        }
#elif JUCE_MAC
        // Never call new_fluid_settings()/new_fluid_synth() from
        // prepareToPlay(). In JUCE's standalone wrapper this method runs while
        // AudioProcessorPlayer owns its callback mutex. Creating FluidSynth
        // settings queries Core Audio outputs, producing an AB/BA deadlock
        // with the already-running Core Audio IO thread.
        if (layer.synth != nullptr)
            fluid_synth_set_sample_rate_immediately(layer.synth.get(),
                                                    static_cast<float>(currentSampleRate));
#else
        // On platforms where FluidSynth does not export the private immediate
        // helper, the synth was already created with currentSampleRate.
        // Windows rebuilds explicitly above because its device rate may change.
        juce::ignoreUnused(layer);
#endif
    }
}

void Sf2Engine::reset()
{
    const juce::ScopedLock guard(lock);
    for (auto& layer : layers)
        if (layer.synth)
        {
            fluid_synth_system_reset(layer.synth.get());
            layer.lastChorusParameters.fill(-1.0f);
            layer.filterState = { 0.0f, 0.0f };
            layer.highPassInput = { 0.0f, 0.0f };
            layer.highPassOutput = { 0.0f, 0.0f };
            layer.lowPassState = { 0.0f, 0.0f };
            layer.monoNote = -1;
            layer.monoChannel = 1;
            layer.sustainDown.fill(false);
            layer.heldNotes.fill(false);
            layer.heldVelocities.fill(0.0f);
            layer.heldNoteOrder.fill(0);
            layer.heldNoteSequence = 0;
        }
}

struct Sf2Engine::PreparedSoundFont
{
    Layer replacement;
    int targetLayer = -1;
    double sampleRate = 48000.0;
    uint64_t requestGeneration = 0, audioGeneration = 0;
    bool installed = false;
};

juce::Result Sf2Engine::prepareSoundFont(int index, const juce::File& file,
                                       std::shared_ptr<PreparedSoundFont>& prepared)
{
    prepared.reset();
    if (!juce::isPositiveAndBelow(index, layerCount))
        return juce::Result::fail("Layer SF2 invalida.");
    if (!file.existsAsFile())
        return juce::Result::fail("Arquivo SF2 nao encontrado.");

    auto pending = std::make_shared<PreparedSoundFont>();
    {
        const juce::ScopedLock guard(lock);
        pending->targetLayer = index;
        pending->sampleRate = currentSampleRate;
        pending->audioGeneration = prepareGeneration;
        pending->requestGeneration = ++loadGenerations[(size_t) index];
        pending->replacement.config = layers[(size_t) index].config;
    }
    // Disk reads, sample allocation and synth creation must not hold either
    // the engine rendering lock or the processor callback lock.
    auto& replacement = pending->replacement;
    createSynth(replacement, pending->sampleRate);
    if (!replacement.synth)
        return juce::Result::fail("Nao foi possivel criar o motor SF2.");
    const auto id = fluid_synth_sfload(replacement.synth.get(), file.getFullPathName().toRawUTF8(), 1);
    if (id < 0)
        return juce::Result::fail("FluidSynth nao conseguiu abrir este SF2. O som atual foi mantido.");
    replacement.soundFontId = id;
    replacement.soundFontPath = file.getFullPathName();
    prepared = std::move(pending);
    return juce::Result::ok();
}

juce::Result Sf2Engine::installSoundFont(int index, const std::shared_ptr<PreparedSoundFont>& pending)
{
    if (!pending || pending->targetLayer != index || !juce::isPositiveAndBelow(index, layerCount))
        return juce::Result::fail("Carregamento SF2 invalido.");
    const juce::ScopedLock guard(lock);
    if (pending->installed || pending->requestGeneration != loadGenerations[(size_t) index])
        return juce::Result::fail("O carregamento SF2 foi substituido por outra troca de instrumento.");
    if (pending->audioGeneration != prepareGeneration)
        return juce::Result::fail("O audio foi reconfigurado durante a leitura. Carregue o SF2 novamente.");

    auto& layer = layers[(size_t) index];
    auto& replacement = pending->replacement;
    if (replacement.config.polyphony != layer.config.polyphony)
        fluid_synth_set_polyphony(replacement.synth.get(),
            juce::jlimit(64, maxPolyphonyPerLayer, layer.config.polyphony));
    // Retire the old synth into the prepared object: its destructor runs after
    // the caller releases the callback lock, never in the audio callback.
    layer.synth.swap(replacement.synth);
    layer.settings.swap(replacement.settings);
    layer.soundFontPath.swapWith(replacement.soundFontPath);
    layer.soundFontId = replacement.soundFontId;
    layer.selectedBank = 0;
    layer.selectedProgram = 0;
    layer.monoNote = -1;
    layer.monoChannel = 1;
    layer.sustainDown.fill(false);
    layer.heldNotes.fill(false);
    layer.heldVelocities.fill(0.0f);
    layer.heldNoteOrder.fill(0);
    layer.heldNoteSequence = 0;
    layer.filterState = { 0.0f, 0.0f };
    layer.highPassInput = { 0.0f, 0.0f };
    layer.highPassOutput = { 0.0f, 0.0f };
    layer.lowPassState = { 0.0f, 0.0f };
    layer.modulationAmount = 0.0f;
    layer.modulationPhase = 0.0;
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
    layer.lastCutoff = layer.lastReverb = layer.lastPortamento = layer.lastMono = -1;
    layer.lastAppliedAttackTimecents = layer.lastAppliedDecayTimecents = -100000.0f;
    layer.lastAppliedSustainCentibels = layer.lastAppliedReleaseTimecents = -100000.0f;
    layer.lastMuteVolumeChannels.fill(0);
    layer.lastChorusParameters.fill(-1.0f);
    layer.lastReverbParameters.fill(-1.0f);
    layer.compressorEnvelope = { 0.0f, 0.0f };
    layer.eq.reset();
    layer.nativeReverb.reset();
    pending->installed = true;
    return juce::Result::ok();
}

juce::Result Sf2Engine::loadSoundFont(int index, const juce::File& file)
{
    std::shared_ptr<PreparedSoundFont> prepared;
    const auto result = prepareSoundFont(index, file, prepared);
    if (result.failed()) return result;
    return installSoundFont(index, prepared);
}

void Sf2Engine::stopAllSounds()
{
    const juce::ScopedLock guard(lock);
    for (auto& layer : layers)
    {
        if (layer.synth == nullptr) continue;
        for (int channel = 0; channel < 16; ++channel)
        {
            fluid_synth_cc(layer.synth.get(), channel, 64, 0);
            fluid_synth_all_notes_off(layer.synth.get(), channel);
            fluid_synth_all_sounds_off(layer.synth.get(), channel);
        }
        layer.monoNote = -1;
        layer.monoChannel = 1;
        layer.sustainDown.fill(false);
        layer.heldNotes.fill(false);
        layer.heldVelocities.fill(0.0f);
        layer.heldNoteOrder.fill(0);
        layer.heldNoteSequence = 0;
    }
}

void Sf2Engine::unloadSoundFont(int index)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    ++loadGenerations[(size_t) index];
    auto& layer = layers[(size_t) index];
    if (layer.synth && layer.soundFontId >= 0)
    {
        for (int channel = 0; channel < 16; ++channel)
        {
            fluid_synth_cc(layer.synth.get(), channel, 64, 0);
            fluid_synth_all_notes_off(layer.synth.get(), channel);
            fluid_synth_all_sounds_off(layer.synth.get(), channel);
        }
        layer.monoNote = -1;
        fluid_synth_sfunload(layer.synth.get(), layer.soundFontId, 1);
    }
    layer.soundFontId = -1;
    layer.soundFontPath.clear();
    layer.selectedBank = 0;
    layer.selectedProgram = 0;
    layer.monoNote = -1;
    layer.monoChannel = 1;
    layer.sustainDown.fill(false);
    layer.heldNotes.fill(false);
    layer.heldVelocities.fill(0.0f);
    layer.heldNoteOrder.fill(0);
    layer.heldNoteSequence = 0;
    layer.modulationAmount = 0.0f;
    layer.modulationPhase = 0.0;
    layer.effectTailPending = false;
    layer.effectQuietSamples = 0;
}

void Sf2Engine::dispatchMidi(Layer& layer, const juce::MidiMessage& message)
{
    const auto channel = message.getChannel();
    if (layer.config.midiChannel != 0 && channel != layer.config.midiChannel)
        return;

    const auto monoMode = layer.config.mono || layer.config.portamento;
    const auto heldIndex = [] (int midiChannel, int note)
    {
        return (size_t) (midiChannel - 1) * 128u + (size_t) note;
    };
    const auto highestHeld = [&layer] ()
    {
        size_t best = layer.heldNotes.size();
        uint64_t newest = 0;
        for (size_t i = 0; i < layer.heldNotes.size(); ++i)
            if (layer.heldNotes[i] && layer.heldNoteOrder[i] >= newest)
            {
                best = i;
                newest = layer.heldNoteOrder[i];
            }
        return best;
    };
    const auto startMonoNote = [&layer] (int midiChannel, int note, float velocity)
    {
        const auto velocityValue = juce::jlimit(1, 127, (int) std::round(velocity * 127.0f));

        // A repeated Note On for the same key must replace the current
        // FluidSynth voice first. Otherwise some monophonic SF2s retain the
        // previous sax/brass voice until that pitch is played again.
        if (layer.monoNote == note)
            fluid_synth_noteoff(layer.synth.get(), layer.monoChannel - 1, layer.monoNote);

        fluid_synth_noteon(layer.synth.get(), midiChannel - 1, note, velocityValue);

        // For different pitches, start the new note before releasing the old
        // one, preserving the continuous legato transition.
        if (layer.monoNote >= 0 && layer.monoNote != note)
            fluid_synth_noteoff(layer.synth.get(), layer.monoChannel - 1, layer.monoNote);

        layer.monoNote = note;
        layer.monoChannel = midiChannel;
    };

    if (message.isController())
    {
        const auto cc = message.getControllerNumber();
        const auto value = message.getControllerValue();
        // Mixer faders own channel volume and expression. Forwarding CC7/CC11
        // to FluidSynth lets one physical fader silence unrelated SF2 layers
        // that happen to listen on the same MIDI channel.
        if (cc == 7 || cc == 11) return;
        if (cc == layer.config.muteCC
            && (layer.config.muteCCChannel < 0 || channel == layer.config.muteCCChannel)) return;
        if (cc == 64)
        {
            if (!layer.config.sustainEnabled) return;
            const auto channelIndex = (size_t) (channel - 1);
            const auto wasDown = layer.sustainDown[channelIndex];
            const auto isDown = value >= 64;
            layer.sustainDown[channelIndex] = isDown;
            // In mono/legato mode sustain is controlled exclusively by the
            // note stack below. Always neutralize FluidSynth's own CC64 state:
            // it may have been left down before the layer entered legato mode.
            if (!monoMode)
                fluid_synth_cc(layer.synth.get(), channel - 1, cc, value);
            else
                fluid_synth_cc(layer.synth.get(), channel - 1, 64, 0);
            if (wasDown && !isDown && monoMode && layer.monoNote >= 0
                && layer.monoChannel == channel)
            {
                const auto fallback = highestHeld();
                if (fallback < layer.heldNotes.size())
                {
                    const auto fallbackNote = (int) (fallback % 128u);
                    const auto fallbackChannel = (int) (fallback / 128u) + 1;
                    if (fallbackNote != layer.monoNote || fallbackChannel != layer.monoChannel)
                        startMonoNote(fallbackChannel, fallbackNote, layer.heldVelocities[fallback]);
                }
                else
                {
                    fluid_synth_noteoff(layer.synth.get(), layer.monoChannel - 1, layer.monoNote);
                    layer.monoNote = -1;
                }
            }
            return;
        }
        if (cc == 1)
            layer.modulationAmount = layer.config.modulationEnabled ? value / 127.0f : 0.0f;
        const auto forwardedValue = (cc == 1 && !layer.config.modulationEnabled) ? 0 : value;
        fluid_synth_cc(layer.synth.get(), channel - 1, cc, forwardedValue);
        return;
    }

    if (message.isAllSoundOff())
    {
        const auto first = (size_t) (channel - 1) * 128u;
        std::fill(layer.heldNotes.begin() + first, layer.heldNotes.begin() + first + 128, false);
        std::fill(layer.heldVelocities.begin() + first, layer.heldVelocities.begin() + first + 128, 0.0f);
        std::fill(layer.heldNoteOrder.begin() + first, layer.heldNoteOrder.begin() + first + 128, 0);
        layer.sustainDown[(size_t) (channel - 1)] = false;
        fluid_synth_cc(layer.synth.get(), channel - 1, 64, 0);
        fluid_synth_all_sounds_off(layer.synth.get(), channel - 1);
        if (monoMode && layer.monoChannel == channel)
        {
            const auto fallback = highestHeld();
            if (fallback < layer.heldNotes.size())
                startMonoNote((int) (fallback / 128u) + 1, (int) (fallback % 128u),
                              layer.heldVelocities[fallback]);
            else
                layer.monoNote = -1;
        }
        return;
    }
    if (message.isAllNotesOff())
    {
        const auto first = (size_t) (channel - 1) * 128u;
        std::fill(layer.heldNotes.begin() + first, layer.heldNotes.begin() + first + 128, false);
        std::fill(layer.heldVelocities.begin() + first, layer.heldVelocities.begin() + first + 128, 0.0f);
        std::fill(layer.heldNoteOrder.begin() + first, layer.heldNoteOrder.begin() + first + 128, 0);
        layer.sustainDown[(size_t) (channel - 1)] = false;
        fluid_synth_cc(layer.synth.get(), channel - 1, 64, 0);
        fluid_synth_all_notes_off(layer.synth.get(), channel - 1);
        if (monoMode && layer.monoChannel == channel)
        {
            const auto fallback = highestHeld();
            if (fallback < layer.heldNotes.size())
                startMonoNote((int) (fallback / 128u) + 1, (int) (fallback % 128u),
                              layer.heldVelocities[fallback]);
            else
                layer.monoNote = -1;
        }
        return;
    }

    if (!message.isNoteOnOrOff())
    {
        if (message.isPitchWheel() && layer.config.pitchBendEnabled)
            fluid_synth_pitch_bend(layer.synth.get(), channel - 1, message.getPitchWheelValue());
        else if (message.isProgramChange())
            fluid_synth_program_change(layer.synth.get(), channel - 1, message.getProgramChangeNumber());
        return;
    }

    const auto sourceNote = message.getNoteNumber();
    if (sourceNote < layer.config.lowNote || sourceNote > layer.config.highNote)
        return;
    const auto note = juce::jlimit(0, 127, sourceNote + layer.config.octave * 12);

    if (message.isNoteOn())
    {
        if (!layer.config.acceptsVelocity((int) message.getVelocity())) return;
        auto velocity = layer.config.velocitySensitivityOff ? (100.0f / 127.0f)
            : juce::jlimit(0.0f, 1.0f, message.getFloatVelocity());
        if (!layer.config.velocitySensitivityOff && layer.config.velocityCurve == 1) velocity = std::sqrt(velocity);
        else if (!layer.config.velocitySensitivityOff && layer.config.velocityCurve == 2) velocity *= velocity;
        else if (!layer.config.velocitySensitivityOff && layer.config.velocityCurve == 4)
            velocity = layer.config.velocityTable[(size_t) message.getVelocity()] / 127.0f;

        if (!monoMode)
        {
            std::array<unsigned int, maxPolyphonyPerLayer> priorSameNoteIds {};
            std::array<fluid_voice_t*, maxPolyphonyPerLayer> voices {};
            size_t priorSameNoteCount = 0;
            const auto channelIndex = (size_t) (channel - 1);
            if (layer.sustainDown[channelIndex])
            {
                fluid_synth_get_voicelist(layer.synth.get(), voices.data(), (int) voices.size(), -1);
                for (auto* voice : voices)
                    if (voice != nullptr && fluid_voice_is_playing(voice)
                        && fluid_voice_get_channel(voice) == channel - 1
                        && fluid_voice_get_key(voice) == note)
                        priorSameNoteIds[priorSameNoteCount++] = fluid_voice_get_id(voice);
            }
            std::sort(priorSameNoteIds.begin(), priorSameNoteIds.begin()
                      + (std::ptrdiff_t) priorSameNoteCount);
            fluid_synth_noteon(layer.synth.get(), channel - 1, note,
                               juce::jlimit(1, 127, (int) std::round(velocity * 127.0f)));
            if (priorSameNoteCount > 0)
            {
                fluid_synth_get_voicelist(layer.synth.get(), voices.data(), (int) voices.size(), -1);
                for (auto* voice : voices)
                {
                    if (voice == nullptr || !fluid_voice_is_playing(voice)
                        || fluid_voice_get_channel(voice) != channel - 1
                        || fluid_voice_get_key(voice) != note)
                        continue;
                    const auto voiceId = fluid_voice_get_id(voice);
                    if (std::binary_search(priorSameNoteIds.begin(),
                                           priorSameNoteIds.begin() + (std::ptrdiff_t) priorSameNoteCount,
                                           voiceId))
                        fluid_voice_release(voice);
                }
            }
            return;
        }

        const auto index = heldIndex(channel, note);
        layer.heldNotes[index] = true;
        layer.heldVelocities[index] = velocity;
        layer.heldNoteOrder[index] = ++layer.heldNoteSequence;
        // The next note starts before the old one is released, producing a
        // continuous legato change rather than a silent gap.
        startMonoNote(channel, note, velocity);
        return;
    }

    if (!monoMode)
    {
        fluid_synth_noteoff(layer.synth.get(), channel - 1, note);
        return;
    }

    const auto index = heldIndex(channel, note);
    if (!layer.heldNotes[index]) return;
    layer.heldNotes[index] = false;
    if (note != layer.monoNote || channel != layer.monoChannel) return;
    if (layer.sustainDown[(size_t) (channel - 1)]) return;

    const auto fallback = highestHeld();
    if (fallback < layer.heldNotes.size())
        startMonoNote((int) (fallback / 128u) + 1, (int) (fallback % 128u),
                      layer.heldVelocities[fallback]);
    else
    {
        fluid_synth_noteoff(layer.synth.get(), layer.monoChannel - 1, layer.monoNote);
        layer.monoNote = -1;
    }
}

void Sf2Engine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                        const std::array<juce::MidiBuffer, layerCount>* routedMidi)
{
    const juce::ScopedLock guard(lock);
    output.clear();
    scratch.setSize(2, output.getNumSamples(), false, false, true);
    std::array<std::array<bool, 128>, 16> layerControlControllers {};
    for (const auto& target : layers)
        for (size_t control = 0; control < target.config.learnedControlCCs.size(); ++control)
        {
            const auto cc = target.config.learnedControlCCs[control];
            const auto channel = target.config.learnedControlChannels[control];
            if (!juce::isPositiveAndBelow(cc, 128)) continue;
            if (channel < 1 || channel > 16)
                for (auto& channelControllers : layerControlControllers)
                    channelControllers[(size_t) cc] = true;
            else
                layerControlControllers[(size_t) channel - 1][(size_t) cc] = true;
        }
    const auto isLayerControl = [&layerControlControllers](const juce::MidiMessage& message)
    {
        return message.isController() && message.getControllerNumber() != 64
            && layerControlControllers[(size_t) message.getChannel() - 1]
                                       [(size_t) message.getControllerNumber()];
    };
    const auto eventTargetsLayer = [&isLayerControl](const Layer& layer,
                                                       const juce::MidiMessage& message)
    {
        const auto channel = message.getChannel();
        if (channel < 1 || channel > 16 || isLayerControl(message)
            || (layer.config.midiChannel != 0 && channel != layer.config.midiChannel))
            return false;

        if (message.isNoteOnOrOff())
        {
            const auto note = message.getNoteNumber();
            if (note < layer.config.lowNote || note > layer.config.highNote)
                return false;
            if (message.isNoteOn() && !layer.config.acceptsVelocity((int) message.getVelocity()))
                return false;
        }
        return true;
    };
    std::array<unsigned int, 2> muteVolumeChannels {};
    for (const auto& target : layers)
        if (target.config.muteCC == 7 || target.config.muteCC == 11)
            muteVolumeChannels[target.config.muteCC == 7 ? 0u : 1u] |=
                target.config.muteCCChannel < 1 ? 0xffffu : 1u << (target.config.muteCCChannel - 1);

    for (auto& layer : layers)
    {
        if (!layer.config.enabled || layer.soundFontId < 0 || !layer.synth)
        {
            layer.peak.store(layer.peak.load(std::memory_order_relaxed) * 0.82f,
                             std::memory_order_relaxed);
            continue;
        }

        const auto cutoff = juce::jlimit(0, 127, static_cast<int>(std::round(layer.config.cutoff * 1.27f)));
        if (layer.lastMuteVolumeChannels != muteVolumeChannels)
        {
            // Recover every SF2 on this channel, including other layers which
            // previously received a Mute CC as expression/volume by mistake.
            for (size_t control = 0; control < 2; ++control)
                for (int channel = 0; channel < 16; ++channel)
                    if (((layer.lastMuteVolumeChannels[control] | muteVolumeChannels[control])
                          & (1u << channel)) != 0)
                        fluid_synth_cc(layer.synth.get(), channel, control == 0 ? 7 : 11,
                                       control == 0 ? 100 : 127);
            layer.lastMuteVolumeChannels = muteVolumeChannels;
        }
        // Reverb is applied as a native post-SF2 effect below. Keep the
        // SoundFont's own generator send dry, so its quality is independent
        // of each library's embedded reverb setup.
        const auto reverb = 0;
        const auto portamento = layer.config.portamento ? 127 : 0;
        // Portamento is always monophonic. Mono Legato uses the same single
        // voice policy but retains the instantaneous pitch transition.
        const auto mono = (layer.config.mono || layer.config.portamento) ? 1 : 0;
        if (cutoff != layer.lastCutoff || reverb != layer.lastReverb
            || portamento != layer.lastPortamento || mono != layer.lastMono)
        {
            for (int channel = 0; channel < 16; ++channel)
            {
                if (cutoff != layer.lastCutoff) fluid_synth_cc(layer.synth.get(), channel, 74, cutoff);
                if (reverb != layer.lastReverb) fluid_synth_cc(layer.synth.get(), channel, 91, reverb);
                if (portamento != layer.lastPortamento)
                {
                    fluid_synth_cc(layer.synth.get(), channel, 65, portamento);
                    fluid_synth_cc(layer.synth.get(), channel, 5, 32);
                }
                if (mono != layer.lastMono)
                {
                    // Mono/legato is implemented by this engine's note stack.
                    // Do not also enable FluidSynth CC126/127 mode: combining
                    // both allocators leaves voices sustained on some SF2s.
                    fluid_synth_all_sounds_off(layer.synth.get(), channel);
                    // Clear any sustain latch left in FluidSynth before our
                    // own mono/legato note stack takes control.
                    fluid_synth_cc(layer.synth.get(), channel, 64, 0);
                    layer.heldNotes.fill(false);
                    layer.heldVelocities.fill(0.0f);
                    layer.heldNoteOrder.fill(0);
                    layer.heldNoteSequence = 0;
                    layer.monoNote = -1;
                    layer.monoChannel = 1;
                    layer.sustainDown.fill(false);
                }
            }
            layer.lastCutoff = cutoff;
            layer.lastReverb = reverb;
            layer.lastPortamento = portamento;
            layer.lastMono = mono;
        }

        // Each SF2 layer owns a separate FluidSynth instance, so these chorus
        // settings cannot leak into another layer. Apply only on changes; a
        // held pedal otherwise makes repeated effect reconfiguration costly.
        const std::array<float, 3> chorusValues {
            juce::jlimit(0.0f, 100.0f, layer.config.sf2Chorus),
            juce::jlimit(0.1f, 5.0f, layer.config.sf2ChorusRate),
            juce::jlimit(0.0f, 20.0f, layer.config.sf2ChorusDepth)
        };
        if (chorusValues != layer.lastChorusParameters)
        {
            const auto enabled = chorusValues[0] > 0.0f;
            if (enabled)
            {
                fluid_synth_set_chorus_group_nr(layer.synth.get(), -1, 3);
                fluid_synth_set_chorus_group_level(layer.synth.get(), -1,
                                                  chorusValues[0] * 0.0065);
                fluid_synth_set_chorus_group_speed(layer.synth.get(), -1, chorusValues[1]);
                fluid_synth_set_chorus_group_depth(layer.synth.get(), -1, chorusValues[2]);
            }
            fluid_synth_chorus_on(layer.synth.get(), -1, enabled ? 1 : 0);
            for (int channel = 0; channel < 16; ++channel)
                fluid_synth_cc(layer.synth.get(), channel, 93, enabled ? 127 : 0);
            layer.lastChorusParameters = chorusValues;
        }

        // Set the actual voice envelope in timecents instead of relying on
        // optional SF2 CC modulators. The shared attack preserves its legacy
        // 0..50 ms range and extends to 2 s; release extends to 10 s.
        const auto attackSeconds = juce::jmax(0.001, layerAttackSeconds(layer.config.attack));
        const auto attackTimecents = 1200.0f * std::log2(static_cast<float>(attackSeconds));
        const auto releaseSeconds = layerReleaseSeconds(layer.config.release);
        const auto releaseTimecents = 1200.0f * std::log2(releaseSeconds);
        const auto customDecay = layer.config.decayMs > 0.0f || layer.config.sustainLevel < 100.0f;
        const auto decayTimecents = 1200.0f * std::log2(juce::jmax(0.001f, layer.config.decayMs * 0.001f));
        const auto sustainCentibels = juce::jlimit(0.0f, 1440.0f,
            -200.0f * std::log10(juce::jmax(0.000001f, layer.config.sustainLevel * 0.01f)));
        const auto updateAttack = attackTimecents != layer.lastAppliedAttackTimecents;
        const auto updateRelease = releaseTimecents != layer.lastAppliedReleaseTimecents;
        const auto updateDecayAndSustain = customDecay
            && (decayTimecents != layer.lastAppliedDecayTimecents
                || sustainCentibels != layer.lastAppliedSustainCentibels);
        layer.lastAppliedAttackTimecents = attackTimecents;
        layer.lastAppliedReleaseTimecents = releaseTimecents;
        layer.lastAppliedDecayTimecents = customDecay ? decayTimecents : -100000.0f;
        layer.lastAppliedSustainCentibels = customDecay ? sustainCentibels : -100000.0f;
        bool startedVoice = false;
        for (const auto metadata : midi)
        {
            if (!eventTargetsLayer(layer, metadata.getMessage())) continue;
            startedVoice |= metadata.getMessage().isNoteOn();
            dispatchMidi(layer, metadata.getMessage());
        }
        if (routedMidi != nullptr)
            for (const auto metadata : (*routedMidi)[static_cast<size_t>(&layer - layers.data())])
            {
                if (!eventTargetsLayer(layer, metadata.getMessage())) continue;
                startedVoice |= metadata.getMessage().isNoteOn();
                dispatchMidi(layer, metadata.getMessage());
            }

        // Update all required envelope generators in a single pass. New pad
        // attacks previously caused up to three full voice-list scans in one
        // audio block (plus separate scans when the envelope settings changed),
        // which creates an avoidable CPU spike on the realtime thread.
        if (startedVoice || updateAttack || updateRelease || updateDecayAndSustain)
        {
            std::array<fluid_voice_t*, maxPolyphonyPerLayer> voices {};
            fluid_synth_get_voicelist(layer.synth.get(), voices.data(), (int) voices.size(), -1);
            for (auto* voice : voices)
            {
                if (voice == nullptr) continue;
                if ((startedVoice || updateAttack)
                    && fluid_voice_gen_get(voice, GEN_VOLENVATTACK) != attackTimecents)
                {
                    fluid_voice_gen_set(voice, GEN_VOLENVATTACK, attackTimecents);
                    fluid_voice_update_param(voice, GEN_VOLENVATTACK);
                }
                if ((startedVoice || updateRelease)
                    && fluid_voice_gen_get(voice, GEN_VOLENVRELEASE) != releaseTimecents)
                {
                    fluid_voice_gen_set(voice, GEN_VOLENVRELEASE, releaseTimecents);
                    fluid_voice_update_param(voice, GEN_VOLENVRELEASE);
                }
                if (customDecay && (startedVoice || updateDecayAndSustain))
                {
                    if (fluid_voice_gen_get(voice, GEN_VOLENVDECAY) != decayTimecents)
                    {
                        fluid_voice_gen_set(voice, GEN_VOLENVDECAY, decayTimecents);
                        fluid_voice_update_param(voice, GEN_VOLENVDECAY);
                    }
                    if (fluid_voice_gen_get(voice, GEN_VOLENVSUSTAIN) != sustainCentibels)
                    {
                        fluid_voice_gen_set(voice, GEN_VOLENVSUSTAIN, sustainCentibels);
                        fluid_voice_update_param(voice, GEN_VOLENVSUSTAIN);
                    }
                }
            }
        }

        // MIDI controls such as modulation, program changes and sustain must
        // still be dispatched above, but they do not require an audio render
        // until a voice or an effect tail actually produces sound. Previously
        // any event on any channel forced every loaded SF2 instance to render
        // a silent block.
        if (fluid_synth_get_active_voice_count(layer.synth.get()) == 0 && !layer.effectTailPending)
        {
            layer.gain.setTargetValue(juce::jmax(0.0f, layer.config.gain));
            layer.gain.skip(output.getNumSamples());
            layer.peak.store(0.0f, std::memory_order_relaxed);
            continue;
        }
        scratch.clear();
        fluid_synth_write_float(layer.synth.get(), output.getNumSamples(),
                                scratch.getWritePointer(0), 0, 1,
                                scratch.getWritePointer(1), 0, 1);

        // CC1 uses category-specific effects without adding knobs or changing
        // the approved front-end: tremolo for electric pianos and fast rotary
        // movement for organs. CC1 at zero keeps the layer dry.
        const auto mod = layer.config.modulationEnabled
            ? juce::jlimit(0.0f, 1.0f, layer.modulationAmount) : 0.0f;
        if (mod > 0.001f)
        {
            const auto category = juce::File { layer.soundFontPath }
                .getParentDirectory().getFileName();
            if (category == "Piano Eletrico" || category == "Organ")
            {
                const auto tremolo = category == "Piano Eletrico";
                const auto speedHz = tremolo ? 5.0f : 6.8f;
                const auto phaseStep = juce::MathConstants<double>::twoPi * speedHz / currentSampleRate;
                auto* left = scratch.getWritePointer(0);
                auto* right = scratch.getWritePointer(1);
                for (int sample = 0; sample < output.getNumSamples(); ++sample)
                {
                    const auto wave = static_cast<float>(std::sin(layer.modulationPhase));
                    layer.modulationPhase += phaseStep;
                    if (layer.modulationPhase >= juce::MathConstants<double>::twoPi)
                        layer.modulationPhase -= juce::MathConstants<double>::twoPi;

                    if (tremolo)
                    {
                        const auto gain = 1.0f - mod * 0.58f * (0.5f + 0.5f * wave);
                        left[sample] *= gain;
                        right[sample] *= gain;
                    }
                    else
                    {
                        const auto pan = wave * mod * 0.62f;
                        left[sample] *= 1.0f - pan;
                        right[sample] *= 1.0f + pan;
                    }
                }
            }
        }

        const auto cutoffAmount = juce::jlimit(0.0f, 100.0f, layer.config.cutoff);
        if (cutoffAmount < 99.5f)
        {
            const auto frequency = 80.0f * std::pow(250.0f, cutoffAmount / 100.0f);
            const auto coefficient = std::exp(-juce::MathConstants<float>::twoPi * frequency
                                              / static_cast<float>(currentSampleRate));
            for (int channel = 0; channel < scratch.getNumChannels(); ++channel)
            {
                auto state = layer.filterState[(size_t) channel];
                auto* samples = scratch.getWritePointer(channel);
                for (int sample = 0; sample < output.getNumSamples(); ++sample)
                {
                    state = (1.0f - coefficient) * samples[sample] + coefficient * state;
                    samples[sample] = state;
                }
                layer.filterState[(size_t) channel] = state;
            }
        }

        // These filters are independent from the synthesizer CUTOFF control:
        // they are useful for carving space around a SoundFont or a continuous
        // pad without changing its preset. First-order filters are cheap and
        // stable enough to run once per layer in the audio callback.
        const auto highPassHz = juce::jlimit(20.0f, 1000.0f, layer.config.highPassHz);
        const auto lowPassHz = juce::jlimit(1000.0f, 20000.0f, layer.config.lowPassHz);
        const auto highPassEnabled = highPassHz > 20.5f;
        const auto lowPassEnabled = lowPassHz < 19950.0f;
        const auto highPassCoefficient = std::exp(-juce::MathConstants<float>::twoPi * highPassHz
                                                  / static_cast<float>(currentSampleRate));
        const auto lowPassCoefficient = std::exp(-juce::MathConstants<float>::twoPi * lowPassHz
                                                 / static_cast<float>(currentSampleRate));
        if (highPassEnabled || lowPassEnabled)
        {
            for (int channel = 0; channel < scratch.getNumChannels(); ++channel)
            {
                auto previousInput = layer.highPassInput[(size_t) channel];
                auto previousOutput = layer.highPassOutput[(size_t) channel];
                auto previousLowPass = layer.lowPassState[(size_t) channel];
                auto* samples = scratch.getWritePointer(channel);
                for (int sample = 0; sample < output.getNumSamples(); ++sample)
                {
                    auto value = samples[sample];
                    if (highPassEnabled)
                    {
                        const auto filtered = highPassCoefficient * (previousOutput + value - previousInput);
                        previousInput = value;
                        previousOutput = filtered;
                        value = filtered;
                    }
                    if (lowPassEnabled)
                    {
                        previousLowPass = (1.0f - lowPassCoefficient) * value + lowPassCoefficient * previousLowPass;
                        value = previousLowPass;
                    }
                    samples[sample] = value;
                }
                layer.highPassInput[(size_t) channel] = previousInput;
                layer.highPassOutput[(size_t) channel] = previousOutput;
                layer.lowPassState[(size_t) channel] = previousLowPass;
            }
        }

        layer.eq.setParameters(layer.config.eqLow, layer.config.eqMid, layer.config.eqHigh,
                               currentSampleRate, layer.config.eqLowFrequency,
                               layer.config.eqMidFrequency, layer.config.eqHighFrequency,
                               layer.config.eqLowQ, layer.config.eqMidQ, layer.config.eqHighQ);
        if (layer.eq.isActive())
        {
            for (int channel = 0; channel < scratch.getNumChannels(); ++channel)
            {
                auto* samples = scratch.getWritePointer(channel);
                for (int sample = 0; sample < output.getNumSamples(); ++sample)
                    samples[sample] = layer.eq.process(samples[sample], channel);
            }
        }

        // The native reverb is a consistent stereo room for every SF2, rather
        // than relying on an arbitrary reverb generator embedded in each file.
        const auto reverbMix = juce::jlimit(0.0f, 100.0f, layer.config.reverb) / 100.0f;
        const std::array<float, 4> reverbValues {
            reverbMix, layer.config.reverbSize, layer.config.reverbDamping, layer.config.reverbWidth
        };
        if (reverbValues != layer.lastReverbParameters)
        {
            juce::Reverb::Parameters parameters;
            parameters.roomSize = juce::jlimit(0.0f, 1.0f, layer.config.reverbSize / 100.0f);
            parameters.damping = juce::jlimit(0.0f, 1.0f, layer.config.reverbDamping / 100.0f);
            parameters.width = juce::jlimit(0.0f, 1.0f, layer.config.reverbWidth / 100.0f);
            setLayerReverbMix(parameters, reverbMix);
            parameters.freezeMode = 0.0f;
            layer.nativeReverb.setParameters(parameters);
            layer.lastReverbParameters = reverbValues;
        }
        if (layer.nativeReverb.shouldProcess())
            layer.nativeReverb.processStereo(scratch.getWritePointer(0), scratch.getWritePointer(1),
                                             scratch.getNumSamples());

        // Soft-knee compressor with attack/release smoothing. The COMP knob is
        // its wet amount; detailed threshold, ratio, timing and makeup gain
        // are configured in the layer's compressor editor.
        const auto compressorMix = juce::jlimit(0.0f, 100.0f, layer.config.compressor) / 100.0f;
        if (compressorMix > 0.001f)
        {
            const auto thresholdDb = juce::jlimit(-60.0f, 0.0f, layer.config.compressorThreshold);
            const auto ratio = juce::jlimit(1.0f, 20.0f, layer.config.compressorRatio);
            const auto attackSeconds = juce::jmax(0.0001f, layer.config.compressorAttack * 0.001f);
            const auto releaseSeconds = juce::jmax(0.001f, layer.config.compressorRelease * 0.001f);
            const auto attackCoefficient = std::exp(-1.0f / (attackSeconds * (float) currentSampleRate));
            const auto releaseCoefficient = std::exp(-1.0f / (releaseSeconds * (float) currentSampleRate));
            const auto makeup = juce::Decibels::decibelsToGain(
                juce::jlimit(0.0f, 24.0f, layer.config.compressorMakeup));
            for (int channel = 0; channel < scratch.getNumChannels(); ++channel)
            {
                auto envelope = layer.compressorEnvelope[(size_t) channel];
                auto* samples = scratch.getWritePointer(channel);
                for (int sample = 0; sample < output.getNumSamples(); ++sample)
                {
                    const auto dry = samples[sample];
                    const auto magnitude = juce::jmax(1.0e-8f, std::abs(dry));
                    const auto coefficient = magnitude > envelope ? attackCoefficient : releaseCoefficient;
                    envelope = coefficient * envelope + (1.0f - coefficient) * magnitude;
                    const auto levelDb = juce::Decibels::gainToDecibels(envelope, -120.0f);
                    const auto overDb = juce::jmax(0.0f, levelDb - thresholdDb);
                    const auto reductionDb = overDb * (1.0f - 1.0f / ratio);
                    const auto wet = dry * juce::Decibels::decibelsToGain(-reductionDb) * makeup;
                    samples[sample] = dry + (wet - dry) * compressorMix;
                }
                layer.compressorEnvelope[(size_t) channel] = envelope;
            }
        }

        layer.gain.setTargetValue(juce::jmax(0.0f, layer.config.gain));
        const auto leftPan = layer.config.pan <= 0.0f ? 1.0f : 1.0f - layer.config.pan;
        const auto rightPan = layer.config.pan >= 0.0f ? 1.0f : 1.0f + layer.config.pan;
        float renderedPeak = 0.0f;
        float effectTailPeak = 0.0f;
        for (int sample = 0; sample < output.getNumSamples(); ++sample)
        {
            effectTailPeak = juce::jmax(effectTailPeak,
                std::abs(scratch.getSample(0, sample)),
                std::abs(scratch.getSample(1, sample)));
            const auto gain = layer.gain.getNextValue();
            const auto left = scratch.getSample(0, sample) * gain * leftPan;
            renderedPeak = juce::jmax(renderedPeak, std::abs(left));
            output.addSample(0, sample, left);
            if (output.getNumChannels() > 1)
            {
                const auto right = scratch.getSample(1, sample) * gain * rightPan;
                renderedPeak = juce::jmax(renderedPeak, std::abs(right));
                output.addSample(1, sample, right);
            }
        }
        const auto previousPeak = layer.peak.load(std::memory_order_relaxed) * 0.82f;
        layer.peak.store(juce::jmax(renderedPeak, previousPeak), std::memory_order_relaxed);
        updateLayerEffectTail(layer.effectTailPending, layer.effectQuietSamples,
                              fluid_synth_get_active_voice_count(layer.synth.get()) > 0,
                              effectTailPeak, output.getNumSamples(), currentSampleRate);
    }
}

Sf2Engine::LayerConfig Sf2Engine::getConfig(int index) const
{
    const juce::ScopedLock guard(lock);
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].storedConfig : LayerConfig{};
}

void Sf2Engine::setConfig(int index, const LayerConfig& config)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];

    // Muting/soloing or replacing a performance must never leave a sustained
    // pad running in a disabled layer. Explicitly cancel sustain and voices.
    if (layer.config.enabled && !config.enabled && layer.synth != nullptr)
    {
        for (int channel = 0; channel < 16; ++channel)
        {
            fluid_synth_cc(layer.synth.get(), channel, 64, 0);
            fluid_synth_all_notes_off(layer.synth.get(), channel);
            fluid_synth_all_sounds_off(layer.synth.get(), channel);
        }
        layer.monoNote = -1;
    }
    if (layer.config.modulationEnabled && !config.modulationEnabled && layer.synth != nullptr)
        for (int channel = 0; channel < 16; ++channel)
            fluid_synth_cc(layer.synth.get(), channel, 1, 0);
    if (!config.modulationEnabled)
        layer.modulationAmount = 0.0f;
    const auto newPolyphony = juce::jlimit(64, maxPolyphonyPerLayer, config.polyphony);
    if (layer.synth != nullptr && layer.config.polyphony != newPolyphony)
        fluid_synth_set_polyphony(layer.synth.get(), newPolyphony);
    layer.config = config;
    layer.config.polyphony = newPolyphony;
    layer.storedConfig = layer.config;
}

void Sf2Engine::setRuntimeConfig(int index, const LayerConfig& config)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];

    if (layer.config.enabled && !config.enabled && layer.synth != nullptr)
    {
        for (int channel = 0; channel < 16; ++channel)
        {
            fluid_synth_cc(layer.synth.get(), channel, 64, 0);
            fluid_synth_all_notes_off(layer.synth.get(), channel);
            fluid_synth_all_sounds_off(layer.synth.get(), channel);
        }
        layer.monoNote = -1;
    }
    if (layer.config.modulationEnabled && !config.modulationEnabled && layer.synth != nullptr)
        for (int channel = 0; channel < 16; ++channel)
            fluid_synth_cc(layer.synth.get(), channel, 1, 0);
    if (!config.modulationEnabled)
        layer.modulationAmount = 0.0f;
    const auto newPolyphony = juce::jlimit(64, maxPolyphonyPerLayer, config.polyphony);
    if (layer.synth != nullptr && layer.config.polyphony != newPolyphony)
        fluid_synth_set_polyphony(layer.synth.get(), newPolyphony);
    layer.config = config;
    layer.config.polyphony = newPolyphony;
}

juce::String Sf2Engine::getSoundFontPath(int index) const
{
    const juce::ScopedLock guard(lock);
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].soundFontPath : juce::String{};
}

std::vector<Sf2Engine::Preset> Sf2Engine::getPresets(int index) const
{
    const juce::ScopedLock guard(lock);
    std::vector<Preset> result;
    if (!juce::isPositiveAndBelow(index, layerCount)) return result;
    const auto& layer = layers[(size_t) index];
    if (!layer.synth || layer.soundFontId < 0) return result;
    auto* soundFont = fluid_synth_get_sfont_by_id(layer.synth.get(), layer.soundFontId);
    if (soundFont == nullptr) return result;
    fluid_sfont_iteration_start(soundFont);
    while (auto* preset = fluid_sfont_iteration_next(soundFont))
        result.push_back({ juce::String::fromUTF8(fluid_preset_get_name(preset)),
                           fluid_preset_get_banknum(preset), fluid_preset_get_num(preset) });
    return result;
}

int Sf2Engine::getSelectedBank(int index) const
{
    const juce::ScopedLock guard(lock);
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].selectedBank : 0;
}

int Sf2Engine::getSelectedProgram(int index) const
{
    const juce::ScopedLock guard(lock);
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].selectedProgram : 0;
}

void Sf2Engine::selectPreset(int index, int bank, int program)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    if (!layer.synth || layer.soundFontId < 0) return;
    for (int channel = 0; channel < 16; ++channel)
    {
        fluid_synth_bank_select(layer.synth.get(), channel, juce::jlimit(0, 16383, bank));
        fluid_synth_program_change(layer.synth.get(), channel, juce::jlimit(0, 127, program));
    }
    layer.selectedBank = juce::jlimit(0, 16383, bank);
    layer.selectedProgram = juce::jlimit(0, 127, program);
}

void Sf2Engine::sendController(int index, int controller, int value)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    if (!layer.synth || layer.soundFontId < 0) return;
    if (controller == 1)
        layer.modulationAmount = layer.config.modulationEnabled
            ? juce::jlimit(0, 127, value) / 127.0f : 0.0f;
    for (int channel = 0; channel < 16; ++channel)
        fluid_synth_cc(layer.synth.get(), channel, juce::jlimit(0, 127, controller),
                       juce::jlimit(0, 127, value));
}

float Sf2Engine::getLayerPeak(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount)
             ? layers[(size_t) index].peak.load(std::memory_order_relaxed) : 0.0f;
}
