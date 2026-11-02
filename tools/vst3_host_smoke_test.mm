#include <juce_audio_utils/juce_audio_utils.h>
#import <Cocoa/Cocoa.h>

void flushClassicNativeEvents()
{
    @autoreleasepool
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    }
}

juce::Image captureClassicNativeEditor(juce::DocumentWindow& host, juce::Component& editor)
{
    auto* peer = host.getPeer();
    if (peer == nullptr) return {};
    @autoreleasepool
    {
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
    }
}
