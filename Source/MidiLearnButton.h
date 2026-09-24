#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

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
