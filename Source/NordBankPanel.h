// Included by PluginEditor.cpp after its shared UI helpers.
class NordBankPanel final : public juce::Component
{
public:
    NordBankPanel(ClassicPlayerAudioProcessor& p, int layer, std::function<void()> changed)
        : processor(p), index(layer), onChanged(std::move(changed))
    {
        for (auto* b : { &importButton, &banksButton }) { flatButton(*b); addAndMakeVisible(*b); }
        addAndMakeVisible(status);
        status.setColour(juce::Label::textColourId, juce::Colour(text));
        status.setJustificationType(juce::Justification::topLeft);
        importButton.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser>("Importar banco Nord Stage 4", juce::File{}, "*.ns4pbundle");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safe = juce::Component::SafePointer<NordBankPanel>(this)](const juce::FileChooser& fc) {
                    if (safe == nullptr || !fc.getResult().existsAsFile()) return;
                    const WaitCursor wait;
                    const auto result = safe->processor.importNordBank(fc.getResult());
                    if (result.failed()) safe->error(result.getErrorMessage());
                    else { safe->refresh(); safe->showBanks(); }
                });
        };
        banksButton.onClick = [this] { showBanks(); };
        setSize(620, 130); refresh();
    }
    void resized() override {
        auto r = getLocalBounds().reduced(2);
        auto row = r.removeFromTop(30);
        importButton.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2));
        banksButton.setBounds(row.reduced(2));
        status.setBounds(r.reduced(4));
    }
private:
    struct WaitCursor {
        WaitCursor() { juce::MouseCursor::showWaitCursor(); }
        ~WaitCursor() { juce::MouseCursor::hideWaitCursor(); }
    };
    void error(const juce::String& message) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Bancos Nord", message);
    }
    void refresh() {
        const auto banks = NordBankEngine::banks();
        banksButton.setButtonText("BANCOS IMPORTADOS (" + juce::String(banks.size()) + ")");
        const auto path = processor.nordBanks().path(index);
        status.setText(path.isEmpty()
            ? "Importe o banco e escolha um programa em BANCOS IMPORTADOS.\nPiano/B3, NSMP4 e synth analogico adaptado. FM/WAVE ainda indisponiveis."
            : juce::File(path).getFileNameWithoutExtension() + " - Cena "
                + (processor.nordBanks().scene(index) == 0 ? "I" : "II") + "\n"
                + processor.nordBanks().description(index), juce::dontSendNotification);
    }
    void showBanks() {
        struct Selection { juce::File file; int scene; };
        auto choices = std::make_shared<std::vector<Selection>>();
        juce::PopupMenu menu;
        for (const auto& bank : NordBankEngine::banks()) {
            juce::PopupMenu entries;
            for (const auto& program : NordBankEngine::programs(bank)) {
                if(program.file.hasFileExtension("nsmp4")) {
                    choices->push_back({program.file,0});entries.addItem(int(choices->size()),program.name);continue;
                }
                juce::PopupMenu scenes;
                for (int scene = 0; scene < 2; ++scene) {
                    choices->push_back({ program.file, scene });
                    scenes.addItem(int(choices->size()), juce::String(scene == 0 ? "Cena I" : "Cena II")
                        + (program.playable[size_t(scene)] ? "" : " - " + program.reason[size_t(scene)]),
                        program.playable[size_t(scene)], program.file.getFullPathName() == processor.nordBanks().path(index)
                            && scene == processor.nordBanks().scene(index));
                }
                entries.addSubMenu(program.name, scenes);
            }
            menu.addSubMenu(bank.getFileName(), entries);
        }
        if (choices->empty()) menu.addItem(1, "Nenhum banco importado", false);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&banksButton),
            [safe = juce::Component::SafePointer<NordBankPanel>(this), choices](int id) {
                if (safe == nullptr || id <= 0 || size_t(id) > choices->size()) return;
                const auto choice = (*choices)[size_t(id - 1)];
                const WaitCursor wait;
                const auto result = safe->processor.loadNordBankProgram(safe->index, choice.file, choice.scene);
                if (result.failed()) safe->error(result.getErrorMessage());
                safe->refresh();
                if (safe->onChanged) safe->onChanged();
            });
    }
    ClassicPlayerAudioProcessor& processor;
    int index;
    std::function<void()> onChanged;
    juce::TextButton importButton { "IMPORTAR BANCO NORD" }, banksButton;
    juce::Label status;
    std::unique_ptr<juce::FileChooser> chooser;
};
