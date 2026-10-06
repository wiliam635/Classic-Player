#include "UpdateChecker.h"
#include <array>

namespace
{
bool parseStableVersion(juce::String version, std::array<int, 3>& parts)
{
    if (version.startsWithIgnoreCase("v")) version = version.substring(1);
    const auto tokens = juce::StringArray::fromTokens(version, ".", "");
    if (tokens.size() != 3) return false;
    for (int i = 0; i < 3; ++i)
    {
        if (tokens[i].isEmpty() || tokens[i].length() > 6
            || !tokens[i].containsOnly("0123456789")) return false;
        parts[(size_t) i] = tokens[i].getIntValue();
    }
    return true;
}
bool isHttps(const juce::String& value)
{
    return value.startsWithIgnoreCase("https://") && juce::URL(value).getDomain().isNotEmpty();
}
}

ClassicPlayerUpdateChecker::Result ClassicPlayerUpdateChecker::parseManifest(
    const juce::var& manifest, const juce::String& installedVersion, const juce::String& platform)
{
    Result result;
    std::array<int, 3> latestParts {}, installedParts {};
    if (!manifest.isObject() || !manifest["version"].isString()
        || !parseStableVersion(manifest["version"].toString(), latestParts)
        || !parseStableVersion(installedVersion, installedParts)) return result;
    result.version = manifest["version"].toString();
    result.downloadUrl = manifest["downloads"][juce::Identifier(platform)].toString();
    result.notesUrl = manifest["releaseNotesUrl"].toString();
    result.notes = manifest["notes"].toString().substring(0, 4000);
    const bool newer = latestParts > installedParts;
    result.succeeded = !newer || isHttps(result.downloadUrl);
    result.available = newer && result.succeeded;
    if (!isHttps(result.notesUrl)) result.notesUrl.clear();
    return result;
}

bool ClassicPlayerUpdateChecker::start()
{
    bool idle = false;
    if (!state->checking.compare_exchange_strong(idle, true)) return false;
    state->completed.store(false);
    const auto shared = state;
    if (!juce::Thread::launch([shared]
    {
        Result result;
        const juce::String feedUrl = CLASSIC_PLAYER_UPDATE_FEED_URL;
        if (isHttps(feedUrl))
        {
            int status = 0;
            const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withExtraHeaders("Accept: application/json\r\nUser-Agent: Classic-Player/" JucePlugin_VersionString)
                .withConnectionTimeoutMs(5000).withNumRedirectsToFollow(3).withStatusCode(&status);
            if (auto stream = juce::URL(feedUrl).createInputStream(options);
                stream != nullptr && status == 200)
            {
                juce::MemoryBlock data;
                stream->readIntoMemoryBlock(data, 65537);
                if (data.getSize() <= 65536)
                {
                    const auto manifest = juce::JSON::parse(data.toString());
                    if (manifest.isObject() && manifest["version"].isString())
                    {
                       #if JUCE_MAC
                        constexpr auto platform = "macos";
                       #elif JUCE_WINDOWS
                        constexpr auto platform = "windows";
                       #else
                        constexpr auto platform = "linux";
                       #endif
                        result = parseManifest(manifest, JucePlugin_VersionString, platform);
                    }
                }
            }
        }
        {
            const std::lock_guard<std::mutex> lock(shared->mutex);
            shared->result = std::move(result);
        }
        shared->checking.store(false, std::memory_order_release);
        shared->completed.store(true, std::memory_order_release);
    }))
    {
        state->checking.store(false);
        return false;
    }
    return true;
}
bool ClassicPlayerUpdateChecker::takeResult(Result& result)
{
    if (!state->completed.exchange(false, std::memory_order_acquire)) return false;
    const std::lock_guard<std::mutex> lock(state->mutex);
    result = state->result;
    return true;
}
