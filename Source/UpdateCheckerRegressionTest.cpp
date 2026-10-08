#include "UpdateChecker.h"
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        const auto check = [](bool condition, const char* message)
        {
            if (!condition) throw std::runtime_error(message);
        };
        auto manifest = juce::JSON::parse(R"({"version":"2.0.10","downloads":{
            "macos":"https://classickeys.com.br/downloads/mac.pkg",
            "windows":"https://classickeys.com.br/downloads/windows.exe"},
            "releaseNotesUrl":"https://classickeys.com.br/releases/2.0.10"})");
        const auto mac = ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.9", "macos");
        check(mac.succeeded && mac.available, "Numeric version ordering failed at 2.0.9 -> 2.0.10");
        check(mac.downloadUrl.endsWith("mac.pkg"), "macOS selected another platform's installer");
        const auto windows = ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.9", "windows");
        check(windows.available && windows.downloadUrl.endsWith("windows.exe"), "Windows installer routing failed");
        check(!ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.10", "macos").available,
              "Installed release must not notify again");
        check(!ClassicPlayerUpdateChecker::parseManifest(manifest, "2.1.0", "macos").available,
              "Older releases must not offer a downgrade");
        manifest.getDynamicObject()->setProperty("version", "2.1.0-beta.1");
        check(!ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.3", "macos").available,
              "Prerelease was announced as stable");
        manifest.getDynamicObject()->setProperty("version", "invalid");
        check(!ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.3", "macos").succeeded,
              "Invalid manifest was presented as up to date");
        manifest.getDynamicObject()->setProperty("version", "v2.1.0");
        manifest["downloads"].getDynamicObject()->setProperty("macos", "http://classickeys.com.br/mac.pkg");
        check(!ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.3", "macos").available,
              "Insecure installer URL was accepted");
        manifest["downloads"].getDynamicObject()->removeProperty("macos");
        check(!ClassicPlayerUpdateChecker::parseManifest(manifest, "2.0.3", "macos").available,
              "Missing installer URL triggered a notification");
        check(!ClassicPlayerUpdateChecker::parseManifest(juce::JSON::parse("[]"), "2.0.3", "macos").succeeded,
              "Wrong JSON shape was accepted");
        std::cout << "Update feed validation passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
