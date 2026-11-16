#pragma once
#include "PluginProcessor.h"

// Header card for the Nord piano / sample layers: section title, power LED
// and an LCD showing what the layer currently has loaded. Everything shown
// comes from the layer itself (file name, library folder, file type).
namespace nordui
{
constexpr juce::uint32 redTop = 0xffb3202c, redBottom = 0xff7a121b, slate = 0xff2a3a52,
                       slateLight = 0xff3b4d68, lcd = 0xff0c1013, lcdText = 0xffe6f2ff, lcdDim = 0xff7f93a8;
}

class NordFaceplate final : public juce::Component, private juce::Timer
{
public:
    NordFaceplate(ClassicPlayerAudioProcessor& p, int layer, bool sampleLayer)
        : processor(p), index(layer), isSample(sampleLayer)
    {
        setSize(620, 104);
        refresh();
        startTimerHz(4);
    }
    ~NordFaceplate() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        using namespace nordui;
        const auto b = getLocalBounds().toFloat().reduced(1.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(redTop), 0, b.getY(), juce::Colour(redBottom), 0, b.getBottom(), false));
        g.fillRoundedRectangle(b, 7.0f);
        g.setColour(juce::Colour(0xff3a070c)); g.drawRoundedRectangle(b, 7.0f, 1.5f);

        // Left: section title, power LED, layer tag.
        auto left = b.reduced(14.0f, 10.0f).removeFromLeft(b.getWidth() * 0.34f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(17.0f, juce::Font::bold));
        g.drawText(isSample ? "SAMPLE" : "PIANO", left.removeFromTop(24.0f), juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffffd0d3));
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText("SECTION", left.removeFromTop(14.0f), juce::Justification::centredLeft);
        left.removeFromTop(8.0f);
        const auto led = juce::Rectangle<float>(10.0f, 10.0f).withPosition(left.getX(), left.getY() + 4.0f);
        g.setColour(loaded ? juce::Colour(0xff58ff7a) : juce::Colour(0xff4a1216));
        g.fillEllipse(led);
        g.setColour(juce::Colour(0xff1d0305)); g.drawEllipse(led, 1.0f);
        g.setColour(juce::Colour(0xffffd0d3));
        g.drawText(loaded ? "LOADED" : "EMPTY", juce::Rectangle<float>(led.getRight() + 6.0f, left.getY(), 90.0f, 18.0f), juce::Justification::centredLeft);
        g.drawText("LAYER " + juce::String(index + 1), left.withTrimmedTop(20.0f).removeFromTop(16.0f), juce::Justification::centredLeft);

        // Right: LCD.
        auto lcdArea = b.reduced(10.0f).withTrimmedLeft(b.getWidth() * 0.36f);
        g.setColour(juce::Colour(slate)); g.fillRoundedRectangle(lcdArea.expanded(4.0f), 5.0f);
        g.setColour(juce::Colour(lcd)); g.fillRoundedRectangle(lcdArea, 3.0f);
        g.setColour(juce::Colour(0xff1c2a38)); g.drawRoundedRectangle(lcdArea, 3.0f, 1.0f);
        auto text = lcdArea.reduced(12.0f, 8.0f);
        g.setColour(juce::Colour(lcdDim));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(tag, text.removeFromTop(14.0f), juce::Justification::centredLeft, true);
        g.setColour(juce::Colour(lcdText));
        g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText(name, text.removeFromTop(30.0f), juce::Justification::centredLeft, true);
        g.setColour(juce::Colour(lcdDim));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(detail, text.removeFromTop(16.0f), juce::Justification::centredLeft, true);
    }

private:
    void timerCallback() override { if (isShowing()) refresh(); }
    void refresh()
    {
        const auto path = processor.soundFontPath(index);
        const juce::File file(path);
        const bool nowLoaded = path.isNotEmpty() && file.existsAsFile();
        const auto extension = file.getFileExtension().toUpperCase().trimCharactersAtStart(".");
        const auto newTag = (isSample ? juce::String("SAMPLE") : juce::String("PIANO"))
                            + (extension.isNotEmpty() ? "  -  " + extension : juce::String());
        const auto newName = nowLoaded ? file.getFileNameWithoutExtension()
                                       : juce::String::fromUTF8("Nenhum arquivo carregado");
        const auto newDetail = nowLoaded ? file.getParentDirectory().getFileName()
                                         : juce::String::fromUTF8("Escolha na biblioteca abaixo");
        if (nowLoaded != loaded || newTag != tag || newName != name || newDetail != detail)
        {
            loaded = nowLoaded; tag = newTag; name = newName; detail = newDetail;
            repaint();
        }
    }
    ClassicPlayerAudioProcessor& processor;
    int index;
    bool isSample, loaded = false;
    juce::String tag, name, detail;
};

// Header card for the SF2 layer: same family as the Nord faceplate but in
// blue, showing the loaded SoundFont, the selected preset and bank/program.
class Sf2Faceplate final : public juce::Component, private juce::Timer
{
public:
    Sf2Faceplate(ClassicPlayerAudioProcessor& p, int layer) : processor(p), index(layer)
    {
        setSize(620, 84);
        refresh();
        startTimerHz(4);
    }
    ~Sf2Faceplate() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        using namespace nordui;
        const auto b = getLocalBounds().toFloat().reduced(1.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2b6cb0), 0, b.getY(), juce::Colour(0xff17406e), 0, b.getBottom(), false));
        g.fillRoundedRectangle(b, 7.0f);
        g.setColour(juce::Colour(0xff0c2340)); g.drawRoundedRectangle(b, 7.0f, 1.5f);

        auto left = b.reduced(14.0f, 8.0f).removeFromLeft(b.getWidth() * 0.30f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText("SF2", left.removeFromTop(26.0f), juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffcfe3ff));
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText("SOUNDFONT", left.removeFromTop(14.0f), juce::Justification::centredLeft);
        left.removeFromTop(6.0f);
        const auto led = juce::Rectangle<float>(10.0f, 10.0f).withPosition(left.getX(), left.getY() + 2.0f);
        g.setColour(loaded ? juce::Colour(0xff58ff7a) : juce::Colour(0xff10253f));
        g.fillEllipse(led);
        g.setColour(juce::Colour(0xff06101c)); g.drawEllipse(led, 1.0f);
        g.setColour(juce::Colour(0xffcfe3ff));
        g.drawText(loaded ? "LOADED  -  LAYER " + juce::String(index + 1) : "EMPTY  -  LAYER " + juce::String(index + 1),
                   juce::Rectangle<float>(led.getRight() + 6.0f, left.getY(), 170.0f, 14.0f), juce::Justification::centredLeft);

        auto lcdArea = b.reduced(10.0f).withTrimmedLeft(b.getWidth() * 0.32f);
        g.setColour(juce::Colour(slate)); g.fillRoundedRectangle(lcdArea.expanded(4.0f), 5.0f);
        g.setColour(juce::Colour(lcd)); g.fillRoundedRectangle(lcdArea, 3.0f);
        g.setColour(juce::Colour(0xff1c2a38)); g.drawRoundedRectangle(lcdArea, 3.0f, 1.0f);
        auto text = lcdArea.reduced(12.0f, 4.0f);
        g.setColour(juce::Colour(lcdDim));
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(tag, text.removeFromTop(13.0f), juce::Justification::centredLeft, true);
        g.setColour(juce::Colour(lcdText));
        g.setFont(juce::FontOptions(19.0f, juce::Font::bold));
        g.drawText(name, text.removeFromTop(26.0f), juce::Justification::centredLeft, true);
        g.setColour(juce::Colour(lcdDim));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(detail, text.removeFromTop(14.0f), juce::Justification::centredLeft, true);
    }

private:
    void timerCallback() override { if (isShowing()) refresh(); }
    void refresh()
    {
        const auto path = processor.soundFontPath(index);
        const int bank = processor.layerPresetBank(index);
        const int program = processor.layerPresetProgram(index);
        if (path == lastPath && bank == lastBank && program == lastProgram)
            return;
        lastPath = path; lastBank = bank; lastProgram = program;

        const juce::File file(path);
        loaded = path.isNotEmpty() && file.existsAsFile();
        juce::String presetName;
        int presetCount = 0;
        if (loaded)
        {
            const auto presets = processor.layerPresets(index);
            presetCount = (int) presets.size();
            for (const auto& preset : presets)
                if (preset.bank == bank && preset.program == program) { presetName = preset.name; break; }
        }
        tag = loaded ? juce::String("PRESET  ") + juce::String(bank).paddedLeft('0', 3) + ":" + juce::String(program).paddedLeft('0', 3)
                     : juce::String("SOUNDFONT");
        name = loaded ? (presetName.isNotEmpty() ? presetName : file.getFileNameWithoutExtension())
                      : juce::String::fromUTF8("Nenhum SoundFont carregado");
        detail = loaded ? file.getFileNameWithoutExtension() + "  -  " + file.getParentDirectory().getFileName()
                              + "  -  " + juce::String(presetCount) + " presets"
                        : juce::String::fromUTF8("Escolha na biblioteca abaixo");
        repaint();
    }
    ClassicPlayerAudioProcessor& processor;
    int index;
    bool loaded = false;
    int lastBank = -2, lastProgram = -2;
    juce::String lastPath, tag, name, detail;
};
