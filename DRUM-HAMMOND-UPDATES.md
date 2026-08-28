# Drum pads and Hammond follow-up

- The drum layer now exposes its existing Gain parameter below the pads.
  Its audio path applies this gain with a 20 ms ramp, and observes layer
  enable/mute. Gain is saved using the existing layer parameter.
  Multiple drum layers share the existing pad bank and contribute their
  respective gains to the mix; sample playback is advanced only once.
- Hammond control callbacks ignore unchanged widget values, including delayed
  notifications after opening the editor. A true drawbar, percussion or
  timbre-control edit marks the sound custom; volume and Leslie performance
  changes retain the selected registration name.
- CC1 is dedicated to Leslie: 0–63 chorale, 64–127 fast, on the layer's
  configured MIDI channel/input. It takes priority over old CC1 assignments,
  never changes the drawbars, and does not mark the registration custom.
  The existing rotor acceleration/deceleration remains in place.
- Regression coverage: drum unity/half/zero gain, ramp, mute/unmute and save;
  Hammond preset selection, unchanged slider callbacks, close/reopen and
  state restoration; CC1 thresholds, MIDI channel and host/routed paths.

No workflow dispatch is performed by these changes.
