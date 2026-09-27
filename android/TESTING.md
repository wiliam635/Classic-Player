# Android audio regression

Run **Android - emulator audio regression** manually in GitHub Actions on the
branch containing this workflow. GitHub only exposes manual dispatch after the
workflow exists on the default branch. Until then, an empty commit containing
`[run android tests]` on `codex/fix-user-refresh` runs this workflow. Other pushes
skip its job. It does not publish an installer. It compiles an x86_64 test APK; production ABI defaults
remain ARM. Test dependencies are confined to the instrumentation APK.

The suite exercises the actual native renderer offline for SF2, DX7, Analog and
Hammond: audible Note On, Note Off, pedal hold/release, SF2 preset loading, and
128 input notes routed to each of six layers while gains move. A separate smoke
test launches the app and captures the mixer. It does not yet navigate every
editor or automate the document picker.

Download **Android-emulator-audio-report**, including on failed runs:

- JUnit/HTML results identify failing assertions.
- `audio-regression/*.wav` contains the rendered note/release and stress audio.
- JSON includes RMS levels, SF2 load time, render mean/max, full-scale sample
  counts and adjacent-sample jumps. Jump/clipping metrics are observations, not
  automatic proof of clicks or DSP overload.
- `mixer.png` and `logcat.txt` help diagnose launch/layout/native crashes.

The SF2 fixture is a small original generated sine sample, not a representative
large piano bank. DX7 uses the existing bundled bank. A failure in pedal release
is expected until the identified routed-note bookkeeping defect is fixed; do not
weaken the assertion to make the report green. The suite establishes a baseline
without changing synthesis, sustain or polyphony settings in the shipping app.

128 input notes is a stress scenario, **not proof of 128 simultaneous surviving
voices**. Region layering, voice stealing and per-engine release characteristics
need voice-count instrumentation and additional tests. No emulator timing is a
pass/fail promise for the Samsung tablet, and offline PCM does not measure
AudioTrack underruns or USB routing.

With a local Android SDK/JDK 17/Gradle 8.9 and running emulator:

```sh
gradle -p android -PtestAbis=x86_64 :app:connectedDebugAndroidTest
```

On a physical ARM tablet, omit `-PtestAbis=x86_64`. Required follow-up hardware
checks: large user SF2 import without UI/audio stalls, AudioTrack underruns and
render deadline measurements, USB reconnect/actual routed device, sustain,
six-layer chords and fader movement. Keep 128 voices per layer as the requirement.
