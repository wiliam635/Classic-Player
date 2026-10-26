# Nord compatibility in Classic Player 3.0.1

This branch starts from the approved 3.0.1 source. It does not embed the OpenNord
website, application, or AGPL-licensed source code.

| File/function | Classic Player status |
| --- | --- |
| `.npno` piano library | Dedicated NPNO layer, unchanged |
| `.nsmp` (Sample Library 2) | Nord Sample layer: keyboard/velocity zones, tuning, gain, PCM and loops |
| `.nsmp3` (Sample Library 3) | Same layer and controls; needs validation with a real `.nsmp3` file |
| `.nsmp4` (Sample Library 4) | Existing layer support, crossfaded loop seams |
| WAV export | Export each sample zone to an individual 24-bit WAV without replacing existing files |
| Stage 4 programs `.ns4p` | Not yet imported; requires parameter translation and matching local piano/sample libraries |
| Stage 4 piano/organ/synth presets `.ns4n/.ns4o/.ns4y` | Not yet imported; a file reference alone does not contain every required sound engine or library |
| Nord USB transfer or changing hardware slots | Not implemented; writing to a keyboard would require device-specific hardware testing and backup/restore safeguards |

The sample decoder rejects corrupt checksums, invalid zones and oversized decoded
audio. The NPNO and Nord Sample layers remain separate, and a performance stores
the imported sample path so it can be restored later.

OpenNord's Stage 4 parameter browser is useful as a behavioral reference, but its
AGPL-3.0-or-later source cannot be copied into this application under its current
distribution model. Nord program/preset support must be independently implemented
or use a compatible, properly attributed library, and validated against real
Stage 4 files before it is described as supported.
