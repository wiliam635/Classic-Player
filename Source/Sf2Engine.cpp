#include "Sf2Engine.h"
#include <cmath>

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
        createSynth(layer);
}

Sf2Engine::~Sf2Engine() = default;

void Sf2Engine::createSynth(Layer& layer)
{
    layer.synth.reset();
    layer.settings.reset(new_fluid_settings());
    fluid_settings_setnum(layer.settings.get(), "synth.sample-rate", currentSampleRate);
    fluid_settings_setint(layer.settings.get(), "synth.threadsafe-api", 1);
    // A finite ceiling protects the real-time thread from runaway voice counts.
    // FluidSynth performs voice stealing when this ceiling is reached.
    fluid_settings_setint(layer.settings.get(), "synth.polyphony", 512);
    fluid_settings_setnum(layer.settings.get(), "synth.gain", 0.45);
    layer.synth.reset(new_fluid_synth(layer.settings.get()));
    layer.soundFontId = -1;
    layer.monoNote = -1;
    layer.modulationAmount = 0.0f;
    layer.modulationPhase = 0.0;
    layer.lastCutoff = -1;
    layer.lastReverb = -1;
    layer.lastRelease = -1;
    layer.filterState = { 0.0f, 0.0f };
}

void Sf2Engine::prepare(double sampleRate, int maximumBlockSize)
{
    const juce::ScopedLock guard(lock);
    currentSampleRate = sampleRate;
    currentBlockSize = maximumBlockSize;
    scratch.setSize(2, maximumBlockSize, false, false, true);

    for (auto& layer : layers)
    {
#if JUCE_WINDOWS
        // The internal immediate-rate helper is not exported by the Windows
        // FluidSynth library. Rebuild the synth at the requested device rate
        // and restore its SoundFont/preset instead of linking a private symbol.
        const auto soundFontPath = layer.soundFontPath;
        const auto selectedBank = layer.selectedBank;
        const auto selectedProgram = layer.selectedProgram;
        createSynth(layer);

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
            layer.filterState = { 0.0f, 0.0f };
        }
}

juce::Result Sf2Engine::loadSoundFont(int index, const juce::File& file)
{
    if (!juce::isPositiveAndBelow(index, layerCount))
        return juce::Result::fail("Layer inválida");
    if (!file.existsAsFile())
        return juce::Result::fail("Arquivo SF2 não encontrado");

    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    if (!layer.synth)
        createSynth(layer);

    if (layer.soundFontId >= 0)
        fluid_synth_sfunload(layer.synth.get(), layer.soundFontId, 1);

    const auto id = fluid_synth_sfload(layer.synth.get(), file.getFullPathName().toRawUTF8(), 1);
    if (id < 0)
    {
        layer.soundFontId = -1;
        layer.soundFontPath.clear();
        return juce::Result::fail("FluidSynth não conseguiu abrir este SF2");
    }

    layer.soundFontId = id;
    layer.soundFontPath = file.getFullPathName();
    layer.selectedBank = 0;
    layer.selectedProgram = 0;
    layer.filterState = { 0.0f, 0.0f };
    layer.modulationAmount = 0.0f;
    layer.modulationPhase = 0.0;
    return juce::Result::ok();
}

void Sf2Engine::unloadSoundFont(int index)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    auto& layer = layers[(size_t) index];
    if (layer.synth && layer.soundFontId >= 0)
        fluid_synth_sfunload(layer.synth.get(), layer.soundFontId, 1);
    layer.soundFontId = -1;
    layer.soundFontPath.clear();
    layer.selectedBank = 0;
    layer.selectedProgram = 0;
    layer.modulationAmount = 0.0f;
    layer.modulationPhase = 0.0;
}

void Sf2Engine::dispatchMidi(Layer& layer, const juce::MidiMessage& message)
{
    const auto channel = message.getChannel();
    if (layer.config.midiChannel != 0 && channel != layer.config.midiChannel)
        return;

    if (message.isNoteOnOrOff())
    {
        const auto sourceNote = message.getNoteNumber();
        if (sourceNote < layer.config.lowNote || sourceNote > layer.config.highNote)
            return;
        const auto note = juce::jlimit(0, 127, sourceNote + layer.config.octave * 12);
        if (message.isNoteOn())
        {
            auto velocity = juce::jlimit(0.0f, 1.0f, message.getFloatVelocity());
            if (layer.config.velocityCurve == 1) velocity = std::sqrt(velocity);
            else if (layer.config.velocityCurve == 2) velocity = velocity * velocity;
            fluid_synth_noteon(layer.synth.get(), channel - 1, note,
                               juce::jlimit(1, 127, (int) std::round(velocity * 127.0f)));
            // In mono mode, start the new note before releasing the previous
            // one. This preserves a continuous legato transition and avoids
            // the first note being cut by an early note-off in some hosts.
            if (layer.config.mono && layer.monoNote >= 0 && layer.monoNote != note)
                fluid_synth_noteoff(layer.synth.get(), channel - 1, layer.monoNote);
            if (layer.config.mono) layer.monoNote = note;
        }
        else
        {
            fluid_synth_noteoff(layer.synth.get(), channel - 1, note);
            if (layer.monoNote == note) layer.monoNote = -1;
        }
        return;
    }

    if (message.isController())
    {
        if (message.getControllerNumber() == 64 && !layer.config.sustainEnabled)
            return;
        if (message.getControllerNumber() == 1)
            layer.modulationAmount = message.getControllerValue() / 127.0f;
        fluid_synth_cc(layer.synth.get(), channel - 1,
                       message.getControllerNumber(), message.getControllerValue());
    }
    else if (message.isPitchWheel())
        fluid_synth_pitch_bend(layer.synth.get(), channel - 1, message.getPitchWheelValue());
    else if (message.isProgramChange())
        fluid_synth_program_change(layer.synth.get(), channel - 1, message.getProgramChangeNumber());
    else if (message.isAllNotesOff() || message.isAllSoundOff())
        fluid_synth_all_notes_off(layer.synth.get(), channel - 1);
}

void Sf2Engine::process(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                        const std::array<juce::MidiBuffer, layerCount>* routedMidi)
{
    const juce::ScopedLock guard(lock);
    output.clear();
    scratch.setSize(2, output.getNumSamples(), false, false, true);

    for (auto& layer : layers)
    {
        if (!layer.config.enabled || layer.soundFontId < 0 || !layer.synth)
        {
            layer.peak.store(layer.peak.load(std::memory_order_relaxed) * 0.82f,
                             std::memory_order_relaxed);
            continue;
        }

        const auto cutoff = juce::jlimit(0, 127, static_cast<int>(std::round(layer.config.cutoff * 1.27f)));
        const auto reverb = juce::jlimit(0, 127, static_cast<int>(std::round(layer.config.reverb * 1.27f)));
        if (cutoff != layer.lastCutoff || reverb != layer.lastReverb)
        {
            for (int channel = 0; channel < 16; ++channel)
            {
                if (cutoff != layer.lastCutoff) fluid_synth_cc(layer.synth.get(), channel, 74, cutoff);
                if (reverb != layer.lastReverb) fluid_synth_cc(layer.synth.get(), channel, 91, reverb);
            }
            layer.lastCutoff = cutoff;
            layer.lastReverb = reverb;
        }

        for (const auto metadata : midi)
            dispatchMidi(layer, metadata.getMessage());
        if (routedMidi != nullptr)
            for (const auto metadata : (*routedMidi)[static_cast<size_t>(&layer - layers.data())])
                dispatchMidi(layer, metadata.getMessage());

        scratch.clear();
        fluid_synth_write_float(layer.synth.get(), output.getNumSamples(),
                                scratch.getWritePointer(0), 0, 1,
                                scratch.getWritePointer(1), 0, 1);

        // CC1 uses category-specific effects without adding knobs or changing
        // the approved front-end: tremolo for electric pianos and fast rotary
        // movement for organs. CC1 at zero keeps the layer dry.
        const auto category = layer.soundFontPath.getParentDirectory().getFileName();
        const auto mod = juce::jlimit(0.0f, 1.0f, layer.modulationAmount);
        if (mod > 0.001f && (category == "Piano Eletrico" || category == "Organ"))
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

        const auto compressor = juce::jlimit(0.0f, 100.0f, layer.config.compressor) / 100.0f;
        if (compressor > 0.001f)
        {
            const auto threshold = juce::Decibels::decibelsToGain(-24.0f * compressor);
            const auto ratio = 1.0f + 7.0f * compressor;
            const auto makeup = 1.0f + 0.35f * compressor;
            for (int channel = 0; channel < scratch.getNumChannels(); ++channel)
            {
                auto* samples = scratch.getWritePointer(channel);
                for (int sample = 0; sample < output.getNumSamples(); ++sample)
                {
                    const auto input = samples[sample];
                    const auto magnitude = std::abs(input);
                    const auto compressed = magnitude > threshold
                        ? threshold + (magnitude - threshold) / ratio : magnitude;
                    samples[sample] = std::copysign(compressed * makeup, input);
                }
            }
        }

        const auto renderedPeak = juce::jmax(scratch.getMagnitude(0, 0, output.getNumSamples()),
                                             scratch.getMagnitude(1, 0, output.getNumSamples()));
        const auto previousPeak = layer.peak.load(std::memory_order_relaxed) * 0.82f;
        layer.peak.store(juce::jmax(renderedPeak, previousPeak), std::memory_order_relaxed);

        const auto leftGain = layer.config.gain * (layer.config.pan <= 0.0f ? 1.0f : 1.0f - layer.config.pan);
        const auto rightGain = layer.config.gain * (layer.config.pan >= 0.0f ? 1.0f : 1.0f + layer.config.pan);
        output.addFrom(0, 0, scratch, 0, 0, output.getNumSamples(), leftGain);
        if (output.getNumChannels() > 1)
            output.addFrom(1, 0, scratch, 1, 0, output.getNumSamples(), rightGain);
    }
}

Sf2Engine::LayerConfig Sf2Engine::getConfig(int index) const
{
    const juce::ScopedLock guard(lock);
    return juce::isPositiveAndBelow(index, layerCount) ? layers[(size_t) index].config : LayerConfig{};
}

void Sf2Engine::setConfig(int index, const LayerConfig& config)
{
    if (!juce::isPositiveAndBelow(index, layerCount)) return;
    const juce::ScopedLock guard(lock);
    layers[(size_t) index].config = config;
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
        layer.modulationAmount = juce::jlimit(0, 127, value) / 127.0f;
    for (int channel = 0; channel < 16; ++channel)
        fluid_synth_cc(layer.synth.get(), channel, juce::jlimit(0, 127, controller),
                       juce::jlimit(0, 127, value));
}

float Sf2Engine::getLayerPeak(int index) const
{
    return juce::isPositiveAndBelow(index, layerCount)
             ? layers[(size_t) index].peak.load(std::memory_order_relaxed) : 0.0f;
}
