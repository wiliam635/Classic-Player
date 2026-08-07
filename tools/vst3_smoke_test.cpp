#include <CoreFoundation/CoreFoundation.h>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "uso: vst3_smoke_test /caminho/plugin.vst3\n";
        return 64;
    }

    CFStringRef path = CFStringCreateWithCString(nullptr, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, path, kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    CFRelease(path);

    if (bundle == nullptr || !CFBundleLoadExecutable(bundle))
    {
        std::cerr << "FALHA: bundle não carregou\n";
        if (bundle != nullptr) CFRelease(bundle);
        return 1;
    }

    auto symbol = [bundle](const char* name)
    {
        auto string = CFStringCreateWithCString(nullptr, name, kCFStringEncodingASCII);
        auto pointer = CFBundleGetFunctionPointerForName(bundle, string);
        CFRelease(string);
        return pointer;
    };

    using Entry = bool (*)(CFBundleRef);
    using Exit = bool (*)();
    using Factory = void* (*)();
    auto entry = reinterpret_cast<Entry>(symbol("bundleEntry"));
    auto exit = reinterpret_cast<Exit>(symbol("bundleExit"));
    auto getFactory = reinterpret_cast<Factory>(symbol("GetPluginFactory"));

    if (entry == nullptr || exit == nullptr || getFactory == nullptr)
    {
        std::cerr << "FALHA: símbolos VST3 obrigatórios ausentes\n";
        CFRelease(bundle);
        return 2;
    }
    if (!entry(bundle))
    {
        std::cerr << "FALHA: bundleEntry recusou o módulo\n";
        CFRelease(bundle);
        return 3;
    }

    auto* factory = getFactory();
    std::cout << "bundleEntry: OK\n";
    std::cout << "GetPluginFactory: " << (factory != nullptr ? "OK" : "FALHA") << "\n";
    exit();
    CFBundleUnloadExecutable(bundle);
    CFRelease(bundle);
    return factory != nullptr ? 0 : 4;
}
