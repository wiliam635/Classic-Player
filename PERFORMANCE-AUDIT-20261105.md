# Performance audit — 2026-11-05

Base: d5513b52e79673391bb392fbb599868fe6150163.

## Changes

- Cache the EQ graph frequency grid and filter response. Recompute the response only when its eleven controls change; continue repainting the live spectrum and control changes.
- Transform spectrum data only when the audio capture write counter changes. Do not run the EQ FFT while its component is hidden.
- Stop the hidden Analog oscilloscope timer in preset-only mode. Avoid hidden meter repaints.
- Disable FluidSynth internal reverb explicitly. SF2 room processing already uses the Classic per-layer reverb; chorus remains independent.

## Existing safeguards retained

Per-layer polyphony settings, idle-engine checks, effect tails, coefficient caches, sustain/note-off processing, and morphs from zero volume remain intact. Muted/zero-volume engines were not blindly bypassed, because that can strand notes or break morphs and tails.

## Loading corrections

- The normal SF2 import/selection path builds and reads a replacement synth outside both the engine rendering lock and the processor callback lock. Only installation takes those locks. Retired synth resources are destroyed after those scopes.
- Failed SF2 reads keep the existing instrument. Request generations reject stale or already installed preparations, and audio preparation invalidates pending synths so their sample rate cannot be stale.
- Config changes made while loading are retained; note/sustain state and per-synth parameter caches are cleared on a successful replacement.
- Normal NPNO imports now decode before acquiring the callback lock, using the existing prepared-instrument API. NSMP imports also validate the layer type before installing the prepared sample, avoiding an unload of a layer that changed during decoding.

## Validation

A dedicated Mac engine test executable was built and run with a generated one-preset sine-wave SF2 fixture. Passed: empty startup; transactional loading (invalid file keeps old sound, audio renders while a replacement remains prepared, stale/double installations rejected, current config retained, old notes removed, sample-rate reconfiguration and unload invalidate pending replacements); normal sound/filter output and 48/44.1 kHz transitions; release tails at both rates. The final processor/editor source translation units passed compiler syntax checks with the project-generated flags. Source diff whitespace checks passed.

These checks are not a complete application build or installer, and do not establish Windows runtime behavior, real-bank audio fidelity, or a percentage CPU improvement. The local FluidSynth archive emitted deployment-target warnings (built for macOS 26 versus this test target 13.3); this test is not a compatibility certification. No installer or Actions run was triggered.

## Remaining limits

Performance/session restoration and layer cloning have existing outer callback-lock scopes; this change does not promise uninterrupted playback during those whole-session operations. Normal imports remain synchronous for the UI, and temporary memory use includes both old and replacement SF2 until installation completes. Large SF2 banks still need a memory/latency test on the target hardware. NPNO/NSMP decoder behavior was not revalidated with all real user banks in this audit.

Before release: compare SF2/NPNO sustained arpeggios, 44.1/48 kHz and 128/256/512-frame buffers, EQ open/closed, reverb/chorus tails, saved presets and instrument switching on the same hardware. Check SF2 room behavior after disabling the redundant internal effect. No measured CPU percentages are claimed.

## Moog/Analog follow-up

- Replace the 256-slot per-sample scan with an ordered active-voice list. Rebuild after MIDI/arpeggiator events and compact when envelopes finish. Voice allocation/stealing, selected polyphony and oscillator summation order remain unchanged.
- Compute the layer pitch modulation ratio once per sample, not once per voice. Compute the browser base cutoff once per block.
- Cache keyboard tracking and filter coefficients when their exact inputs are unchanged. Modulated cutoff still updates at audio rate; no filter quality/oversampling reduction.
- Skip native cutoff calculations in the browser-compatible branch and the unused analog oscillator mix in FM/WAVE branches.

A three-run before/after probe covered 20 cases: 1/64/128/256 requested notes, two browser presets and native analog/FM-H/FM-I, sustain, release, pitch wheel, modulation and parameter changes. Its 655,360 output samples were bit identical. Local median rendering-time reductions were approximately 4–18% for polyphonic cases and 29–60% for one-note cases; these are engine workload timings, not overall app CPU measurements. Raw measurements and method: `performance/moog-20261105.json`.

A separate before/after edge probe covered WAVE, Arp/Poly/Gate, native/browser mono and MIDI panic; all 393,216 samples were bit identical. The existing audio regression suite also passed, including all 32 browser presets, 44.1/48 kHz, legato/release and block-partition checks. An attempted nonlinear-state reuse was discarded because timings were inconsistent; the original saturation operations were retained.

High real polyphony with sustain still costs CPU: nonlinear oversampled filters execute for every sounding voice. Selecting 64 voices for dense pads, and mono for leads/basses when musically appropriate, remains a practical load control. No automatic reduction of saved polyphony was introduced. No installer/Actions build was triggered. Windows performance remains unmeasured.

The final single-pass recheck again produced bit-identical output and passed the audio suite. Its timings were variable, including an 11% slowdown in one browser/64-note case, while the three-run median for that case improved by 4%. Both measurement sets are retained (`performance/moog-20261105-verification.json`); a uniform gain across presets or hardware is not claimed. Real-time deadline and long-session measurements on the customer machine are still needed.
