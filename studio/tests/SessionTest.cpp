#include "../src/Session.h"
#include "../src/MixerState.h"
#include "../src/TransportState.h"
#include <cassert>
#include <cmath>

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

    classicplayer::MixerState mixer;
    mixer.syncFromSession(loaded.tracks);
    assert(mixer.size() == 2);
    assert(mixer.get(0).name == "Classic Player");
    assert(mixer.get(1).pan < 0.0f);
    mixer.get(0).solo = true;
    mixer.get(1).muted = true;
    assert(mixer.anySoloed());
    assert(mixer.get(0).isAudible(true));
    assert(!mixer.get(1).isAudible(true));

    classicplayer::TransportState transport;
    transport.setSampleRate(48000.0);
    transport.play();
    transport.advanceSamples(24000);
    assert(std::abs(transport.position() - 0.5) < 0.000001);
    transport.pause();
    transport.advanceSamples(48000);
    assert(std::abs(transport.position() - 0.5) < 0.000001);
    transport.stop();
    assert(transport.positionSamples() == 0);

    file.deleteFile();
    return 0;
}
