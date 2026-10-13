# Samplit clean-room recreation notes

This document records the implementation boundary for the Classic Player
recreation. It is based on the shipped Samplit 2.1.1 package and manual, plus
observable macOS Mach-O metadata. It does not copy the Samplit executable,
source code, proprietary resources, or product artwork.

## Observed Samplit baseline

- native macOS application, distributed as an Apple `.pkg`;
- universal binary containing `x86_64` and legacy `i386` slices;
- C++ runtime (`libc++`) plus Objective-C/AppKit classes;
- Carbon and Cocoa UI APIs;
- CoreAudio, AudioToolbox, AudioUnit and Core MIDI;
- custom views, menus, timers, drag-and-drop and Core Graphics drawing;
- packaged bitmap controls for transport, keyboard, meters, EQ and sample
  navigation.

The original application is old and Intel-oriented. A modern recreation should
preserve the user-facing workflow while replacing Carbon-era implementation
details with current JUCE and macOS APIs.

## Current Classic Player mapping

| Samplit capability area | Classic Player implementation |
| --- | --- |
| Native C++ audio application | JUCE C++20 processor and Standalone target |
| CoreAudio/Core MIDI | JUCE audio device and MIDI layers |
| Sample/instrument layers | SF2 layers plus DX7, analog, drum pads and a polyphonic audio-sample layer |
| Transport/recording | Non-blocking WAV recorder and standalone recording controls |
| Keyboard and MIDI interaction | JUCE MIDI keyboard, routing and MIDI Learn |
| Mixer/effects | Per-layer gain, cutoff, reverb, compressor and master EQ |
| Preset/project workflow | DAW state, standalone programs and Live Set slots |
| AU/VST plugin delivery | JUCE Standalone, VST3 and Audio Unit targets |

## Implemented sample slice

The clean-room branch now includes a first-class audio-sample layer:

1. WAV, AIFF, FLAC, OGG and MP3 decoding through JUCE;
2. polyphonic MIDI note triggering with root-note pitch tracking;
3. linear interpolation, note-range/octave routing and a short release envelope;
4. source selection in the layer menu and standalone editor;
5. sample paths persisted in DAW/program state;
6. an offline smoke test covering decode, MIDI response and rendered output.

The next focused slice is waveform preview plus editable start/end and loop
regions. Original Samplit assets remain outside the product until replacement
artwork is created or separately licensed.

The branch is intentionally clean-room: behavior is reconstructed from public
observations and tests, while implementation and assets are new.
