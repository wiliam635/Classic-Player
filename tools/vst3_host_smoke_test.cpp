#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>
#include <cstring>
#include <memory>
#if JUCE_MAC
#import <Cocoa/Cocoa.h>
#elif JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif


// The hosted VST3 editor owns a native NSView/HWND in another module.
// JUCE paintEntireComponent on the HOST wrapper cannot rasterise that view.
static void flushNativeEvents()
{
   #if JUCE_MAC
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
   #elif JUCE_WINDOWS
    const auto end = GetTickCount64() + 500;
    do
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1);
    } while (GetTickCount64() < end);
   #endif
}
static int paintedPixels(const juce::Image& image)
{
    if (!image.isValid()) return 0;
    const auto background = image.getPixelAt(0, 0).getARGB();
    int count = 0;
    for (int y = 8; y < image.getHeight(); y += 17)
        for (int x = 8; x < image.getWidth(); x += 17)
            if (image.getPixelAt(x, y).getARGB() != background) ++count;
    return count;
}
static juce::Image captureNativeEditor(juce::DocumentWindow& host, juce::Component& editor)
{
    auto* peer = host.getPeer();
    if (peer == nullptr) return {};
   #if JUCE_MAC
    NSView* view = static_cast<NSView*>(peer->getNativeHandle());
    if (view == nil) return {};
    [view displayIfNeeded];
    const auto area = host.getLocalArea(&editor, editor.getLocalBounds());
    const NSRect rect = NSMakeRect(area.getX(), area.getY(), area.getWidth(), area.getHeight());
    NSBitmapImageRep* bitmap = [view bitmapImageRepForCachingDisplayInRect:rect];
    if (bitmap == nil) return {};
    [view cacheDisplayInRect:rect toBitmapImageRep:bitmap];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (png == nil) return {};
    return juce::ImageFileFormat::loadFrom([png bytes], static_cast<size_t>([png length]));
   #elif JUCE_WINDOWS
    struct Search { HWND window = nullptr; long long area = 0; } search;
    EnumChildWindows(static_cast<HWND>(peer->getNativeHandle()),
        [](HWND child, LPARAM data) -> BOOL
        {
            auto& found = *reinterpret_cast<Search*>(data);
            RECT rect {};
            if (IsWindowVisible(child) && GetClientRect(child, &rect))
            {
                const auto area = static_cast<long long>(rect.right) * rect.bottom;
                if (rect.right >= 200 && rect.bottom >= 200 && area >= found.area)
                { found.window = child; found.area = area; }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
    if (search.window == nullptr) return {};
    RECT rect {}; GetClientRect(search.window, &rect);
    const int width = rect.right, height = rect.bottom;
    HDC source = GetDC(search.window);
    if (source == nullptr) return {};
    HDC memory = CreateCompatibleDC(source);
    BITMAPINFO info {}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (memory == nullptr || bitmap == nullptr || pixels == nullptr)
    {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory != nullptr) DeleteDC(memory);
        ReleaseDC(search.window, source); return {};
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RedrawWindow(search.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    const auto copy = [&]
    {
        juce::Image image(juce::Image::ARGB, width, height, true);
        const auto* bytes = static_cast<const unsigned char*>(pixels);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto index = (static_cast<size_t>(y) * width + x) * 4;
                image.setPixelAt(x, y, juce::Colour(bytes[index+2], bytes[index+1], bytes[index]));
            }
        return image;
    };
    juce::Image image;
    if (BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY)) image = copy();
    if (paintedPixels(image) < 12)
    {
        std::memset(pixels, 0, static_cast<size_t>(width) * height * 4);
        if (PrintWindow(search.window, memory, PW_CLIENTONLY | 0x00000002)) image = copy();
    }
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(search.window, source);
    return image;
   #else
    return editor.createComponentSnapshot(editor.getLocalBounds());
   #endif
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Uso: vst3_host_smoke_test <pasta Classic Player.vst3>\n";
        return 64;
    }

    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    const juce::File bundle(juce::String::fromUTF8(argv[1]));
    if (!bundle.isDirectory())
    {
        std::cerr << "Bundle VST3 nao encontrado\n";
        return 1;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundle.getFullPathName());
    if (descriptions.isEmpty())
    {
        std::cerr << "O host nao descobriu nenhum instrumento no bundle VST3\n";
        return 2;
    }

    for (const auto* description : descriptions)
    {
        juce::String error;
        auto instance = format.createInstanceFromDescription(*description, 44100.0, 512, error);
        if (instance == nullptr)
        {
            std::cerr << "Falha ao instanciar " << description->name.toStdString()
                      << ": " << error.toStdString() << "\n";
            return 3;
        }
        if (!instance->hasEditor())
        {
            std::cerr << "VST3 sem editor grafico: " << description->name.toStdString() << "\n";
            return 4;
        }
        std::unique_ptr<juce::AudioProcessorEditor> editor(instance->createEditorAndMakeActive());
        if (editor == nullptr)
        {
            std::cerr << "Host nao conseguiu abrir o editor VST3\n";
            return 5;
        }
        editor->setSize(1000, 650);
        if (editor->getWidth() != 1000 || editor->getHeight() != 650)
        {
            std::cerr << "Editor VST3 nao aceitou redimensionamento\n";
            return 6;
        }
        juce::DocumentWindow hostWindow("Classic Player VST3 smoke test",
                                        juce::Colours::black,
                                        juce::DocumentWindow::closeButton);
        hostWindow.setContentNonOwned(editor.get(), true);
        hostWindow.centreWithSize(1000, 650);
        hostWindow.setVisible(true);
        flushNativeEvents();
        const auto snapshot = captureNativeEditor(hostWindow, *editor);
        if (snapshot.isValid())
        {
            juce::FileOutputStream output(juce::File::getCurrentWorkingDirectory().getChildFile("vst3-host-editor.png"));
            juce::PNGImageFormat png;
            if (output.openedOk()) png.writeImageToStream(snapshot, output);
        }
        hostWindow.setVisible(false);
        hostWindow.clearContentComponent();
        if (!snapshot.isValid())
        {
            std::cerr << "Editor VST3 nao produziu imagem\n";
            return 7;
        }
        const auto paintedSamples = paintedPixels(snapshot);
        std::cout << "Native editor snapshot " << snapshot.getWidth() << "x" << snapshot.getHeight()
                  << ", varied pixel samples: " << paintedSamples << "\n";
        if (paintedSamples < 12)
        {
            std::cerr << "Editor VST3 criou uma imagem, mas nao desenhou o conteudo da interface\n";
            return 8;
        }
        std::cout << "Host abriu, redimensionou e desenhou: "
                  << description->name.toStdString() << "\n";
    }
    return 0;
}
