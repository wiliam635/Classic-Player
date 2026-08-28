#include "Dx7Engine.h"
#include "AnalogSynthEngine.h"
#include "AnalogBrowserPresets.h"
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static juce::MidiBuffer events(int start, int count, bool transitions)
{
    juce::MidiBuffer result;
    const auto add = [&](int at, juce::MidiMessage message)
    {
        if (at >= start && at < start + count) result.addEvent(message, at - start);
    };
    add(73, juce::MidiMessage::noteOn(1, 60, 0.8f));
    if (transitions)
    {
        add(399, juce::MidiMessage::noteOn(1, 67, 0.3f));
        add(613, juce::MidiMessage::noteOff(1, 67));
        add(871, juce::MidiMessage::noteOff(1, 60));
        add(910, juce::MidiMessage::noteOn(1, 64, 0.9f));
        add(1200, juce::MidiMessage::noteOff(1, 64));
    }
    return result;
}

static std::vector<float> dx7(const juce::File& fixture, int block, bool mono, bool transitions)
{
    auto engine = std::make_unique<Dx7Engine>();
    engine->prepare(48000, 512);
    require(engine->loadSysEx(0, fixture).wasOk(), "DX7 fixture load");
    std::array<Sf2Engine::LayerConfig, Dx7Engine::layerCount> configs;
    for (auto& config : configs) config.enabled = false;
    configs[0].enabled = true;
    configs[0].mono = mono;
    configs[0].dx7Chorus = 0;
    configs[0].cutoff = 100;
    configs[0].gain = 0.8f;
    std::vector<float> result;
    for (int start = 0; start < 4096; start += block)
    {
        const auto count = std::min(block, 4096 - start);
        juce::AudioBuffer<float> audio(2, count);
        audio.clear();
        auto midi = events(start, count, transitions);
        engine->process(audio, midi, nullptr, configs);
        result.insert(result.end(), audio.getReadPointer(0), audio.getReadPointer(0) + count);
    }
    return result;
}

static std::vector<float> analog(int block, bool mono, bool transitions)
{
    auto engine = std::make_unique<AnalogSynthEngine>();
    engine->prepare(48000, 512);
    std::array<AnalogSynthEngine::Config, AnalogSynthEngine::layerCount> configs;
    for (auto& config : configs) config.routing.enabled = false;
    configs[0].routing.enabled = true;
    configs[0].monophonic = mono;
    configs[0].routing.gain = 0.8f;
    std::vector<float> result;
    for (int start = 0; start < 4096; start += block)
    {
        const auto count = std::min(block, 4096 - start);
        juce::AudioBuffer<float> audio(2, count);
        audio.clear();
        auto midi = events(start, count, transitions);
        engine->process(audio, midi, configs);
        result.insert(result.end(), audio.getReadPointer(0), audio.getReadPointer(0) + count);
    }
    return result;
}

static void independentAnalogTuning()
{
    // A muted oscillator's tuning must not transpose the other oscillators,
    // including when mono note-off returns to an earlier held note.
    for (bool mono : { false, true })
    {
        auto reference = std::make_unique<AnalogSynthEngine>();
        auto changed = std::make_unique<AnalogSynthEngine>();
        reference->prepare(48000, 512);
        changed->prepare(48000, 512);
        std::array<AnalogSynthEngine::Config, AnalogSynthEngine::layerCount> a;
        for (auto& config : a) config.routing.enabled = false;
        a[0].routing.enabled = true;
        a[0].monophonic = mono;
        a[0].oscillator1Enabled = false;
        auto b = a;
        b[0].oscillator1Semitones = -12;
        b[0].oscillator1FineCents = 23;
        float peak = 0;
        for (int start = 0; start < 4096; start += 512)
        {
            juce::AudioBuffer<float> left(2, 512), right(2, 512);
            left.clear(); right.clear();
            auto midi = events(start, 512, true);
            reference->process(left, midi, a);
            changed->process(right, midi, b);
            for (int i = 0; i < 512; ++i)
            {
                peak = std::max(peak, std::abs(left.getSample(0, i)));
                require(std::abs(left.getSample(0, i) - right.getSample(0, i)) < 1e-7f,
                        "OSC 1 tuning transposed another oscillator");
            }
        }
        require(peak > 0.0001f, "oscillator tuning test silent");
    }
}

static void compare(const std::vector<float>& a, const std::vector<float>& b)
{
    float error = 0, peak = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        require(std::isfinite(a[i]) && std::isfinite(b[i]), "non-finite audio");
        error = std::max(error, std::abs(a[i] - b[i]));
        peak = std::max(peak, std::abs(a[i]));
        if (i < 73) require(a[i] == 0 && b[i] == 0, "MIDI note rendered before its timestamp");
    }
    require(peak > 0.0001f, "silent engine");
    std::cout << "partition error: " << error << ", peak: " << peak << '\n';
    require(error < 0.00001f, "audio depends on host block size");
}

static void volumeRamp(const juce::File& fixture)
{
    auto reference = std::make_unique<Dx7Engine>();
    auto changed = std::make_unique<Dx7Engine>();
    reference->prepare(48000, 512); changed->prepare(48000, 512);
    require(reference->loadSysEx(0, fixture).wasOk(), "reference fixture");
    require(changed->loadSysEx(0, fixture).wasOk(), "changed fixture");
    std::array<Sf2Engine::LayerConfig, Dx7Engine::layerCount> configs;
    for (auto& config : configs) config.enabled = false;
    configs[0].enabled = true; configs[0].gain = 0.8f; configs[0].dx7Chorus = 0;
    juce::AudioBuffer<float> a(2, 512), b(2, 512);
    for (int block = 0; block < 8; ++block)
    {
        a.clear(); b.clear();
        juce::MidiBuffer midi;
        if (block == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        reference->process(a, midi, nullptr, configs);
        changed->process(b, midi, nullptr, configs);
    }
    a.clear(); b.clear();
    juce::MidiBuffer empty;
    reference->process(a, empty, nullptr, configs);
    configs[0].gain = 0.0f;
    changed->process(b, empty, nullptr, configs);
    const auto initialDifference = std::abs(a.getSample(0, 0) - b.getSample(0, 0));
    std::cout << "volume step initial difference: " << initialDifference << '\n';
    require(initialDifference < 0.001f, "volume changed discontinuously");
    for (int block = 0; block < 16; ++block) { b.clear(); changed->process(b, empty, nullptr, configs); }
    require(b.getMagnitude(0, 512) < 0.0001f, "volume did not settle at silence");
}

static std::vector<float> browserAudio(AnalogSynthEngine::Config c, int block, double rate,
                                       float velocity, bool transitions = true)
{
    auto engine = std::make_unique<AnalogSynthEngine>();
    engine->prepare(rate, 512);
    std::array<AnalogSynthEngine::Config, AnalogSynthEngine::layerCount> configs;
    for (auto& item : configs) item.routing.enabled = false;
    c.routing.enabled = true; c.routing.gain = 1; c.routing.reverb = 0; c.routing.compressor = 0;
    configs[0] = c;
    std::vector<float> out;
    for (int start = 0; start < 24000; start += block)
    {
        const int count = std::min(block, 24000 - start);
        juce::AudioBuffer<float> audio(2, count); audio.clear();
        juce::MidiBuffer midi;
        const auto add = [&](int at, const juce::MidiMessage& message) {
            if (at >= start && at < start + count) midi.addEvent(message, at - start);
        };
        add(0, juce::MidiMessage::noteOn(1, 60, velocity));
        if (transitions)
        {
            add(5101, juce::MidiMessage::noteOn(1, 61, velocity));
            add(8001, juce::MidiMessage::noteOff(1, 61));
            add(11113, juce::MidiMessage::noteOff(1, 60));
            add(15551, juce::MidiMessage::noteOn(1, 67, velocity));
            add(19003, juce::MidiMessage::noteOff(1, 67));
        }
        engine->process(audio, midi, configs);
        out.insert(out.end(), audio.getReadPointer(0), audio.getReadPointer(0) + count);
    }
    return out;
}

static void browserRegression()
{
    require(AnalogBrowserPresets::bank.size() == 32, "browser preset count");
    for (double rate : {44100.0, 48000.0})
        for (size_t p = 0; p < AnalogBrowserPresets::bank.size(); ++p)
        {
            const auto config = AnalogBrowserPresets::config(p, Sf2Engine::LayerConfig{});
            const auto a = browserAudio(config, 127, rate, 0.15f);
            const auto b = browserAudio(config, 512, rate, 0.95f);
            float peak = 0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                require(std::isfinite(a[i]) && std::abs(a[i]) < 2, "browser non-finite/excessive output");
                require(std::abs(a[i] - b[i]) < 1e-6f, "browser velocity or block dependence");
                peak = std::max(peak, std::abs(a[i]));
            }
            require(peak > 0.00001f, "silent browser preset");
        }
    // Independent sine/biquad/VCA reference, including a-rate filter modulation.
    auto c = AnalogBrowserPresets::config(0, Sf2Engine::LayerConfig{});
    c.oscillator1Wave = AnalogSynthEngine::Waveform::sine;
    c.oscillator1Level = 1; c.oscillator2Level = c.oscillator3Level = 0;
    const auto actual = browserAudio(c, 127, 48000, 0.8f, false);
    double x1=0,x2=0,y1=0,y2=0,maxError=0;
    for (size_t n=0; n<actual.size(); ++n)
    {
        const double t=static_cast<double>(n)/48000;
        const double x=std::sin(juce::MathConstants<double>::twoPi * 261.6255653005986 * t);
        const double cut=2350 + std::sin(juce::MathConstants<double>::twoPi*4*t)*105;
        const double omega=juce::MathConstants<double>::twoPi*cut/48000;
        const double alpha=std::sin(omega)/(2*std::pow(10.0,0.2/20));
        const double y=((1-std::cos(omega))*(x+2*x1+x2)/2
            +2*std::cos(omega)*y1-(1-alpha)*y2)/(1+alpha);
        x2=x1;x1=x;y2=y1;y1=y;
        const double env=t<0.034 ? 0.0001*std::pow(3400,t/0.034)
            : 0.34*std::pow(0.76,(t-0.034)/3.9);
        maxError=std::max(maxError,std::abs(actual[n]-y*env*0.45));
    }
    std::cout<<"Browser sine/filter/envelope reference max error: "<<maxError<<'\n';
    require(maxError < 0.001, "browser reference mismatch");
    std::cout<<"32 browser presets, 44.1/48 kHz, velocity, legato/release and block-size checks passed\n";
}

int main()
{
    try
    {
        juce::TemporaryFile fixture(".syx");
        std::array<uint8_t, 163> bytes {};
        bytes[0] = 0xf0; bytes[1] = 0x43; bytes[4] = 1; bytes[5] = 27;
        auto* patch = bytes.data() + 6;
        for (int op = 0; op < 6; ++op)
        {
            const int offset = op * 21;
            patch[offset] = 99; patch[offset + 1] = 80;
            patch[offset + 2] = 80; patch[offset + 3] = 70;
            patch[offset + 4] = 99; patch[offset + 5] = 80; patch[offset + 6] = 80;
            patch[offset + 16] = 75; patch[offset + 18] = 1; patch[offset + 20] = 7;
        }
        for (int i = 0; i < 4; ++i) { patch[126 + i] = 99; patch[130 + i] = 50; }
        patch[134] = 31; patch[135] = 7; patch[144] = 24;
        for (int i = 145; i < 155; ++i) patch[i] = 'T';
        int sum = 0; for (int i = 0; i < 155; ++i) sum += patch[i];
        bytes[161] = static_cast<uint8_t>((128 - (sum & 127)) & 127); bytes[162] = 0xf7;
        require(fixture.getFile().replaceWithData(bytes.data(), bytes.size()), "fixture write");
        volumeRamp(fixture.getFile());
        independentAnalogTuning();
        browserRegression();
        for (bool mono : {false, true})
            for (bool transitions : {false, true})
            {
                compare(dx7(fixture.getFile(), 512, mono, transitions), dx7(fixture.getFile(), 127, mono, transitions));
                compare(analog(512, mono, transitions), analog(127, mono, transitions));
            }
        AudioTransition transition;
        transition.process(0.8f);
        transition.retarget(48000);
        require(std::abs(transition.process(-0.7f) - 0.8f) < 1e-6f, "voice replacement discontinuity");
        for (int i = 0; i < 145; ++i) transition.process(-0.7f);
        require(std::abs(transition.last + 0.7f) < 1e-6f, "transition did not settle");
        std::cout << "Audio regression tests passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
