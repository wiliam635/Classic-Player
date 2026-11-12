#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

class MidiLearnButton final : public juce::TextButton
{
public:
    explicit MidiLearnButton(const juce::String& name = {}) : juce::TextButton(name) {}
    std::function<void()> onClearMapping;

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!event.mods.isPopupMenu())
        {
            juce::TextButton::mouseDown(event);
            return;
        }
        juce::PopupMenu menu;
        menu.addItem(1, "Excluir mapeamento");
        const juce::Component::SafePointer<MidiLearnButton> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safe](int choice)
                           {
                               if (choice == 1 && safe != nullptr && safe->onClearMapping)
                                   safe->onClearMapping();
                           });
    }
};

// Right-click on a knob, fader, pad or button to assign a MIDI CC to it: shows a small menu,
// starts the existing learn mechanism of that control and floats the chosen CC above it.
class RightClickMidiLearn final : public juce::MouseListener, private juce::Timer
{
public:
    RightClickMidiLearn(juce::Component& targetComponent, MidiLearnButton& learnButton,
                        std::function<juce::String()> mappingText, std::function<bool()> isLearning)
        : target(targetComponent), button(learnButton),
          mapping(std::move(mappingText)), learning(std::move(isLearning))
    {
        target.addMouseListener(this, true);
    }

    ~RightClickMidiLearn() override
    {
        *alive = false;
        target.removeMouseListener(this);
        hideToast();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!event.mods.isPopupMenu()) return;
        const auto current = mapping();
        juce::PopupMenu menu;
        menu.addItem(100, current.isEmpty() ? juce::String("Sem mapeamento MIDI")
                                            : juce::String("Mapeado: ") + current, false);
        menu.addSeparator();
        menu.addItem(1, juce::String::fromUTF8("Atribuir MIDI (mova o controle f\xc3\xadsico)"));
        menu.addItem(2, "Excluir mapeamento", current.isNotEmpty());
        const auto flag = alive;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
                               juce::Rectangle<int>(event.getScreenPosition(), event.getScreenPosition()).expanded(1)),
                           [this, flag](int choice)
                           {
                               if (!*flag) return;
                               if (choice == 1)
                               {
                                   if (!learning() && button.onClick) button.onClick();
                                   showToast(juce::String::fromUTF8("MOVA O CONTROLE MIDI\xe2\x80\xa6"));
                                   waiting = true; ticks = 0;
                                   startTimer(150);
                               }
                               else if (choice == 2 && button.onClearMapping)
                               {
                                   button.onClearMapping();
                                   showToast("MAPEAMENTO EXCLUIDO");
                                   waiting = false; ticks = 0;
                                   startTimer(150);
                               }
                           });
    }

private:
    void timerCallback() override
    {
        ++ticks;
        if (waiting)
        {
            if (learning()) { if (ticks > 200) { waiting = false; hideToast(); stopTimer(); } return; }
            waiting = false; ticks = 0;
            const auto current = mapping();
            showToast(current.isEmpty() ? juce::String("CANCELADO") : "ATRIBUIDO: " + current);
            return;
        }
        if (ticks > 17) { hideToast(); stopTimer(); }
    }

    void showToast(const juce::String& text)
    {
        auto* top = target.getTopLevelComponent();
        if (top == nullptr) return;
        if (toast == nullptr)
        {
            toast = std::make_unique<juce::Label>();
            toast->setJustificationType(juce::Justification::centred);
            toast->setInterceptsMouseClicks(false, false);
            toast->setColour(juce::Label::backgroundColourId, juce::Colour(0xf0f5a524));
            toast->setColour(juce::Label::textColourId, juce::Colour(0xff0f1114));
            toast->setFont(juce::FontOptions(13.0f, juce::Font::bold));
            top->addAndMakeVisible(*toast);
        }
        toast->setText(text, juce::dontSendNotification);
        const int w = juce::jmax(150, text.length() * 9 + 24);
        auto centre = top->getLocalPoint(&target, target.getLocalBounds().getCentre());
        auto top0 = top->getLocalPoint(&target, juce::Point<int>(0, 0));
        int y = juce::jmax(2, top0.y - 26);
        int x = juce::jlimit(2, juce::jmax(2, top->getWidth() - w - 2), centre.x - w / 2);
        toast->setBounds(x, y, w, 24);
        toast->toFront(false);
    }

    void hideToast() { toast.reset(); }

    juce::Component& target;
    MidiLearnButton& button;
    std::function<juce::String()> mapping;
    std::function<bool()> learning;
    std::unique_ptr<juce::Label> toast;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool waiting = false;
    int ticks = 0;
};
