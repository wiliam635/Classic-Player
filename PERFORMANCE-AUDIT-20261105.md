# Performance audit — 2026-11-05

Base: d5513b52e79673391bb392fbb599868fe6150163.

## Changes

- Cache the EQ graph frequency grid and filter response. Recompute the response only when its eleven controls change; continue repainting the live spectrum and control changes.
- Transform spectrum data only when the audio capture write counter changes. Do not run the EQ FFT while its component is hidden.
- Stop the hidden Analog oscilloscope timer in preset-only mode. Avoid hidden meter repaints.
- Disable FluidSynth internal reverb explicitly. SF2 room processing already uses the Classic per-layer reverb; chorus remains independent.

## Existing safeguards retained

Per-layer polyphony settings, idle-engine checks, effect tails, coefficient caches, sustain/note-off processing, and morphs from zero volume remain intact. Muted/zero-volume engines were not blindly bypassed, because that can strand notes or break morphs and tails.

## Outstanding risk

SF2 loading still holds the processor callback lock and the engine lock during file loading. Switching a SoundFont while playing can therefore block rendering. A complete correction requires preparing a replacement synth outside those locks and swapping it safely, including concurrent sample-rate changes and failed loads. This audit does not claim that risk is fixed.

## Validation and limits

Source diff whitespace checks passed. FluidSynth reverb API declaration was checked against the installed header. No compilation, audio regression suite, listening test, or CPU benchmark was run for this audit. Improvements are code-level reductions of repeated work, not measured CPU percentages.

Before release: compare SF2/NPNO sustained arpeggios, 44.1/48 kHz and 128/256/512-frame buffers, EQ open/closed, reverb/chorus tails, saved presets and instrument switching on the same hardware. Check SF2 room behavior after disabling the redundant internal effect.
