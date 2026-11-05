# Follow-up: Hammond, Analog, Continuous Pad and layer menu

Baseline: 91f076674fdffcc60d3cff0a74432109516c9579.

## Changes

- Hammond: ordered active-voice list instead of 256 render calls per sample; attack/release sample counts computed once per block.
- Analog: unchanged exponential-envelope coefficients computed once per block instead of per voice/sample. Filter quality, saturation and oversampling retained.
- Continuous Pad: ordered list of playing/fading pads and block-level loop geometry; stopped slots no longer traversed for every audio frame. Crossfades, filter tails and STOP retained.
- Layer popup: prevent duplicate opening through the component and its recursive mouse listener. Dismiss active menus before scheduling removal on the message thread.
- Hammond output calibration: +10.24 dB (3.25x) after tone/effects, without changing the layer fader, drawbars, drive or saved instrument-level control. This intentionally raises existing Hammond performances too.

## Validation

- Before/after performance-only output: 278,528 Hammond/Pad samples and 655,360 Analog samples were bit identical before the intentional Hammond gain change.
- Three-run local engine medians: Hammond approximately 72–95% less render time in the tested long-attack/release workloads; Pad approximately 40–69% less. Pad absolute cost was already below 1 ms for 96 x 256 frames, so it did not account for extreme overall app CPU in this probe.
- Analog varied by preset/note count: most cases improved up to approximately 13%, with two median regressions of 1.3% and 3.8%. No uniform gain is claimed. Dense nonlinear polyphony remains costly.
- Actual reference-level test includes the processor DX7 trim of -18 dB. Same C4 note, velocity 100, layer gain 80%, effects off: calibrated Hammond RMS 0.0285239; first factory DX7 patch 0.0293258; difference -0.240825 dB. Half-volume fader response passed. Other presets naturally differ in loudness.
- UI regression delivered the context click twice, observed exactly one modal menu, selected Delete through its keyboard action, then verified one layer removed and no modal menu remaining without an outside click.
- Full audio regression passed: 32 browser presets, reference envelope/filter, 44.1/48 kHz and block partition checks.
- Full Hammond/UI/pad regression passed: continuous loops/resampling/crossfade/STOP/MIDI/state/layer moves, Hammond CC1/channel/threshold/host/routed paths, compressor, idle wake, reference output gain and editor persistence.
- Source whitespace checks passed. Raw measurements: performance/followup-20261105.json.

## Limits

These are local engine/test results, not release installers, Windows runtime measurements or a guarantee of the same CPU reduction on the customer machine. The existing local SDK compatibility shim described by the previous audit was used only in test build objects. No installer/Actions build was triggered. The selected voice limits were preserved; heavy Moog polyphony still needs a target-hardware test.

## Fixed Moog voice limit (subsequent user request)

Moog/Analog now allocates exactly 32 voice slots and ignores variable polyphony requests. The Analog selector is hidden and its row uses two velocity columns. Defaults, factory presets, runtime config, exported state and legacy-state restoration expose 32. The common SF2 routing store retains its own minimum; the Analog projection and engine enforce the effective 32 independently. Other engines retain their existing voice controls. Mono remains available.

Audio regression includes a 96-note sustain/voice-stealing scenario: an old 256-voice request must produce exactly the same output as a 32-voice request, with a compile-time 32-slot assertion. Factory-preset, processor restore and UI-selector visibility checks cover the effective limit. No installer/Actions run was requested for these source changes.
