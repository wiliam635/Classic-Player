# Analog: approved browser fallback (2026-08-28)

Reference: the user's screenshot and confirmation selected the sound produced
when the browser lab says its optional Huovilainen filter failed to load.
The reference is therefore the native Web Audio lowpass, not the external WASM
ladder. No external filter code or LGPL dependency was incorporated.

## Implementation

- All 32 presets imported from analog-browser-lab/app.js, with their names,
  waveforms, independent oscillator tuning, levels and envelope times.
- Explicit browserCompatible config, serialized per layer. Old programs without
  this flag retain the legacy engine. Select a factory preset to opt in.
  Newly created Analog layers start with the browser Solo Lead.
- Second-order lowpass using the Web Audio coefficients and Q in dB:
  https://www.w3.org/TR/webaudio-1.0/#filters-characteristics
- Per-voice sine LFO adds mod * 420 Hz to cutoff, rather than multiplying
  logarithmic cutoff. No factory pitch vibrato or filter envelope.
- Exponential amplitude attack/decay, 8 ms legato target, release time/5 target,
  fixed 0.34 voice level and 0.45 browser master. MIDI velocity does not change
  the factory voice level. The app's layer/master gains and optional effects
  remain available and can affect comparison loudness.
- No always-on mixer saturation or extra post-filter in browser mode.
- Oscillator 1 pitch no longer transposes oscillators 2 and 3.
- Exact cutoff values survive editor and parameter transfer; Solo Lead's
  3900 ms decay is no longer clamped to 3000 ms.
- Preset loading resets hidden synth settings instead of inheriting the prior
  patch. Noise starts at zero: the reference exposed a noise value without
  actually creating a noise generator.
- Existing MIDI routing, sustain, pitch bend and click-safe transition support
  stay in place. Synth config changes synchronize with the audio callback.

## Validation and limits

The audio regression suite renders all presets at 44.1 and 48 kHz, compares
127/512 sample blocks and different MIDI velocities, and exercises mono
retargeting, note release and polyphonic adjacent notes. An independent sine,
modulated lowpass and exponential VCA calculation is compared with the engine.
The processor round-trip test includes the browser mode, decay and LFO depth.

This is not a bit-identical port of a browser's oscillator implementation:
the native engine retains its band-limited oscillator approximations and
3 ms click-safe retarget correction. Physical MIDI and subjective timbre
equivalence need the user's listening comparison. No Hammond DSP/UI changes.

To compare an existing program: select the desired factory preset again;
the editor identifies this path as BROWSER 12 dB. Turn off layer reverb,
compressor and master EQ if comparing with the dry browser lab, and match
the listening level. Save a new program after approval.

## Editor close regression

The compact editor used to cache the mix controls when opened and write all
four back on close. Selecting a preset changed the processor cutoff but left
that cache stale; closing therefore replaced the selected filter setting.
The Analog common controls now use individual APVTS slider attachments and
closing only refreshes the strip. No values are committed on close.
Regression coverage uses the same common-control factory as the actual dialog:
select a preset, change volume, apply external parameter updates, destroy and
recreate the controls, and verify exact cutoff and mix values are preserved.
