#include "../src/Session.h"
#include <cassert>

int main()
{
    classicplayer::Session source;
    source.tempoBpm = 128.0;
    source.tracks.getReference(0).name = "Classic Player";
    classicplayer::SessionTrack audioTrack;
    audioTrack.name = "Audio 1";
    audioTrack.instrument = false;
    audioTrack.volume = 0.8f;
    audioTrack.pan = -0.2f;
    source.tracks.add(audioTrack);

    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("classic-player-studio-session-test.xml");
    assert(source.save(file));
    classicplayer::Session loaded;
    assert(loaded.load(file));
    assert(loaded.tempoBpm == 128.0);
    assert(loaded.tracks.size() == 2);
    assert(loaded.tracks[0].name == "Classic Player");
    assert(!loaded.tracks[1].instrument);
    file.deleteFile();
    return 0;
}
