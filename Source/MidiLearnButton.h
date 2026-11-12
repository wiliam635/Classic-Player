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

// Right-click on any slider that carries a "conceptParameter" or "midiParameter" property
// (the APVTS parameter id) to assign a MIDI CC to that parameter.
class ParameterMidiMenu final : public juce::MouseListener, private juce::Timer
{
public:
    struct Hooks
    {
        std::function<juce::String(const juce::String&)> mapping;      // "CC 7 C1" or empty
        std::function<bool(const juce::String&)> isLearning;
        std::function<void(const juce::String&)> begin, clear;
    };

    ParameterMidiMenu(juce::Component& root, Hooks h) : owner(root), hooks(std::move(h))
    {
        owner.addMouseListener(this, true);
    }

    ~ParameterMidiMenu() override
    {
        *alive = false;
        owner.removeMouseListener(this);
        hideToast();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!event.mods.isPopupMenu()) return;
        juce::Component* found = nullptr;
        juce::String id;
        for (auto* c = event.originalComponent; c != nullptr && c != &owner; c = c->getParentComponent())
        {
            id = c->getProperties().getWithDefault("midiParameter",
                     c->getProperties().getWithDefault("conceptParameter", juce::var())).toString();
            if (id.isNotEmpty()) { found = c; break; }
        }
        if (found == nullptr) return;
        target = found;
        paramId = id;
        const auto current = hooks.mapping(id);
        juce::PopupMenu menu;
        menu.addItem(100, current.isEmpty() ? juce::String("Sem mapeamento MIDI")
                                            : juce::String("Mapeado: ") + current, false);
        menu.addSeparator();
        menu.addItem(1, juce::String::fromUTF8("Atribuir MIDI (mova o controle f\xc3\xadsico)"));
        menu.addItem(2, "Excluir mapeamento", current.isNotEmpty());
        const auto flag = alive;
        const juce::Component::SafePointer<juce::Component> safeTarget(found);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
                               juce::Rectangle<int>(event.getScreenPosition(), event.getScreenPosition()).expanded(1)),
                           [this, flag, id, safeTarget](int choice)
                           {
                               if (!*flag || safeTarget == nullptr) return;
                               target = safeTarget; paramId = id;
                               if (choice == 1)
                               {
                                   if (!hooks.isLearning(id)) hooks.begin(id);
                                   showToast(juce::String::fromUTF8("MOVA O CONTROLE MIDI\xe2\x80\xa6"));
                                   waiting = true; ticks = 0; startTimer(150);
                               }
                               else if (choice == 2)
                               {
                                   hooks.clear(id);
                                   showToast("MAPEAMENTO EXCLUIDO");
                                   waiting = false; ticks = 0; startTimer(150);
                               }
                           });
    }

private:
    void timerCallback() override
    {
        ++ticks;
        if (waiting)
        {
            if (hooks.isLearning(paramId)) { if (ticks > 200) { waiting = false; hideToast(); stopTimer(); } return; }
            waiting = false; ticks = 0;
            const auto current = hooks.mapping(paramId);
            showToast(current.isEmpty() ? juce::String("CANCELADO") : "ATRIBUIDO: " + current);
            return;
        }
        if (ticks > 17) { hideToast(); stopTimer(); }
    }

    void showToast(const juce::String& text)
    {
        if (target == nullptr) return;
        auto* top = target->getTopLevelComponent();
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
        auto centre = top->getLocalPoint(target.getComponent(), target->getLocalBounds().getCentre());
        auto origin = top->getLocalPoint(target.getComponent(), juce::Point<int>(0, 0));
        const int y = juce::jmax(2, origin.y - 26);
        const int x = juce::jlimit(2, juce::jmax(2, top->getWidth() - w - 2), centre.x - w / 2);
        toast->setBounds(x, y, w, 24);
        toast->toFront(false);
    }

    void hideToast() { toast.reset(); }

    juce::Component& owner;
    Hooks hooks;
    juce::Component::SafePointer<juce::Component> target;
    juce::String paramId;
    std::unique_ptr<juce::Label> toast;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool waiting = false;
    int ticks = 0;
};

// Button that only reacts to the left mouse button; the right button is free for MIDI assignment.
class LeftClickOnlyButton final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void mouseDown(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) juce::TextButton::mouseDown(e); }
    void mouseUp(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) juce::TextButton::mouseUp(e); }
    void mouseDrag(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) juce::TextButton::mouseDrag(e); }
};
