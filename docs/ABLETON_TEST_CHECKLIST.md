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

## Samples (Phase 2)
- [ ] ✅ Drag a WAV from Finder / Live's browser onto a pad: name + length appear, it plays from MIDI and mouse
- [ ] ✅ Drag a file onto the green screen: loads into the selected pad
- [ ] ✅ LOAD opens a file chooser; CLEAR empties the pad
- [ ] ✅ AIFF and 24-bit / 44.1 kHz files load and play at the right pitch in a 48 kHz set
- [ ] ✅ Loading a long break while the transport plays causes no dropout
- [ ] ✅ Waveform shows on the screen for the selected pad; playback cursor moves while it plays
- [ ] ✅ Drag S / E markers: the START / END knobs follow, and vice versa; region outside is dimmed
- [ ] ✅ Wheel / pinch zooms around the pointer, drag scrolls, double-click zooms out
- [ ] ✅ PITCH +/-12 st, GAIN, PAN, REVERSE all audible; knobs automatable (pad01_pitch etc.)
- [ ] ✅ Move END while a long sample plays: playback stops at the new end without a click
- [ ] ✅ Retrigger fast (1/32 at 174 BPM): no clicks

## Pad modes (Phase 3)
- [ ] ✅ PLAY page: set a break pad to LOOP; holding the pad / MIDI note loops it, releasing stops it without a click
- [ ] ✅ LOOP + SYNC with Live playing: the break stays locked to Live's metronome for 64 bars; start it mid-bar and it enters in time
- [ ] ✅ Change Live's tempo while it loops: it follows; loop-brace wraps keep it on the grid
- [ ] ✅ RATE 1/2 and 2x: half / double speed, still on the grid
- [ ] ✅ Put open + closed hat pads in CHOKE 1: the closed hat cuts the open one cleanly
- [ ] ✅ M / S keys mute / solo pads (tiles darken, red / green lamps)
- [ ] ✅ VEL 0%: soft and hard hits are equally loud
- [ ] ✅ FIX key on a pad: soft hits from the controller play at full level; off again: velocity works
- [ ] ✅ NOTE: LEARN then hit a pad on your controller: that note now plays the selected pad; save / reopen keeps it

## Slicing (Phase 4)
- [ ] ✅ Drop your favourite break on pad 1, SLICER key, AUTO SLICE: markers land on the hits (check by ear by clicking slices in SLICE mode)
- [ ] ✅ SENS up / down then AUTO again: more / fewer slices; COUNT 8 keeps the main hits
- [ ] ✅ Drag, add (double-click), delete (double-click a marker) markers; RESET brings back the AUTO result
- [ ] ✅ SPREAD: pads fill with slices; play the break from your controller; attacks sound punchy (not softened)
- [ ] ✅ Save, close, reopen: markers and spread pads all come back

## SWAP groups (0.4.1)
- [ ] ✅ Two breaks on pads 1 and 2 (LOOP, SYNC on, AUTO SLICE each), both SWAP group 1. Live playing: hit pad 1, let go: it keeps playing
- [ ] ✅ Hit pad 2 mid-bar: pad 2 flashes, then takes over on pad 1's next slice, in time, starting cleanly on a hit (by ear: no flam, no gap, no double hit)
- [ ] ✅ Chop between them for a few minutes from your controller: always on the grid vs Live's metronome
- [ ] ✅ AT = BAR on pad 2: the swap waits for the next bar line
- [ ] ✅ Hit the playing pad again: stops at its next slice. PANIC stops everything
- [ ] ✅ Save, close, reopen: groups and AT settings come back

## Group buses & mixer (Phase 5)
- [ ] ✅ Kick + snare pads to GROUP A, hats to B (SOUND page A-D keys): tiles show GRP A / GRP B
- [ ] ✅ MIXER key: four strips; the LED bars move for the groups that are playing
- [ ] ✅ Group B fader down / PAN / M / S: only the hats change; muting mid-break doesn't click
- [ ] ✅ Automate `Group B Level` in Live: fader follows, no zipper noise
- [ ] ✅ Click a strip's name plate: the selected pad moves to that group
- [ ] ✅ Save, close, reopen: routing and mixer settings come back

## iRig Pads lights (0.5.1)
- [ ] ✅ LIGHTS -> pick iRig Pads: the key's lamp goes green; pads with samples glow dim green
- [ ] ✅ Test lights: all pads red, then green, then both. If red and green are the wrong way round, Swap red / green
- [ ] ✅ Play: the pad you hit (and pads triggered from Live's clips) light green while they sound
- [ ] ✅ SWAP: the queued pad flashes red until it takes over
- [ ] ✅ If the wrong pads light: Learn pad layout, hit each pad as it lights red
- [ ] ✅ Unplug / replug the iRig: lights come back within a few seconds. Close the set: all lights off

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
- [ ] ✅ Load samples on several pads, change pitch / start / end / reverse, save, reopen: all restored
- [ ] Phase 5+: patterns, FX states and snapshots all restored

## Sample recovery
- [ ] ✅ Rename a sample on disk, reopen the set: pad shows **MISSING** (red), set opens, other pads fine
- [ ] ✅ Load the file again onto that pad (drag or LOAD): plays again; saving fixes the path
- [ ] Phase 8: relocate missing files in one go
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
