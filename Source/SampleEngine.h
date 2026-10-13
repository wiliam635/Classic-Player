#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "Sf2Engine.h"
#include <array>
#include <atomic>

// A small, self-contained sample instrument used by the clean-room Samplit
// compatibility layer.  It intentionally keeps the audio callback free from
// file I/O: files are decoded before they become visible to the renderer.
class SampleEngine
{
public:
    static constexpr int layerCount = Sf2Engine::layerCount;

    struct LayerConfig
    {
        int rootNote = 60;
        bool loop = false;
        double startSeconds = 0.0;
        double endSeconds = 0.0; // zero means the end of the decoded sample
    };

    SampleEngine();

    void prepare(double sampleRate, int maximumBlockSize);
    juce::Result loadSample(int layer, const juce::File& file);
    void unload(int layer);
    void stopAllSounds();
    bool isLoaded(int layer) const;
    juce::String path(int layer) const;
    float getLayerPeak(int layer) const;
    LayerConfig getConfig(int layer) const;
    void setConfig(int layer, const LayerConfig& config);

    void process(juce::AudioBuffer<float>& output,
                 const std::array<juce::MidiBuffer, layerCount>& routedMidi,
                 const std::array<Sf2Engine::LayerConfig, layerCount>& routing);

private:
    struct Voice
    {
        bool active = false;
        bool released = false;
        int note = -1;
        int channel = 1;
        int age = 0;
        double position = 0.0;
        double increment = 1.0;
        float level = 0.0f;
        float releaseGain = 1.0f;
    };

    struct Layer
    {
        juce::AudioBuffer<float> audio;
        juce::String sourcePath;
        double sourceSampleRate = 44100.0;
        LayerConfig config;
        std::array<Voice, 32> voices;
        std::atomic<float> peak { 0.0f };
    };

    void startVoice(Layer&, int note, int channel, float velocity,
                    const Sf2Engine::LayerConfig& routing);
    void releaseVoice(Layer&, int note, int channel);
    void renderLayer(Layer&, juce::AudioBuffer<float>&,
                     const juce::MidiBuffer&, const Sf2Engine::LayerConfig&);
    static float readInterpolated(const juce::AudioBuffer<float>&, int channel, double position);

    std::array<Layer, layerCount> layers;
    juce::AudioFormatManager formats;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    mutable juce::CriticalSection lock;
};
