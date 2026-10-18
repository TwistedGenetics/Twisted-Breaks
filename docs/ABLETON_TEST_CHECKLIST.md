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

## Performance FX (Phase 6)
- [ ] ✅ Break on a pad in group A playing. GRP A, click LPF: filtered; FX key: turn CUTOFF / RES while it plays (no zipper)
- [ ] ✅ Hold STUTTER (M): it waits for the next 1/16 and stutters in time with Live; release: straight back
- [ ] ✅ REPEAT (set DIV 1/8 in the editor): repeats the last 1/8, on the beat. REVERSE: plays the last beat backwards, in time
- [ ] ✅ PITCH: tape-stop dive; GATE: chops on the grid; CRUSH / RATE / DRIVE / TAPE / COMB: sound right, no clicks switching on / off
- [ ] ✅ DELAY / REVERB throws: switch on for a snare, off again: the echoes / tail carry on
- [ ] ✅ MIDI: notes C3-D#4 from your controller play FX 1-16 of the selected group (LOCKED toggles, MOMENTARY while held)
- [ ] ✅ Change a key to MOMENTARY / LOCKED in the FX editor; save, reopen: modes, group and FX settings come back
- [ ] ✅ Record FX automation in Live (arm automation, click keys): plays back

## Sequencer (Phase 7)
- [ ] ✅ SEQUENCER key: program a kick / snare / hat pattern by clicking cells; drag up / down on steps for velocity
- [ ] ✅ Play in Live: it plays in time with the metronome; stop / start / loop-brace / jump around: always on the right step
- [ ] ✅ LEN 12 against Live's 4/4: polymetric loop; STEP 1/8 and 1/32 change the speed
- [ ] ✅ Select a pad (click its lane number), ARM, play the pad from the iRig while Live runs: the hits appear in its lane, quantised to REC Q, and play back next time round (no doubled hits)
- [ ] ✅ REC Q 1/32: off-grid hits play back where you played them
- [ ] ✅ Save, close, reopen: the pattern comes back; automate `Sequencer Run` to drop the pattern in and out

## Step features (Phase 9)
- [ ] ✅ Hat lane: RAT x3 / x4 on a few steps: rolls in time with a Drum Rack hat on 1/32s
- [ ] ✅ CHANCE 50 % on ghost snares: varies each time round, ratchets come all-or-nothing
- [ ] ✅ PITCH / REV / SLICE on a break pad's steps (pad in SLICE or one-shot mode): the right slice, direction and pitch, next plain step normal
- [ ] ✅ FX STUTTER, HIT off, on the last step of a bar while a break loop runs: stutters for exactly that step, the loop isn't restarted
- [ ] ✅ COND 1:2 / 2:2 / 3:4 with a 4-bar loop brace: same steps every time round; FIRST plays once after pressing play
- [ ] ✅ MIDI-map `Sequencer Fill` to an iRig pad: FILL steps play while held, !FILL steps drop out
- [ ] ✅ Save, close, reopen (and a set saved with 0.8): every step setting comes back

## Sequence pads (0.14)
- [ ] ✅ A break pad with a chopped lane (SLICE / PITCH steps): MODE SEQL, LAUNCH BEAT, hit it while Live plays: it comes in on the next beat and loops; hit again: stops on the next beat
- [ ] ✅ SEQ1: one pass then silence; hit again for another pass; LEN 8 for a half-bar fill
- [ ] ✅ 0.14.1: hit a SEQ pad mid-bar: it joins in time with Live's playhead (its lane playhead sits on the same column as the sequencer's), not from step 1
- [ ] ✅ Two SEQ pads with different chopped patterns in SWAP group 1, AT SLICE: they cut each other on the next step, in time; try BEAT / BAR, and FROM STEP 1
- [ ] ✅ Play them from the iRig; lights flash while a launch / stop / hand-off waits
- [ ] ✅ Loop brace on: launched sequences follow the playhead back round the wrap

## 0.27: LOOP CONT
- [ ] PLAY page MODE has CONT after SEQL; old sets' modes unchanged; the tile says CONT
- [ ] Live playing: hit a CONT pad - it loops; let go - it carries on; hit again - it stops
- [ ] Stop Live: LOOP pads stop, the CONT pad keeps playing in time (STRETCH PITCH / BEAT / TEX / AKAI at Live's last tempo; OFF runs free)
- [ ] With Live stopped: hit a CONT pad to start it, hit again to stop it
- [ ] Press play: the CONT loop slides back onto the bar (a short fade, no click) and stays in time with the arrangement
- [ ] Red STOP stops it; changing its MODE stops it; a choke group / SWAP group still works on it
- [ ] Mikro / Push: CONT pads light as playing while latched, also with Live stopped
- [ ] MIDI OUT recording: the clip replays the start and stop hits (the loop starts and stops on playback)

## 0.26: STRETCH on the PLAY page
- [ ] An old set: loops that had SYNC on show STRETCH PITCH and sound the same; loops with SYNC off show OFF; one-shots with a stretch set show it
- [ ] Right-click a pad: no Stretch submenu (Render Akai, Layers, TWIST, copy / paste still there)
- [ ] A LOOP break, STRETCH BEAT: change Live's tempo 160 -> 175: it stays on the bar, the pitch stays put; PITCH: the pitch follows the tempo
- [ ] TEX and AKAI / AK.R on a loop: in time, pitch kept, the Akai buzz on AKAI; PITCH knob changes only the pitch
- [ ] Hit the loop mid-bar: it enters in place; Live loop brace jumps: it re-locks
- [ ] Change STRETCH while the loop plays: it switches without stopping
- [ ] The number above STRETCH: amber once typed in; click it: Double it on a loop playing at half speed fixes it; gaps / grain submenus work
- [ ] Double-click the selector: OFF; Cmd-Z after choosing a mode puts the old one back
- [ ] One-shot / SLICE pads: all modes behave as in 0.23-0.25

## 0.25.2: controllers found on their own
- [ ] Open a set that had Push 3 / Mikro mode off: the key at the bottom left now says CONTROLLERS; its menu has no Push 3 mode / Mikro MK3 mode items, just a status line for each
- [ ] Mikro plugged in, Maschine app closed: within ~2 s the pads light and the screen shows PADS A (no menu needed); CONTROLLERS says Playing
- [ ] Unplug the Mikro: status says not found; plug back in: it comes back by itself
- [ ] Open NI's Maschine app: it takes the Mikro, the plugin lets go (status says why); close the app: the plugin picks it up again
- [ ] Push 3 in Live mode: Push behaves as normal, CONTROLLERS says Found - press USER; press USER: the grid lights and plays the plugin; USER again: Push is Live's again
- [ ] Two Twisted Breaks: click each window in turn; Push / Mikro follow the last one clicked
- [ ] CONTROLLERS > Mikro MK3 test still runs (the Mikro pauses meanwhile) and the Mikro comes back when you stop it
- [ ] Export a WAV while Push is in User mode: Push keeps playing this plugin (the export copy doesn't grab it)

## 0.25.1: record on the plugin's own track

- [ ] CONTROLLERS > Notes port on; Live MIDI settings: Track on for Twisted Breaks Notes.
- [ ] Twisted Breaks track: MIDI From = Twisted Breaks Notes, arm, record a jam on the Mikro: each hit sounds once (not doubled).
- [ ] Play the clip back: the jam plays. Mikro KEYBOARD slices too.

## 0.25: AKAIZER

- [ ] FX grid: AKAIZER where STUTTER was (MIDI G#3). On an amen: Classic, 25 ms, +5 st; try 10 / 60 ms; Revised.
- [ ] Hold it as MOMENTARY, then set it LOCKED; automate PITCH.
- [ ] Right-click a pad > Render Akai: 200 %, 0 st, 25 ms Classic: a new pad with the stretched break.
- [ ] Pad Stretch > Akai Classic at a different Live tempo.
- [ ] An old set that used STUTTER opens fine (AKAIZER is off; JUNGLE ROLL still rolls).
- [ ] Listen to docs/audio/akaizer against the real Akaizer / an S950 if you have them.

## 0.24: macro editor

- [ ] FX > MACRO > EDIT (or right-click a macro): change DESTROY's CRUSH HARD, take DRIVE out, add LPF; HOLD TO HEAR and AMOUNT 0 / 100 %.
- [ ] Rename a macro: the key shows the new name. RESET / RESET ALL.
- [ ] SAVE a macro as .tbmacro, LOAD it into another slot.
- [ ] Save and reopen the set, and save / load a kit: the edits come back.

## 0.23: stretch

- [ ] A 170 BPM break on a pad (ONE), Live at 174 / 160 / 140: Stretch > Beats keeps it on the grid at its pitch; try the three gap fills at 140.
- [ ] Texture on a pad or vocal; Akai Classic vs Revised on an amen at 20-30 ms cycle: the 90s metallic stretch.
- [ ] The menu shows the detected BPM; Double / Halve / Type it in fix a wrong guess; Detect goes back.
- [ ] Sequencer slice steps and slice sequences stay in time with Stretch on; PITCH changes only pitch.
- [ ] A 0.21 set with FIT on still plays (as Repitch).

## 0.22.1: record your jamming, stop means stop

- [ ] VST3: new MIDI track, MIDI From = Twisted Breaks track / Twisted Breaks, arm, record while playing pads (mouse, Mikro, Push): notes land in the clip in time.
- [ ] Route that track's MIDI To the Twisted Breaks track (disarm the recorder) and play the clip: the jam plays back. Mikro KEYBOARD slices too (channel 2).
- [ ] AU: CONTROLLERS > "Twisted Breaks Notes" port on; Live Settings > MIDI: Track on for Twisted Breaks Notes; record on a MIDI track with that input.
- [ ] Start Live, latch a SWAP loop, start a slice sequence and a SEQ pad, stop Live: all stop at once.
- [ ] With Live stopped, hold a loop pad: plays while held, stops on release; nothing keeps running.

## 0.22: export and song

- [ ] PRESETS > EXPORT > The whole kit > 4 bars: RENDER % then DRAG WAV; drag it onto an audio track: 4 bars, in time, sounds like the kit.
- [ ] EXPORT > Pad 01 alone: only that pad. Live keeps playing during the render.
- [ ] EXPORT > MIDI > Every pad > 2 bars; drag onto a MIDI track with Twisted Breaks (or a Drum Rack): the pattern plays.
- [ ] SNAPSHOTS: store 3 snapshots, + + + in SONG, set lengths, SONG ON, play from bar 1: each switches on its bar line; jump around: the right step comes in.
- [ ] Automate the Song parameter on/off; save and reopen: the chain is still there.

## 0.21: TWIST, layers, fit

- [ ] TWIST (above the pads) a few times on a break: the sound changes each time, around your settings; UNDO walks back.
- [ ] "..." : lock PITCH and set 25 %: TWIST leaves the pitch, changes are smaller. MIDI-map "Twist" and fire it from a controller.
- [ ] Mikro EVENTS twists the selected pad, SHIFT + EVENTS undoes it.
- [ ] Right-click a pad > Layers > Add a layer (two different snares): hits alternate (RR2 on the tile); Velocity: soft vs hard; Random.
- [ ] Right-click > Fit to Live's tempo on a one-shot break, change Live's tempo: it plays faster / slower; the CRT shows the break's BPM.
- [ ] Save, reopen and save a kit preset: layers and FIT come back.

## 0.20: presets

- [ ] PRESETS (top row): three factory kits appear; click one to hear it, double-click to load; the kit name shows in the header.
- [ ] Save As > Kit with a name, tags and a note; it appears in the list; change a knob: the header shows *; SAVE clears it.
- [ ] Move or rename the original sample files, open a new Live set, load the kit: every pad plays (samples come from the preset).
- [ ] Save As > Pad, then load it onto another pad (right-click > Load onto); Save As > Sequences, clear the steps, load it back.
- [ ] Load a kit while Live plays: it lands on the next bar, no dropout.
- [ ] UNDO after a kit load puts the old pads back.
- [ ] Search, KITS / PADS / SEQS / FAV, the star, A-Z / NEW, Rename, Delete (Trash), Finder, Import, drop a preset file on the view.
- [ ] COLLECT SAMPLES: the pads now point into ~/Music/Twisted Breaks/Library/Audio; save and reopen the set.
- [ ] Mikro: BROWSE, pads 1-4, encoder, push to load, SHIFT + push to listen, BROWSE to leave.
- [ ] Push: Browse, encoder 1, Select, Shift + Select.

## 0.19: copy, paste, swap, undo

- [ ] Right-click a loaded, sliced pad with steps > Copy; right-click an empty pad > Paste: same sample, slices, settings and steps, its own MIDI note.
- [ ] Paste special > Steps only / Sound only / Settings only paste just that part.
- [ ] Alt-drag a pad onto another copies it; Shift-drag swaps them; no sound plays.
- [ ] Cmd-C / Cmd-V on the selected pad (click the plugin window first).
- [ ] UNDO / REDO keys: step back through paste, swap, clear, a sample drop, a marker move and some step taps; each burst is one step.
- [ ] Mikro: hold DUPLICATE, hit pad 1 then pad 3 (copy); SHIFT + DUPLICATE swaps; SHIFT + ERASE + pad clears; SHIFT + pad 1 / 2 = undo / redo; SHIFT + pad 11 / 12 = copy / paste the selected pad.
- [ ] Push: hold Duplicate, hit two pads; Undo / Shift + Undo.
- [ ] Reopen the set: UNDO is dark (fresh history), everything else as saved.

## 0.18: whole-pad steps and slice steps
- [ ] Mikro: PAD MODE, hit a looping break pad; STEP: tap steps -> the break plays out like the plugin's own steps (until choked)
- [ ] KEYBOARD: pick a slice; STEP: tap -> slice steps (in colour); PAD MODE pick again -> whole-pad steps (orange)
- [ ] In STEP, hold PAD MODE, hit another pad, let go: you're on that pad's lane
- [ ] An unsliced pad: KEYBOARD shows NO SLICES
- [ ] SEQUENCER view: WHOLE on for a pad -> its slice steps play the whole pad

## 0.17: transport, host-locked sequencer, slice by slice
- [ ] Sequencer: plays with Live's play, stops with Live's stop; no RUN key in the SEQUENCER view
- [ ] SEQ pads / SLICE SEQ still play when hit with Live stopped
- [ ] Mikro mode on: Live › Settings › MIDI: Remote on for "Twisted Breaks Remote"; ⌘M, map Play / Stop / Record to the Mikro's PLAY / STOP / REC
- [ ] Mikro PLAY / STOP / REC run Live; PLAY lights while Live plays, REC while recording; Push 3 Play / Record (User mode) work too
- [ ] KEYBOARD: hit a slice (screen S07), STEP: tap steps to place it; pick another, place it; same slice again = off; steps in slice colours
- [ ] In STEP, hold KEYBOARD, hit a slice, let go: back on STEP with the new slice; SHIFT + KEYBOARD: whole pad again
- [ ] Arm the pad's lane, Live playing: finger-drum slices on KEYBOARD, they record with their slice numbers

## Mikro MK3 mode (0.16)
- [ ] NI's Maschine app closed. LIGHTS > Mikro MK3 mode: the pads light, the screen shows PADS A and the pad
- [ ] PAD MODE: pads play with velocity (Mikro pad 1, bottom left = plugin 01), pressure works (pad's Pressure setting), GROUP flips to B
- [ ] STEP: tap steps on / off, hold one + turn the encoder (push = next field), > for 17-32, playhead moves
- [ ] KEYBOARD: a sliced break plays slice by slice; PERFORM: pads switch the FX; SCENE: SHIFT + pad stores, pad recalls
- [ ] Encoder pages: PLUG-IN / SWING / VOLUME, push = next setting, the screen shows name + value
- [ ] Strip = crossfader (SHIFT: macro amount); NOTE REPEAT + pad rolls in time (SHIFT + NOTE REPEAT: rate)
- [ ] VARIATION = MUTATE, TAP = FILL, SAMPLING = RESAMPLE, MUTE / SOLO / DUPLICATE / ERASE + pad
- [ ] Unplug mid-roll: the roll stops; plug back in: lights and screen come back
- [ ] Two Twisted Breaks: clicking a window hands the Mikro over

## Mikro MK3 USB test (0.15.1)
- [ ] Mikro plugged in, Maschine software closed. LIGHTS > Mikro MK3 test
- [ ] Pads light in 16 colours, buttons dim, strip lit; screen: TWISTED / BREAKS - MIKRO MK3 TEST
- [ ] Hit pads 1-16 (1 = bottom left): each goes white, screen + status strip say which and how hard
- [ ] Every button, the encoder (turn, push, touch) and the touch strip show up
- [ ] With NI's software running too: does it still connect? do NI's lights fight ours?
- [ ] LIGHTS > Copy Mikro test report, paste it to Claude

## Push 3 mode (0.15)
- [ ] Live's Settings › Link, Tempo & MIDI: Track OFF for "Ableton Push 3 (User Port)" input
- [ ] LIGHTS › Push 3 mode: the menu says Connected; press USER on Push: the grid lights (A left, B right, steps below)
- [ ] Hit pads on Push: they play with velocity, select in the plugin window (B pads flip the view)
- [ ] Tap steps: on / off; hold a step + turn encoders (VEL, PITCH ...); hold one + tap another for a range; DELETE + step
- [ ] Encoder pages (keys over the display): PAD / FX / SEQ / MIX; each move is one undo step in Live
- [ ] Touch strip moves the crossfader; scene column: MUTATE, FILL (hold), RESAMPLE, VIEW, LAYOUT
- [ ] Push Setup › Pads › Expression = Poly Aftertouch: press into a pad: FILTER / DRIVE / STUTTER / PITCH (PAD page encoder 8)
- [ ] Lights: playing white, sequences green, queued flashing, muted red, step playhead moving
- [ ] Press USER again: Push controls Live as normal; nothing stuck
- [ ] Two Twisted Breaks in the set: clicking a window hands Push to it
- [ ] If any key does nothing: note the status strip's LAST FROM PUSH and report it

## Scenes A / B (Phase 13)
- [ ] ✅ An older set opens exactly as before (scene A, crossfader in the middle, scene B empty)
- [ ] ✅ VIEW B, load a different break and pattern onto B pads; play both: crossfader A <-> B blends them, CURVE SHARP cuts at the edges, CUT A / CUT B while held
- [ ] ✅ The iRig plays and lights the scene on view; map a button to `Scene View` and a fader / knob to `Crossfader`
- [ ] ✅ FX / macros on a group hit whichever scene's pads are routed there, smoothly across a fade
- [ ] ✅ Snapshots switch both scenes at once, leaving the crossfader where it is; save / reopen keeps scene B
- [ ] ✅ CPU with both scenes playing busy patterns

## RESAMPLE + CHARACTER (Phase 12)
- [ ] ✅ RESAMPLE key: pattern + FX playing, select an empty pad, REC (LENGTH 2 BAR, START BAR): it waits for the bar, records 2 bars, then the pad has it as a synced, sliced loop that plays back in time
- [ ] ✅ SOURCE GRP B: only group B's sound is in the take
- [ ] ✅ The WAV is in Music / Twisted Breaks / Resamples; save the set, reopen: the pad still has it
- [ ] ✅ MIDI-map `Resample` to a pad and REC from the controller; CANCEL while waiting
- [ ] ✅ CHARACTER on a kick / snare: SP-1200 and LO-FI gritty, MPC60 warm, S950 dark; pitch the pad down 12: more grit; ALL PADS
- [ ] ✅ CPU still fine with every pad on SP-1200

## Performance macros (Phase 11)
- [ ] ✅ MACRO tab, a break playing: hold each macro key in turn: each sounds right and stops cleanly when let go (DELAY / REVERB tails ring)
- [ ] ✅ Ride AMOUNT while holding DESTROY / DARKEN: mild to hard, no zipper noise
- [ ] ✅ TAPE STOP and REWIND land in time with the grid; JUNGLE ROLL rolls at 1/16 (HARD: 1/32)
- [ ] ✅ Your own LPF on at a setting, hold DARKEN, let go: the LPF is back where you left it
- [ ] ✅ GROUP B with drums on A and a reese on B: only the reese is hit
- [ ] ✅ iRig pads mapped to E4-B4 (or MIDI-map the `Macro ...` parameters): hold / LATCH both work

## MUTATE (Phase 10)
- [ ] ✅ A full kit pattern, MUTATE tab, AMOUNT 35 %, AT BAR: press GO while Live plays: it changes exactly on the next bar, the beat stays put, it still grooves
- [ ] ✅ GO a few times, UNDO (and UNDO again), RESET: back to your pattern exactly
- [ ] ✅ AUTO 4 BAR for a couple of minutes, with and without a 4-bar loop brace: a new variation every 4 bars, on the bar line, no glitches
- [ ] ✅ SCOPE PAD on a SLICE SEQ break pad: only that break is rearranged (beats keep their slices)
- [ ] ✅ Switch targets off one at a time (e.g. only RAT): only that changes; DRIFT on: variations wander further each press
- [ ] ✅ MIDI-map `Mutate` to an iRig pad: one press = one variation

## Slice sequencer (0.7.1)
- [ ] ✅ Break on a pad, AUTO SLICE, SEQ tab, SLICE SEQ on. Live playing: hit the pad: the break plays exactly as recorded, in time; hit again: stops
- [ ] ✅ Change Live's tempo (e.g. 160 -> 175): the break follows the grid (re-sequenced, slices at their own speed)
- [ ] ✅ Drag steps up / down to pick slices; REST / TIE; REV, PITCH, RAT, CHANCE, MICRO on a step: all audible and in time
- [ ] ✅ SHUFFLE / RANDOM / REVERSE / HALF / ROTATE / RESET; hold FILL for rolls; SWING 50 %
- [ ] ✅ SPREAD the break, STEP REC, tap pads: their slices fill the steps in order
- [ ] ✅ Two SLICE SEQ pads in one SWAP group: hand-off lands on the next step
- [ ] ✅ Automate `Pad 01 Slice Seq Slot` between slots 1 and 2: the switch lands at the pattern start
- [ ] ✅ Save, close, reopen: slots, steps and settings come back

## MIDI pad triggering
- [ ] ✅ Notes **C1–D#2 (36–51)** fire pads 1–16 (0.16.1: bottom-left = pad 1, top-right = pad 16, like a Drum Rack); pad lamp flashes; MIDI lamp blinks; LAST NOTE shows the note
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
- [ ] ✅ Phase 6: FX on/off per group automatable (`Group A FX LPF`) and MIDI-mappable (Cmd-M a key's parameter)

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
- [ ] ✅ Phase 7: **sequencer sync** — sequenced pad vs Drum Rack on the same steps: no flam when soloing both; still tight after relocating and after tempo changes

## Project save / reload
- [ ] ✅ Set Master Level −7.5 dB, limiter OFF, save, close Live, reopen: both restored
- [ ] ✅ Duplicate the track: the copy has identical settings
- [ ] ✅ Undo/redo a Master Level change works
- [ ] ✅ Save the set with the VST3 and with the AU version on separate tracks; both reload
- [ ] ✅ Load samples on several pads, change pitch / start / end / reverse, save, reopen: all restored
- [ ] ✅ Patterns, FX states, snapshots and the bank all restored

## Sample recovery
- [ ] ✅ Rename a sample on disk, reopen the set: pad shows **MISSING** (red), set opens, other pads fine
- [ ] ✅ Load the file again onto that pad (drag or LOAD): plays again; saving fixes the path
- [ ] ✅ Phase 8: relocate missing files in one go (SNAPSHOTS view, FIND MISSING)
- [ ] Collect All and Save: set reopens on another machine with samples

## Snapshot switching (Phase 8)
- [ ] ✅ STORE two different set-ups (samples, mixer, FX, patterns) in slots 1 and 2
- [ ] ✅ Switch snapshots Immediate / Next Beat / Next Bar while playing — change lands exactly on the boundary
- [ ] ✅ No dropouts, clicks or CPU spike while switching every beat for 2 minutes
- [ ] ✅ Program Change 1-8 from your controller recalls snapshots; automate `Snapshot` in the arrangement
- [ ] ✅ Break bank: copy two sliced breaks in, SPREAD pads + ALL SLICE PADS FOLLOW, switch Break Bank on the beat: same playing, new break
- [ ] ✅ Save, close, reopen: snapshots, bank and the active snapshot come back

## Panic
- [ ] ✅ PANIC (red STOP) button: all sound stops within ~5 ms, no click, button flashes
- [ ] ✅ MIDI CC 120 (All Sound Off) acts as panic
- [ ] ✅ Phase 6: PANIC also kills stutter, beat repeat, delay / reverb tails and switches the FX off
- [ ] Phase 7: panic also stops sequencer-triggered voices

## Latency
- [ ] ✅ Live's device delay compensation shows **0 samples** for the plugin (hover the device title bar with Delay Compensation on)
- [ ] ✅ Recorded MIDI → audio (resample TwistedBreaks onto an audio track): hits line up with the grid

## CPU use
- [ ] ✅ Idle, editor closed: Live CPU meter ≈ unchanged vs empty track
- [ ] ✅ Editor open, transport running: UI stays smooth (lamps/meter ~30 fps), no audio dropouts at 128-sample buffer
- [ ] ✅ 8 instances on 8 tracks with editors closed: no dropouts at 256 samples
- [ ] ✅ Phase 6: all 16 FX on all 4 groups at once — note CPU %, no dropouts at 256
- [ ] ✅ Freeze and flatten a track: frozen audio matches live playback

## Stress
- [ ] ✅ Change Live's sample rate 44.1 → 48 → 96 kHz with the plugin playing
- [ ] ✅ Change buffer size 64 → 2048 while playing
- [ ] ✅ Export audio (offline render faster than real-time): timing and sound identical to real-time
