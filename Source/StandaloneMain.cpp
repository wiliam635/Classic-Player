#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_devices/juce_audio_devices.h>
#if JUCE_MAC
 #include <CoreAudio/CoreAudio.h>
#endif
#include <algorithm>
#include <cstring>

namespace
{
juce::File webRoot()
{
    const auto application = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
#if JUCE_WINDOWS
    return application.getParentDirectory().getChildFile("Resources").getChildFile("WebUI");
#else
    return application.getChildFile("Contents").getChildFile("Resources").getChildFile("WebUI");
#endif
}

juce::String mimeTypeFor(const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".jpeg" || extension == ".jpg") return "image/jpeg";
    if (extension == ".png") return "image/png";
    if (extension == ".pem") return "application/x-pem-file";
    if (extension == ".json") return "application/json";
    return "application/octet-stream";
}

std::optional<juce::WebBrowserComponent::Resource> provideWebResource(const juce::String& rawPath)
{
    auto path = rawPath.upToFirstOccurrenceOf("?", false, false);
    path = juce::URL::removeEscapeChars(path).trimCharactersAtStart("/");
    if (path.isEmpty()) path = "index.html";
    if (path.contains("..")) return std::nullopt;

    const auto root = webRoot();
    const auto file = root.getChildFile(path);
    if (!file.existsAsFile() || !file.isAChildOf(root))
    {
        juce::Logger::writeToLog("Recurso web não encontrado: " + path);
        return std::nullopt;
    }

    juce::MemoryBlock block;
    if (!file.loadFileAsData(block)) return std::nullopt;

    std::vector<std::byte> bytes(block.getSize());
    std::memcpy(bytes.data(), block.getData(), block.getSize());
    return juce::WebBrowserComponent::Resource { std::move(bytes), mimeTypeFor(file) };
}

#if JUCE_MAC
class CoreAudioController
{
public:
    juce::var configuration() const
    {
        const auto current = defaultOutputDevice();
        juce::Array<juce::var> list;
        for (const auto device : outputDevices())
        {
            auto* item = new juce::DynamicObject();
            const auto rate = scalar<Float64>(device, kAudioDevicePropertyNominalSampleRate,
                                              48000.0, kAudioObjectPropertyScopeGlobal);
            const auto buffer = scalar<UInt32>(device, kAudioDevicePropertyBufferFrameSize,
                                               0, kAudioObjectPropertyScopeGlobal);
            const auto latencyFrames = scalar<UInt32>(device, kAudioDevicePropertyLatency, 0)
                                     + scalar<UInt32>(device, kAudioDevicePropertySafetyOffset, 0)
                                     + buffer;
            item->setProperty("uid", stringProperty(device, kAudioDevicePropertyDeviceUID));
            item->setProperty("name", stringProperty(device, kAudioObjectPropertyName));
            item->setProperty("channels", outputChannels(device));
            item->setProperty("selected", device == current);
            item->setProperty("sampleRate", rate);
            item->setProperty("bufferSize", static_cast<int>(buffer));
            item->setProperty("latencyMs", rate > 0.0 ? latencyFrames * 1000.0 / rate : 0.0);
            item->setProperty("bufferSizes", juce::var(availableBufferSizes(device)));
            item->setProperty("sampleRates", juce::var(availableSampleRates(device)));
            list.add(juce::var(item));
        }

        auto* root = new juce::DynamicObject();
        root->setProperty("devices", juce::var(list));
        root->setProperty("driver", "CoreAudio");
        return juce::var(root);
    }

    juce::var selectDevice(const juce::String& uid) const
    {
        const auto device = findDevice(uid);
        if (device == kAudioObjectUnknown) return result(false, "Interface de áudio não encontrada");
        AudioObjectPropertyAddress address { kAudioHardwarePropertyDefaultOutputDevice,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
        auto value = device;
        const auto status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &address,
                                                       0, nullptr, sizeof(value), &value);
        return result(status == noErr,
                      status == noErr ? "Saída CoreAudio alterada" : "A interface recusou a seleção");
    }

    juce::var setBufferSize(const juce::String& uid, int requested) const
    {
        const auto device = findDevice(uid);
        if (device == kAudioObjectUnknown) return result(false, "Interface de áudio não encontrada");
        AudioObjectPropertyAddress address { kAudioDevicePropertyBufferFrameSize,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
        Boolean settable = false;
        if (AudioObjectIsPropertySettable(device, &address, &settable) != noErr || settable == 0)
            return result(false, "O driver não permite alterar o buffer");
        auto value = static_cast<UInt32>(juce::jmax(16, requested));
        const auto status = AudioObjectSetPropertyData(device, &address, 0, nullptr,
                                                       sizeof(value), &value);
        return result(status == noErr,
                      status == noErr ? "Buffer alterado" : "O driver recusou o buffer escolhido");
    }

    juce::var setSampleRate(const juce::String& uid, double requested) const
    {
        const auto device = findDevice(uid);
        if (device == kAudioObjectUnknown) return result(false, "Interface de áudio não encontrada");
        AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
        Boolean settable = false;
        if (AudioObjectIsPropertySettable(device, &address, &settable) != noErr || settable == 0)
            return result(false, "O driver não permite alterar o sample rate");
        auto value = static_cast<Float64>(requested);
        const auto status = AudioObjectSetPropertyData(device, &address, 0, nullptr,
                                                       sizeof(value), &value);
        return result(status == noErr,
                      status == noErr ? "Sample rate alterado" : "O driver recusou o sample rate");
    }

private:
    static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                              AudioObjectPropertyScope scope)
    {
        return { selector, scope, kAudioObjectPropertyElementMain };
    }

    template <typename Type>
    static Type scalar(AudioObjectID device, AudioObjectPropertySelector selector, Type fallback,
                       AudioObjectPropertyScope scope = kAudioDevicePropertyScopeOutput)
    {
        auto property = address(selector, scope);
        Type value = fallback;
        UInt32 size = sizeof(value);
        return AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &value) == noErr
                   ? value : fallback;
    }

    static juce::String fromCFString(CFStringRef value)
    {
        if (value == nullptr) return {};
        const auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),
                                                                kCFStringEncodingUTF8) + 1;
        std::vector<char> text(static_cast<size_t>(capacity));
        return CFStringGetCString(value, text.data(), capacity, kCFStringEncodingUTF8)
                   ? juce::String::fromUTF8(text.data()) : juce::String{};
    }

    static juce::String stringProperty(AudioObjectID device, AudioObjectPropertySelector selector)
    {
        auto property = address(selector, kAudioObjectPropertyScopeGlobal);
        CFStringRef value = nullptr;
        UInt32 size = sizeof(value);
        if (AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &value) != noErr)
            return {};
        const auto result = fromCFString(value);
        if (value != nullptr) CFRelease(value);
        return result;
    }

    static std::vector<AudioDeviceID> allDevices()
    {
        auto property = address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, nullptr, &size) != noErr)
            return {};
        std::vector<AudioDeviceID> result(size / sizeof(AudioDeviceID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr,
                                       &size, result.data()) != noErr) return {};
        return result;
    }

    static int outputChannels(AudioDeviceID device)
    {
        auto property = address(kAudioDevicePropertyStreamConfiguration,
                                kAudioDevicePropertyScopeOutput);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size) != noErr || size == 0)
            return 0;
        juce::MemoryBlock memory(size, true);
        auto* buffers = static_cast<AudioBufferList*>(memory.getData());
        if (AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, buffers) != noErr)
            return 0;
        int channels = 0;
        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
            channels += static_cast<int>(buffers->mBuffers[i].mNumberChannels);
        return channels;
    }

    static std::vector<AudioDeviceID> outputDevices()
    {
        auto devices = allDevices();
        devices.erase(std::remove_if(devices.begin(), devices.end(),
                                     [](auto device) { return outputChannels(device) == 0; }),
                      devices.end());
        return devices;
    }

    static AudioDeviceID defaultOutputDevice()
    {
        auto property = address(kAudioHardwarePropertyDefaultOutputDevice,
                                kAudioObjectPropertyScopeGlobal);
        AudioDeviceID device = kAudioObjectUnknown;
        UInt32 size = sizeof(device);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &size, &device);
        return device;
    }

    static AudioDeviceID findDevice(const juce::String& uid)
    {
        for (const auto device : outputDevices())
            if (stringProperty(device, kAudioDevicePropertyDeviceUID) == uid) return device;
        return kAudioObjectUnknown;
    }

    static juce::Array<juce::var> availableBufferSizes(AudioDeviceID device)
    {
        AudioValueRange range { 32.0, 2048.0 };
        auto property = address(kAudioDevicePropertyBufferFrameSizeRange,
                                kAudioObjectPropertyScopeGlobal);
        UInt32 size = sizeof(range);
        AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &range);
        juce::Array<juce::var> values;
        for (const int candidate : { 32, 64, 128, 256, 512, 1024, 2048, 4096 })
            if (candidate >= range.mMinimum && candidate <= range.mMaximum) values.add(candidate);
        return values;
    }

    static juce::Array<juce::var> availableSampleRates(AudioDeviceID device)
    {
        auto property = address(kAudioDevicePropertyAvailableNominalSampleRates,
                                kAudioObjectPropertyScopeGlobal);
        UInt32 size = 0;
        juce::Array<juce::var> values;
        if (AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size) != noErr) return values;
        std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
        if (AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, ranges.data()) != noErr)
            return values;
        for (const int candidate : { 44100, 48000, 88200, 96000, 176400, 192000 })
            if (std::any_of(ranges.begin(), ranges.end(), [candidate](const auto& range)
                            { return candidate >= range.mMinimum && candidate <= range.mMaximum; }))
                values.add(candidate);
        return values;
    }

    juce::var result(bool ok, const juce::String& message) const
    {
        auto response = configuration();
        response.getDynamicObject()->setProperty("ok", ok);
        response.getDynamicObject()->setProperty("message", message);
        return response;
    }
};
#else
// Windows uses JUCE's device abstraction so the same WebUI can list ASIO,
// WASAPI and DirectSound without linking to a platform-specific API.  ASIO
// appears here only when the build was configured with the Steinberg SDK.
class CoreAudioController
{
public:
    CoreAudioController()
    {
        const auto result = deviceManager.initialise(0, 2, nullptr, true);
        if (result.isNotEmpty())
            juce::Logger::writeToLog("Falha ao inicializar dispositivos de áudio: " + result);
    }

    juce::var configuration() const
    {
        juce::Array<juce::var> list;
        auto* current = deviceManager.getCurrentAudioDevice();
        const auto currentType = current != nullptr ? current->getTypeName() : juce::String{};
        const auto currentName = current != nullptr ? current->getName() : juce::String{};

        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            const auto names = type->getDeviceNames(false);
            juce::Logger::writeToLog("Tipo de áudio encontrado: " + type->getTypeName()
                                     + " (" + juce::String(names.size()) + " dispositivo(s))");
            for (const auto& name : names)
            {
                auto* item = new juce::DynamicObject();
                const auto uid = type->getTypeName() + ":" + name;
                item->setProperty("uid", uid);
                item->setProperty("name", name);
                item->setProperty("driver", type->getTypeName());
                item->setProperty("channels", 2);
                item->setProperty("selected", currentType == type->getTypeName() && currentName == name);
                item->setProperty("sampleRate", current != nullptr ? current->getCurrentSampleRate() : 48000.0);
                item->setProperty("bufferSize", current != nullptr ? current->getCurrentBufferSizeSamples() : 256);
                item->setProperty("latencyMs", current != nullptr && current->getCurrentSampleRate() > 0.0
                                      ? current->getCurrentBufferSizeSamples() * 1000.0
                                        / current->getCurrentSampleRate() : 0.0);
                item->setProperty("bufferSizes", juce::var(juce::Array<juce::var>{ 32, 64, 128, 256, 512, 1024 }));
                item->setProperty("sampleRates", juce::var(juce::Array<juce::var>{ 44100, 48000, 88200, 96000 }));
                list.add(juce::var(item));
            }
        }

        auto* root = new juce::DynamicObject();
        root->setProperty("devices", juce::var(list));
        root->setProperty("driver", currentType.isNotEmpty() ? currentType : "Nenhum");
        root->setProperty("asioAvailable", listContainsDriver(list, "ASIO"));
        return juce::var(root);
    }

    juce::var selectDevice(const juce::String& uid) const
    {
        const auto separator = uid.indexOfChar(':');
        if (separator <= 0) return result(false, "Dispositivo de áudio inválido");
        const auto typeName = uid.substring(0, separator);
        const auto deviceName = uid.substring(separator + 1);
        auto* type = findType(typeName);
        if (type == nullptr) return result(false, "Tipo de driver não encontrado: " + typeName);
        // JUCE 9 switches the driver type through a void-returning API.
        // The actual open result is reported by setAudioDeviceSetup below.
        deviceManager.setCurrentAudioDeviceType(typeName, true);
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = deviceName;
        setup.inputDeviceName.clear();
        const auto openError = deviceManager.setAudioDeviceSetup(setup, true);
        if (openError.isNotEmpty()) return result(false, openError);
        juce::Logger::writeToLog("Saída de áudio selecionada: " + typeName + " / " + deviceName);
        return result(true, "Saída de áudio alterada");
    }

    juce::var setBufferSize(const juce::String&, int requested) const
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.bufferSize = juce::jmax(16, requested);
        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        return result(error.isEmpty(), error.isEmpty() ? "Buffer alterado" : error);
    }

    juce::var setSampleRate(const juce::String&, double requested) const
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.sampleRate = requested;
        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        return result(error.isEmpty(), error.isEmpty() ? "Sample rate alterado" : error);
    }

private:
    mutable juce::AudioDeviceManager deviceManager;

    juce::AudioIODeviceType* findType(const juce::String& name) const
    {
        for (auto* type : deviceManager.getAvailableDeviceTypes())
            if (type->getTypeName() == name) return type;
        return nullptr;
    }

    static bool listContainsDriver(const juce::Array<juce::var>& list, const juce::String& driver)
    {
        for (const auto& value : list)
            if (auto* object = value.getDynamicObject(); object != nullptr
                && object->getProperty("driver").toString() == driver) return true;
        return false;
    }

    juce::var result(bool ok, const juce::String& message) const
    {
        auto response = configuration();
        response.getDynamicObject()->setProperty("ok", ok);
        response.getDynamicObject()->setProperty("message", message);
        return response;
    }
};
#endif

class ClassicPlayerWebView final : public juce::WebBrowserComponent,
                                   private juce::MidiInputCallback,
                                   private juce::Timer
{
public:
    ClassicPlayerWebView()
        : ClassicPlayerWebView(std::make_shared<CoreAudioController>())
    {
    }

private:
    explicit ClassicPlayerWebView(std::shared_ptr<CoreAudioController> controller)
        : WebBrowserComponent(makeOptions(controller)), coreAudio(std::move(controller))
    {
        setOpaque(true);
        goToURL(getResourceProviderRoot());
        startTimer(1500);
    }

public:
    ~ClassicPlayerWebView() override
    {
        stopTimer();
        for (auto& input : midiInputs) input->stop();
        midiInputs.clear();
    }

    bool pageAboutToLoad(const juce::String& url) override
    {
        juce::Logger::writeToLog("Carregando interface: " + url);
        return url.startsWith(getResourceProviderRoot());
    }

    void pageFinishedLoading(const juce::String& url) override
    {
        juce::Logger::writeToLog("Interface Full HD carregada: " + url);
        pageReady = true;
        refreshMidiInputs(true);
    }

private:
    static Options makeOptions(const std::shared_ptr<CoreAudioController>& controller)
    {
        return Options{}
            .withNativeIntegrationEnabled()
            .withNativeFunction("get_audio_configuration",
                                [controller](const auto&, auto complete)
                                {
                                    complete(controller->configuration());
                                })
            .withNativeFunction("select_audio_device",
                                [controller](const auto& args, auto complete)
                                {
                                    complete(controller->selectDevice(args[0].toString()));
                                })
            .withNativeFunction("set_audio_buffer",
                                [controller](const auto& args, auto complete)
                                {
                                    complete(controller->setBufferSize(args[0].toString(),
                                                                        static_cast<int>(args[1])));
                                })
            .withNativeFunction("set_audio_sample_rate",
                                [controller](const auto& args, auto complete)
                                {
                                    complete(controller->setSampleRate(args[0].toString(),
                                                                        static_cast<double>(args[1])));
                                })
            .withResourceProvider(provideWebResource);
    }

    void timerCallback() override
    {
        refreshMidiInputs(false);
    }

    void refreshMidiInputs(bool force)
    {
        const auto devices = juce::MidiInput::getAvailableDevices();
        juce::String fingerprint;
        for (const auto& device : devices) fingerprint << device.identifier << "|" << device.name << ";";
        if (!force && fingerprint == midiFingerprint) return;

        midiFingerprint = fingerprint;
        for (auto& input : midiInputs) input->stop();
        midiInputs.clear();

        juce::Array<juce::var> deviceList;
        for (const auto& device : devices)
        {
            if (auto input = juce::MidiInput::openDevice(device.identifier, this))
            {
                input->start();
                midiInputs.push_back(std::move(input));
            }

            auto* object = new juce::DynamicObject();
            object->setProperty("id", device.identifier);
            object->setProperty("name", device.name);
            deviceList.add(juce::var(object));
        }

        juce::Logger::writeToLog("Entradas MIDI nativas: " + juce::String(deviceList.size()));
        if (pageReady) emitEventIfBrowserIsVisible("classic_midi_devices", juce::var(deviceList));
    }

    void handleIncomingMidiMessage(juce::MidiInput* source,
                                   const juce::MidiMessage& message) override
    {
        juce::Array<juce::var> bytes;
        const auto* raw = message.getRawData();
        for (int i = 0; i < message.getRawDataSize(); ++i) bytes.add(static_cast<int>(raw[i]));

        auto* object = new juce::DynamicObject();
        object->setProperty("id", source != nullptr ? source->getIdentifier() : "midi");
        object->setProperty("name", source != nullptr ? source->getName() : "Controlador MIDI");
        object->setProperty("data", juce::var(bytes));
        const juce::var payload(object);
        const juce::Component::SafePointer<ClassicPlayerWebView> safe(this);

        juce::MessageManager::callAsync([safe, payload]
        {
            if (safe != nullptr)
                safe->emitEventIfBrowserIsVisible("classic_midi_message", payload);
        });
    }

    bool pageReady = false;
    std::shared_ptr<CoreAudioController> coreAudio;
    juce::String midiFingerprint;
    std::vector<std::unique_ptr<juce::MidiInput>> midiInputs;
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow("Classic Player", juce::Colour(0xff07101a),
                         DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setResizeLimits(960, 540, 1920, 1080);
        setContentOwned(new ClassicPlayerWebView(), true);

        const auto display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto available = display != nullptr ? display->userArea
                                                   : juce::Rectangle<int>(0, 0, 1600, 900);
        centreWithSize(juce::jmin(1600, static_cast<int>(available.getWidth() * 0.94f)),
                       juce::jmin(900, static_cast<int>(available.getHeight() * 0.92f)));
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class ClassicPlayerApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Classic Player"; }
    const juce::String getApplicationVersion() override { return CLASSIC_PLAYER_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override
    {
        logger.reset(juce::FileLogger::createDefaultAppLogger(
            "Classic Player", "Classic-Player.log",
            "Classic Player Standalone " CLASSIC_PLAYER_VERSION));
        juce::Logger::setCurrentLogger(logger.get());
        juce::Logger::writeToLog("Iniciando interface Full HD protegida");
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override
    {
        mainWindow.reset();
        juce::Logger::writeToLog("Aplicativo encerrado");
        juce::Logger::setCurrentLogger(nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String&) override
    {
        if (mainWindow != nullptr) mainWindow->toFront(true);
    }

private:
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<MainWindow> mainWindow;
};
}

START_JUCE_APPLICATION(ClassicPlayerApplication)
