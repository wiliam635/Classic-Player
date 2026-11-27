// Classic Converter: turns NPNO pianos and NSMP samples into Classic Player files (.ckp / .cks).
#include "ClassicSoundConvert.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <iostream>

namespace
{
juce::String text(const char* utf8) { return juce::String::fromUTF8(utf8); }

juce::String megabytes(juce::int64 bytes) { return juce::String((double) bytes / 1.0e6, 1) + " MB"; }

juce::String convertOne(const juce::File& source, const juce::File& folder, bool& ok)
{
    juce::File created;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto result = ClassicSoundConvert::convert(source, folder, created);
    ok = result.wasOk();
    if (!ok) return source.getFileName() + ": " + text("não convertido. ") + result.getErrorMessage();
    return source.getFileName() + " (" + megabytes(source.getSize()) + ") -> " + created.getFileName() + " ("
         + megabytes(created.getSize()) + "), conferido, "
         + juce::String((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) + " s";
}

class ConverterWindowContent final : public juce::Component,
                                     public juce::FileDragAndDropTarget,
                                     private juce::Thread
{
public:
    ConverterWindowContent() : juce::Thread("Classic Converter")
    {
        folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Classic Converter");
        title.setText(text("Converte pianos .npno e amostras .nsmp, .nsmp3 e .nsmp4 para a Classic Player"),
                      juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centredLeft);
        chooseFiles.setButtonText("ESCOLHER ARQUIVOS...");
        chooseFolder.setButtonText("PASTA DE DESTINO...");
        openFolder.setButtonText("ABRIR PASTA");
        chooseFiles.onClick = [this] { pickFiles(); };
        chooseFolder.onClick = [this] { pickFolder(); };
        openFolder.onClick = [this] { if (folder.createDirectory().wasOk()) folder.revealToUser(); };
        log.setMultiLine(true);
        log.setReadOnly(true);
        log.setScrollbarsShown(true);
        for (auto* component : std::initializer_list<juce::Component*> { &title, &chooseFiles, &chooseFolder,
                                                                        &openFolder, &folderLabel, &log })
            addAndMakeVisible(component);
        showFolder();
        append(text("Escolha os arquivos ou arraste para esta janela. Os originais não são alterados."));
        setSize(720, 420);
    }

    ~ConverterWindowContent() override { stopThread(-1); } // never abandons a half-written conversion

    bool busy() const noexcept { return working.load(); }
    void explainBusy() { append(text("Aguarde o fim da conversão para fechar.")); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(14);
        title.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);
        auto buttons = area.removeFromTop(34);
        chooseFiles.setBounds(buttons.removeFromLeft(220));
        buttons.removeFromLeft(10);
        chooseFolder.setBounds(buttons.removeFromLeft(200));
        buttons.removeFromLeft(10);
        openFolder.setBounds(buttons.removeFromLeft(140));
        area.removeFromTop(8);
        folderLabel.setBounds(area.removeFromTop(24));
        area.removeFromTop(8);
        log.setBounds(area);
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (const auto& path : files)
            if (ClassicSoundConvert::outputExtension(juce::File(path)).isNotEmpty()) return true;
        return false;
    }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        juce::Array<juce::File> dropped;
        for (const auto& path : files) dropped.add(juce::File(path));
        enqueue(dropped);
    }

private:
    struct Job { juce::File source, folder; };

    void showFolder() { folderLabel.setText("Destino: " + folder.getFullPathName(), juce::dontSendNotification); }

    void append(const juce::String& line)
    {
        log.moveCaretToEnd();
        log.insertTextAtCaret(line + "\n");
    }

    void pickFiles()
    {
        chooser = std::make_unique<juce::FileChooser>("Escolha os arquivos Nord", juce::File{},
            "*.npno;*.NPNO;*.nsmp;*.NSMP;*.nsmp3;*.NSMP3;*.nsmp4;*.NSMP4");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::canSelectMultipleItems,
                             [this](const juce::FileChooser& dialog) { enqueue(dialog.getResults()); });
    }

    void pickFolder()
    {
        chooser = std::make_unique<juce::FileChooser>("Pasta para os arquivos convertidos", folder);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser& dialog)
                             {
                                 const auto selected = dialog.getResult();
                                 if (selected == juce::File{}) return;
                                 folder = selected;
                                 showFolder();
                             });
    }

    void enqueue(const juce::Array<juce::File>& files)
    {
        int added = 0;
        {
            const juce::ScopedLock guard(queueLock);
            for (const auto& file : files)
                if (ClassicSoundConvert::outputExtension(file).isNotEmpty())
                {
                    queue.add({ file, folder });
                    ++added;
                }
        }
        if (added == 0) return;
        working.store(true);
        append("Na fila: " + juce::String(added) + (added == 1 ? " arquivo" : " arquivos"));
        if (!isThreadRunning()) startThread();
        notify();
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            Job job;
            bool found = false;
            {
                const juce::ScopedLock guard(queueLock);
                if (!queue.isEmpty()) { job = queue.removeAndReturn(0); found = true; }
                else working.store(false);
            }
            if (!found) { wait(-1); continue; }
            post("Convertendo " + job.source.getFileName() + "...");
            bool ok = false;
            post(convertOne(job.source, job.folder, ok));
        }
    }

    void post(const juce::String& line)
    {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ConverterWindowContent>(this), line]
        {
            if (safe != nullptr) safe->append(line);
        });
    }

    juce::Label title, folderLabel;
    juce::TextButton chooseFiles, chooseFolder, openFolder;
    juce::TextEditor log;
    juce::File folder;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::CriticalSection queueLock;
    juce::Array<Job> queue;
    std::atomic<bool> working { false };
};

class ConverterWindow final : public juce::DocumentWindow
{
public:
    ConverterWindow()
        : juce::DocumentWindow("Classic Converter", juce::Colour(0xff141d2b),
                               juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        content = new ConverterWindowContent();
        setContentOwned(content, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    ConverterWindowContent* content = nullptr;
};

class ClassicConverterApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Classic Converter"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise(const juce::String&) override
    {
        // Headless use: "Classic Converter" --convert <file> <destination folder>
        const auto arguments = getCommandLineParameterArray();
        if (arguments.size() == 3 && arguments[0] == "--convert")
        {
            bool ok = false;
            const auto cwd = juce::File::getCurrentWorkingDirectory();
            std::cout << convertOne(cwd.getChildFile(arguments[1].unquoted()),
                                    cwd.getChildFile(arguments[2].unquoted()), ok) << std::endl;
            setApplicationReturnValue(ok ? 0 : 1);
            quit();
            return;
        }
        window = std::make_unique<ConverterWindow>();
    }

    void shutdown() override { window.reset(); }

    void systemRequestedQuit() override
    {
        if (window != nullptr && window->content->busy()) { window->content->explainBusy(); return; }
        quit();
    }

private:
    std::unique_ptr<ConverterWindow> window;
};
}

START_JUCE_APPLICATION(ClassicConverterApplication)
