# Ableton Live test checklist

Tick each line per build. ✅ = applies now (Phase 1). Later items are filled in
as each phase lands — keep running the earlier ones as regression checks.

Setup: Live 11 or 12, 44.1 or 48 kHz, buffer 256. One MIDI track with
TwistedBreaks; a second with a Drum Rack for timing comparison.

## Installation & scanning
- [ ] ✅ **VST3 appears**: Settings → Plug-Ins → VST3 system folders on → Rescan → Plug-Ins → Twisted Genetics → TwistedBreaks (VST3)
- [ ] ✅ **AU appears**: Audio Units v2 on → Rescan → TwistedBreaks (AU). If missing: `killall -9 AudioComponentRegistrar`, rescan
- [ ] ✅ Loads as an **instrument** (MIDI track, not audio effect); window opens at 1200×800, resizes 75–150 % keeping aspect
- [ ] ✅ Open and close the editor 20× quickly — no crash, no leak warnings

## MIDI pad triggering
- [ ] ✅ Notes **C1–D#2 (36–51)** fire pads 1–16 in reading order (top-left = pad 1); pad lamp flashes; MIDI lamp blinks; LAST NOTE shows the note
- [ ] ✅ Velocity changes loudness (play soft vs hard from a controller)
- [ ] ✅ Mouse: clicking near the top of a pad is louder than near the bottom
- [ ] ✅ Notes outside 36–51 do nothing (but still blink the MIDI lamp)
- [ ] ✅ Rapid retriggering (1/32 notes at 174 BPM) has no clicks
- [ ] ✅ Computer-keyboard MIDI (M key) plays pads
- [ ] Phase 3: pad MIDI note can be reassigned and survives reload

## Automation
- [ ] ✅ **Master Level** appears in the automation chooser; drawn automation moves the knob and audio; no zipper noise on fast ramps
- [ ] ✅ **Master Limiter** toggles from automation (switch flips)
- [ ] ✅ **Panic** automated 0→1 stops all sound with no click
- [ ] ✅ "Configure" mode: touching the knob adds it to the device's parameter list
- [ ] ✅ Map Master Level to a MIDI CC (Cmd-M)
- [ ] Phase 6: FX on/off per group automatable and MIDI-mappable

## BPM sync & transport
- [ ] ✅ **TEMP / BPM** readout matches Live (try 160, 174, 87.5); follows tempo automation live
- [ ] ✅ **BAR.BEAT.16** matches Live's arrangement position while playing
- [ ] ✅ **ХОД (RUN)** lamp lights while playing, off when stopped
- [ ] ✅ **СИНХР (SYNC)** lamp is lit (host is sending position)
- [ ] ✅ 32-step lamps advance once per 1/16 and wrap every 2 bars, in time with the metronome
- [ ] ✅ Change the set to 7/8: **МЕТР** shows 7/8; bar/beat counts correctly
- [ ] ✅ **Transport start/stop**: start from bar 1, from mid-bar, and with the loop brace on — step lamps resync instantly after each loop wrap
- [ ] ✅ Stopping transport doesn't cut ringing pads (tails decay naturally)
- [ ] Phase 3: **loop sync** — synced loop stays locked to the grid for 64 bars, re-aligns after loop-brace wraps
- [ ] Phase 7: **sequencer sync** — sequenced pad vs Drum Rack on the same steps: no flam when soloing both; still tight after relocating and after tempo changes

## Project save / reload
- [ ] ✅ Set Master Level −7.5 dB, limiter OFF, save, close Live, reopen: both restored
- [ ] ✅ Duplicate the track: the copy has identical settings
- [ ] ✅ Undo/redo a Master Level change works
- [ ] ✅ Save the set with the VST3 and with the AU version on separate tracks; both reload
- [ ] Phase 2+: samples, slices, patterns, FX states and snapshots all restored

## Sample recovery (Phase 2/8)
- [ ] Rename a sample on disk, reopen the set: pad shows **MISSING** (red), set opens, other pads fine
- [ ] Relocate the file from the pad: plays again; saving fixes the path
- [ ] Collect All and Save: set reopens on another machine with samples

## Snapshot switching (Phase 8)
- [ ] Switch snapshots Immediate / Next Beat / Next Bar while playing — change lands exactly on the boundary
- [ ] No dropouts, clicks or CPU spike while switching every beat for 2 minutes

## Panic
- [ ] ✅ PANIC (red STOP) button: all sound stops within ~5 ms, no click, button flashes
- [ ] ✅ MIDI CC 120 (All Sound Off) acts as panic
- [ ] Phase 6/7: panic also kills stutter, beat repeat, delay/reverb tails and sequencer-triggered voices

## Latency
- [ ] ✅ Live's device delay compensation shows **0 samples** for the plugin (hover the device title bar with Delay Compensation on)
- [ ] ✅ Recorded MIDI → audio (resample TwistedBreaks onto an audio track): hits line up with the grid

## CPU use
- [ ] ✅ Idle, editor closed: Live CPU meter ≈ unchanged vs empty track
- [ ] ✅ Editor open, transport running: UI stays smooth (lamps/meter ~30 fps), no audio dropouts at 128-sample buffer
- [ ] ✅ 8 instances on 8 tracks with editors closed: no dropouts at 256 samples
- [ ] Phase 6: all 16 FX on all 4 groups at once — note CPU %, no dropouts at 256
- [ ] ✅ Freeze and flatten a track: frozen audio matches live playback

## Stress
- [ ] ✅ Change Live's sample rate 44.1 → 48 → 96 kHz with the plugin playing
- [ ] ✅ Change buffer size 64 → 2048 while playing
- [ ] ✅ Export audio (offline render faster than real-time): timing and sound identical to real-time
