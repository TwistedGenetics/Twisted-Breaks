# Spec additions

Features added to the original brief (`TWISTED BREAKS` spec) after the
project started, written as build prompts in the same style. Each one
names where it lands in the phase plan.

---

## A1. SWAP groups — chop and change between breaks at slice points

**Status: built in 0.4.1** (between Phase 4 and Phase 5).

Prompt:

> Let a sliced break play through its slices from ONE pad when it's
> triggered. When I trigger another pad that also holds a sliced break,
> it takes over from the playing break at the next slice point
> (quantised), from the SAME point in the bar: e.g. the Amen is playing on
> pad 1, it gets to the snare, I hit pad 2 and pad 2 takes over from that
> same point and plays from ITS snare.

Behaviour as built:

* Per pad: **SWAP group** (Off / 1–4) and **SWAP AT** (Slice / 1/16 / Beat /
  Bar), both automatable and saved with the set (`pad01_swap`,
  `pad01_swapq`). On the pad editor's **SWAP** page.
* Pads in the same group take turns: one plays at a time and it is
  **latched** (note-off ignored), so you can let go of the pad.
* Hitting another pad in the group **queues** a hand-off (the pad flashes).
  It lands sample-accurately at the playing break's **next slice marker**
  (or the next 1/16, beat or bar of Live's grid if SWAP AT says so).
  Unsliced breaks use their 1/16 grid.
* The incoming break continues from the **same musical point**: a synced
  loop takes its own position on Live's grid at that moment (otherwise, with
  Live stopped, the same fraction of the way through). It then snaps to its
  own nearest slice marker (within a 1/32 note), so it starts ON its hit
  instead of a few ms into it, and keeps that tiny offset so it never
  drifts off the grid.
* The outgoing break fades over 1 ms right on its slice point (just before
  its next hit), so nothing of that hit leaks through.
* Hitting the playing pad again stops it at the next point; hitting it while
  a swap is queued cancels the swap. PANIC stops everything.
* Real-time safe: plain data on the audio thread, no allocation.

Exit tests (automated): queued not immediate; lands within 2 samples of
the marker; outgoing faded in 1 ms; incoming starts on its own marker; stays
on the grid for 4+ s; AT BAR lands on the bar line; cancel; stop; works with
Live stopped; saved with the set.

---

## A3. Controller pad lights (iRig Pads)

**Status: built in 0.5.1.**

Prompt:

> Light the pads of my iRig Pads to show the currently playing pad.

As built: `Core/PadLights`. iRig Pads lights each pad from a pair of CCs
on channel 1 (red / green, 0-127). The plugin sends them straight to the
chosen MIDI output from a 30 Hz timer thread (not the audio thread, not
Ableton's routing): green following the pad's level while it plays, dim
green when loaded, flashing red for a queued SWAP. Hardware pads are
matched to plugin pads by note (default scene) or by LEARN. Only changes
are sent, plus a refresh every 4 s; replugging is picked up; all lights
go off when the plugin closes.

---

## A4. Fixed velocity per pad

**Status: built in 0.5.2.**

Prompt:

> I would like a fixed velocity button for each pad.

As built: per-pad `pad01_fixvel` (automatable), the **FIX** key on the PLAY
page next to VEL. On: every hit (MIDI, mouse, and later the sequencer)
plays at full level regardless of velocity, like an MPC's FULL LEVEL; VEL
dims while it's on.

---

## A5. Slice sequencer per pad — rearrange a break's slices

**Status: built in 0.7.1.** As built: per-pad `pad01_sseq` (on) and
`pad01_sslot` (slot 1-8, switches at the pattern start), steps / slots in
`Core/SliceSeqData` (lock-free, saved with the set), the pad editor's
**SEQ** page (SLICE SEQ, SLOT, LEN, RATE 1/8 / 1/16 / 1/32 / 1/16T, SWING)
and the slice-sequence screen (32 steps, per-step SLICE / REST / TIE, REV,
PITCH, VEL, LEN gate, RATCHET, CHANCE, MICRO; FILL; SHUFFLE, RANDOM,
REVERSE, HALF, ROTATE, RESET; STEP REC from SPREAD pads). An unwritten
slot plays the DEFAULT pattern (each slice on its nearest step with
micro-timing) and replays the break sample-exactly. Hitting the pad
starts / stops it (latched); the main sequencer starts it; SWAP groups
hand off on the next step. Not built from the list below: undo / redo in
the editor and drag-across-steps copy (later).

(Original prompt and plan follow.)

Prompt:

> Give every pad its own sequencer for when it holds a sliced break, so I
> can rearrange the slices into a different order and have the pad play
> that order (see SLOOP for the M-VAVE FM-1 for ideas).

What to build:

* **Per-pad slice sequence**, on the pad editor's new **SEQ** page and the
  CRT: a row of 16 steps (32 with a second page), length 1–32, each step a
  1/16 (rate 1/8, 1/16, 1/32, 1/16T selectable). By default step N plays
  slice N, so an untouched sequence plays the break as recorded.
* **Each step** chooses:
  * which **slice** (1–16), or **REST**, or **TIE** (let the previous slice
    keep playing),
  * **REV** (reverse that slice), **PITCH** (±12 st), **VELOCITY** (or the
    pad's FIXED VEL), **LENGTH** (cut short: 1/4 … full),
  * **RATCHET** ×1/×2/×3/×4 (retrigger inside the step: rolls and stutters),
  * **CHANCE** (probability 0–100 %),
  * **MICRO-TIMING** ±½ step in 1/64-step increments (SLOOP-style), so
    rearranged hits keep the original groove,
  * plus a pattern-wide **SWING**.
* **Edit by playing**: hold the pad's SEQ and tap the pads of a SPREAD
  break to write slices into steps, or click a step then a slice on the CRT.
  Drag across steps to copy a slice. Undo / redo within the editor.
* **One-press variations** (the jungle bit):
  * **SHUFFLE** (random reorder, keeping kicks on downbeats when the
    slices came from AUTO SLICE),
  * **RANDOM** within the current slices,
  * **REVERSE ORDER**, **HALF** (repeat the first half), **ROTATE** ±1 step,
  * **FILL** (ratchets / rolls on the last beat, momentary while held),
  * **RESET** to 1, 2, 3 … (the original break),
  * 8 **pattern slots** per pad, switchable on the next bar.
* **Playback**: the pad becomes a "sequenced slice" pad: triggering it
  starts its sequence (latched, synced to Live's bar grid like a LOOP);
  works with SWAP groups (hand-off lands on a step), choke, groups / FX,
  and the break bank (A2) so the same pattern can play another break.
* **Real-time safety**: steps are plain fixed-size data handed to the
  audio thread like slice tables; sample-accurate step timing from PPQ.
* **State**: every sequence, slot and setting saved with the set; the
  sequence position never drifts from Live's grid (tested over minutes,
  with loop-brace jumps and tempo changes).

Exit tests (automated): identity sequence plays the break sample-exactly;
rearranged steps start each slice exactly on its step; REST / TIE / REV /
RATCHET / CHANCE (seeded) / micro-timing / swing timing measured to the
sample; slot switch lands on the bar; state round trip; zero allocations.

---

## A2. Break bank swap — change the break under all 16 pads

**Status: built in 0.8.0.** As built: `Core/BreakBank` (8 slots: drop a
file, or copy the selected pad's break + markers; AUTO SLICE runs on
load), per-pad `pad01_bank` (FOLLOW BANK, on the SWAP page) and the
automatable `bank_active` (1-8) + `bank_at` (Now / Next Beat / Next Bar).
The engine keeps each pad's own sample and plays the active bank break for
following pads; a bank change is an event in the block's sorted event list,
so it lands on the exact sample of the beat / bar. Slice sequences carry on
across the change (same pattern, new break). Saved with the set.

(Original prompt and plan follow.)

Prompt:

> Keep a bank of up to 8 sliced breaks. Every pad plays "slice N of the
> active break". Changing the active break (knob, MIDI note or automation)
> swaps the source under all 16 pads at once, quantised to the next beat or
> bar, so my fingers and the sequencer pattern stay the same and only the
> break changes. Automatable, so break changes can be written into the
> arrangement.

Notes for the build:

* Bank slots hold a sample path + slice table each (shared decoding via
  `SampleManager`, so a bank costs one decode per break).
* Pads gain a "bank" source option: slice index into the active slot
  instead of their own sample.
* Active slot is an automatable choice parameter (`bank_active`); switching
  uses the same next-beat / next-bar landing as snapshots and SWAP AT.
* Saved with the set; missing files in a slot show MISSING like pads.

---

## A6. Sequence pads — launch a pad's sequence by hitting it, and SWAP between sequences

**Status: built in 0.14.0.** Decided with Jamie: a SEQ pad plays its own
lane, so its sequence triggers only its own sound (no multi-lane "pattern
pads"). As built: MODE gains SEQ ONE / SEQ LOOP (appended); per-pad
`launch` (Now / 1/16 / Beat / Bar), `seqlen` (0 = global, 1-32) and
`swapfrom` (Same Step / Step 1); launched lanes run on the slice
sequencer's clock (Live's, or free when stopped), share the main
sequencer's step code, and are cut / restarted / handed off through
generation-checked events in the block's event list. SWAP AT SLICE means
the next STEP for a sequence. Live starting or stopping ends them. Not
built: the optional slice sequencer ONCE mode.

**Changed in 0.14.1 (Jamie: "it should follow Live's playhead"):** a
launched sequence is phase-locked to Live's grid. It always plays the
step under the playhead (song position mod its LEN), as if it had been
running all along, so it lines up with the bar and with the sequencer's
playhead column. LAUNCH only decides *when* it comes in. SEQ ONE plays one
full pass (LEN steps) from its launch; conditions count passes on the
grid, with FIRST = the first time round after the launch. A loop-brace
wrap or relocate takes it to the step under the new playhead position.
SWAP FROM SAME STEP = the step under the playhead; STEP 1 still starts
the pattern from the top at the hand-off. With Live stopped, a hit with
nothing else running starts the free clock, so step 1 plays at the hit.

Prompt:

> The sequencer is great but I want to be able to trigger a sequence
> (either one-off or looped) by hitting the pad with the sequence in. I
> guess this will be set in the play mode selector. I also want to swap
> between pads with different sequences and have them cut each other's
> sequence in the same way as slices do (slice, 1/16, beat / bar).

What to build:

* **Two new play modes** on the PLAY page's **MODE** selector (appended to
  the choice list, so saved sets and automation keep their meaning):
  * **SEQ ONE**: a hit plays one full pass of the pad's sequencer lane
    (in time with the playhead, see 0.14.1 above), then stops.
  * **SEQ LOOP**: a hit starts the lane and it loops until it is stopped.
  * In both modes the lane no longer runs on its own with Live's
    transport: it plays only when launched. Lanes of pads in the other
    modes behave exactly as now.
* **What a launched sequence plays**: the pad's own lane in the main
  sequencer, with every step feature (velocity, CHANCE, RATCHET, PITCH,
  SLICE, REV, MICRO, FX, conditions). A sliced break on a SEQ pad can
  therefore play a whole chopped pattern of its own slices from one hit.
  The hit's velocity scales the steps' velocities (following the pad's VEL
  / FIX settings).
* **LAUNCH** (new per-pad setting on the PLAY page, shown in SEQ modes,
  automatable, saved): when a hit starts the sequence:
  **NOW, 1/16, BEAT, BAR** (default BEAT). It comes in exactly on that
  boundary sample, even mid-block, on the step under Live's playhead. With Live stopped it starts at once on
  the free clock, like the slice sequencer.
* **SEQ LEN** (per pad, saved): **GLOBAL** (the sequencer's LEN) or 1-32
  steps, so a 1-bar fill and a 2-bar groove can sit on different pads.
  The STEP size stays global.
* **Hitting a running sequence pad again**:
  * SEQ LOOP: it stops at its next LAUNCH boundary (the tile flashes while
    it waits); hitting it again before then cancels the stop.
  * SEQ ONE: another full pass starts at the next LAUNCH boundary.
* **SWAP groups for sequences**: SEQ pads in the same SWAP group (1-4) take
  turns like breaks do. One sequence plays at a time; hitting another
  queues a hand-off (the incoming pad flashes) that lands on the playing
  sequence's **AT** setting:
  * **STEP**: the next step of the playing sequence (the sequence
    equivalent of SLICE),
  * **1/16, BEAT, BAR**: the next line of Live's grid.

  The outgoing sequence is cut there; the incoming one takes over on the
  same sample.
* **SWAP FROM** (per pad, saved): **SAME STEP** (default: the incoming
  sequence carries on from the same position in its own pattern, like a
  break hand-off continuing from the same point) or **STEP 1** (it starts
  its pattern from the top).
* **Mixed SWAP groups**: a SEQ pad and a break pad (or a slice-sequencer
  pad) in one SWAP group hand off to each other by the same rules.
* **Everything else keeps working**:
  * choke groups stop a running sequence;
  * MUTATE varies the lanes of SEQ pads too (a variation lands while the
    sequence plays);
  * snapshots store the modes, LAUNCH, SEQ LEN and SWAP FROM, and stop
    sequences whose pad is no longer in a SEQ mode;
  * scenes A / B both have sequence pads;
  * conditions count passes from the moment the sequence was launched
    (FIRST = its first time round);
  * PANIC and Live stopping end every launched sequence.
* **Optional, for consistency**: give the slice sequencer the same choice,
  **LOOP** (as now) or **ONCE**, so a slice pattern can also be a one-shot.
* **On screen**:
  * pad tiles tag the mode (`SEQ1` / `SEQL`), light while the sequence
    runs and flash while a launch, stop or hand-off is waiting;
  * the sequencer view shows each launched lane's playhead (on the same
    column as Live's) and dims lanes of SEQ pads that aren't
    running;
  * the iRig lights follow the same states (green running, red flashing
    queued).
* **Real-time safety**: launches, stops and hand-offs are events in the
  block's sorted event list (like SWAP and the slice sequencer), with
  per-pad run state (anchor step, slot, generation) as plain data. No
  allocation, no locks.

Exit tests (automated):
* a hit on a SEQ ONE pad plays its lane once from the LAUNCH boundary
  sample (NOW / 1/16 / BEAT / BAR, mid-block) and then stops;
* SEQ LOOP keeps going across many passes without drift;
* SEQ pads don't play when not launched;
* a hit on a running pad stops it at the boundary, and a second hit
  cancels the stop;
* SEQ LEN per pad;
* SWAP hand-off between two SEQ pads at STEP, 1/16, BEAT and BAR lands on
  the exact sample, with no doubled or missing step at the seam;
* SAME STEP vs STEP 1;
* a mixed SWAP group (sequence ↔ break);
* choke, Live stopped (free clock), conditions counted from the launch,
  velocity scaling;
* the state round trip, and an old set opening unchanged;
* zero allocations on the audio thread.

## A7. Push 3 mode — play and program Twisted Breaks from Push 3

**Status: built in 0.15.0.** Push 3 keeps Push 2's MIDI protocol (Ableton's
published Push 2 MIDI and Display Interface Manual); the User port, the
mode echo (sysex 0x0A) and the palette read (0x04) come from it. As built,
with these changes from the prompt below: RESAMPLE is on the scene column
(Push 2's spec has no Capture key to check against) instead of a long
press on Capture; the pad Pressure target is a new per-pad parameter
(`padNN_press`, kV17: Filter / Drive / Stutter / Pitch / Off), set from
the PAD encoder page or LIGHTS › Pad .. pressure; the PAD page has
PRESSURE in place of VEL SENS; the plugin window's status strip is the
"Push panel"; "hold one step and tap another" copies the first across the
range. Push's screen stays Live's.

Prompt:

> I want to play and program Twisted Breaks from my Ableton Push 3
> (plugged into the Mac, running Live 12) without looking at the screen.
> When I press Push's **User** button, Push becomes a dedicated Twisted
> Breaks controller. When I press it again, it's back to normal Live
> control.

What to build:

* **Connection**: PUSH 3 in the LIGHTS menu; the plugin talks to Push's
  User port directly (lights out; pads, encoders, buttons, touch strip
  in), only while Push is in User mode, handing everything back cleanly.
  Several instances don't fight: the one touched last gets Push. Check
  Push 3's real User-mode map first. Live's own setup: Track off for the
  User port on the Twisted Breaks track (in the manual).
* **Grid**: top 4 rows = the 32 pads (A left 4x4, B right 4x4, like the
  matrix), lit by state (dim loaded, white playing, green sequence
  running, flashing queued, dim red muted); hit = play with velocity +
  select. Bottom 4 rows = the selected pad's 32 steps: on / off and
  velocity, moving playhead, dark past LEN; tap toggles, hold + encoder
  edits VEL / CHANCE / RATCHET / PITCH / SLICE / MICRO, hold + tap = range.
* **Pressure**: per-pad pressure to a per-pad target (filter, drive,
  stutter, pitch; default filter).
* **Encoders**: 8 encoders, 4 pages (PAD, FX, SEQ, MIX), relative, SHIFT =
  fine, every move automatable with gestures; touching shows the
  parameter in the plugin window.
* **Touch strip**: the A/B crossfader.
* **Buttons**: Play / Record stay Live's; Mute / Solo / Duplicate / Delete
  / Shift + pad; Fixed Length cycles LEN; Layout / Note / Scale: lower half
  steps <-> big 4x4 pads; arrows: scene A/B; MUTATE (GO), FILL (hold);
  long press Capture: RESAMPLE.
* **Display**: a small Push panel in the plugin window (page, parameter
  names, selected pad).
* **When it stops**: leaving User mode, closing or unplugging releases
  everything and switches the lights off; coming back redraws at once.
  No Push = exactly as now.
* **Real-time safety**: Push input into the engine through lock-free
  queues; lights from the message thread at ~30 Hz, changes only; nothing
  new on the audio thread.

Exit tests (automated, fake Push port): pad notes play the right pad with
velocity and select it; step pads toggle, hold + encoder edits; encoders
per page with gestures, fine and relative wrap; touch strip; each button;
LED output matches state and only changes are sent; leaving User mode
releases everything; two instances don't fight; no allocations on the
audio thread; all earlier tests pass.

## A8. Maschine Mikro MK3 mode

**Status: built in 0.16.0.** Step 1 (0.15.1 / 0.15.2) proved the USB route
on Jamie's Mikro: it connected (64-byte input, 265-byte output reports),
all 39 buttons and the pads (velocity + pressure) read as the open-source
drivers describe, lights and screen accepted. NI's Maschine 3 app takes
the Mikro over when it opens, so Mikro mode wants it closed. Step 2 is
the full mode, as prompted below (built as written; KEYBOARD plays the
selected pad's 16 slices; NOTE REPEAT rolls are a new engine feature,
sample-accurate on Live's grid). Prompt:

> When I turn on Mikro MK3 mode, the Mikro becomes a dedicated Twisted
> Breaks controller. Its screen, pad colours and button lights all
> belong to the plugin. PAD MODE: the 16 pads of the scene on view
> (velocity, pressure, lit like the plugin; GROUP flips A/B). STEP: the
> selected pad's steps (< > for 17-32, hold + encoder edits, push picks
> the field). KEYBOARD: its slices. PERFORM: the 16 FX. Encoder + screen:
> PLUG-IN = PAD page, SWING = SEQ page, VOLUME = MIX page; push or
> SHIFT + turn = next parameter. Strip = crossfader (SHIFT: macro
> amount). MUTE / SOLO / DUPLICATE / ERASE / SELECT + pad; FIXED VEL;
> VARIATION = MUTATE; TAP = FILL; SAMPLING = RESAMPLE; NOTE REPEAT rolls
> at 1/16 (SHIFT: 1/8, 1/32); SCENE = snapshots 1-8; PLAY lit while Live
> plays (PLAY / REC / STOP stay Live's). One instance owns it (the one
> clicked last); replugging just works; nothing on the audio thread.

**Step 1 built in 0.15.1: the hardware test.** Jamie asked to "fully
integrate" his Maschine Mikro MK3. Chosen route: talk to it over USB HID
directly (vendor 0x17CC, product 0x1700), not NI's MIDI mode, so the plugin
owns the pad colours, every button light, the touch strip LEDs and the
128 x 32 screen. The protocol isn't documented by NI; it comes from two
open-source drivers that agree (r00tman/maschine-mikro-mk3-driver, MIT;
flokapi/pymikro). Risks: NI's background service may hold the device on
the Mac, and the protocol is unverified here. So step 1 is
LIGHTS > "Mikro MK3 test": open it (or say why not, with the IOKit code),
light pads / buttons / strip, write the screen, show every pad, button,
encoder and strip event on the Mikro, in the status strip and in a
copyable text report. Step 2 (the full Mikro mode, written up as a prompt
first) waits for that report.

## A9. Sequencer follows Live; Live's transport from controllers; slice by slice on the Mikro

**Status: built in 0.17.0.** Decided with Jamie: only the main sequencer is
tied to Live (SEQ pads and slice sequences still play with Live stopped);
transport through a MIDI-mapped virtual port.

Prompt:

> I want the sequencer start stop to only be linked to the host playing,
> so in sequencer mode play starts when the host starts and stops when the
> host stops. The play, record and stop should control Ableton. When using
> the Maschine Mikro in slice mode I want to be able to select a slice from
> the keyboard menu and input that in the step tab, then go back and choose
> a new slice and input that into the step so I can build up a new break
> manually this way.

As built:
* **Sequencer**: plays exactly while Live plays. The RUN key is gone; the
  `Sequencer Run` parameter still loads with sets and automation but is
  ignored. SEQ1 / SEQL pads and SLICE SEQ unchanged (they still run on the
  free clock with Live stopped).
* **Transport**: "Twisted Breaks Remote", a virtual MIDI output (macOS /
  Linux), created while Mikro or Push 3 mode is on. Mikro PLAY / STOP / REC
  and Push 3 Play / Record (User mode) send notes 94 / 93 / 95 on channel
  16 (on / off with the key). Live: Remote on for the port, MIDI-map
  Play / Stop / Record once. PLAY / Play lights while Live plays; REC /
  Record while Live records (the host's isRecording) or while held.
* **Slice by slice (Mikro)**: KEYBOARD hit = play + choose that slice
  (screen S07). STEP tap: empty step -> on with the chosen slice; a step
  with another slice -> takes the chosen one; the same slice -> off.
  Steps (and KEYBOARD pads) light in their slice's colour. Hold KEYBOARD
  from STEP for a quick pick (back to STEP on release after a hit or
  400 ms). SHIFT + KEYBOARD clears the chosen slice. Holding a step: the
  encoder edits SLICE first. Slices finger-drummed on KEYBOARD record into
  an armed lane with their slice numbers while Live plays (rolls don't).
  SWAP / SLICE SEQ pads: the screen says slices are ignored.

## A10. Whole-pad steps and slice steps

**Status: built in 0.18.0**, with the per-pad switch (Jamie: "go with").

Prompt:

> On the plugin, a step plays the whole break (looped or one-shot) until
> another pad in the same choke group cuts it. On the Mikro, my steps play
> the break as a short hit. I want to be able to sequence either single
> pad hits (the whole pad) or slices picked from KEYBOARD, and choose which
> pad I'm sequencing, from the Mikro.

As built:
* Two kinds of step in one lane: whole-pad (slice 0: plays like the
  plugin's sequencer always has) and slice (1-16).
* Mikro: the last pick decides. PAD MODE hit = WHOLE; KEYBOARD hit =
  that slice. Selecting another pad (Mikro or plugin window) resets to
  WHOLE. Tap rules: empty -> the choice; another kind / slice -> takes
  the choice, stays on; the same -> off. Hold PAD MODE in STEP: pick the
  pad (it plays) and come back to its lane. Whole-pad steps light orange,
  slice steps in their slice colour; holding a step shows WHOLE PAD /
  the slice. A pad with no slice markers: KEYBOARD dark, NO SLICES.
* Per pad `padNN_whole` (kV18, "Pad NN Whole Steps"; WHOLE key in the
  SEQUENCER view header for the selected lane): the engine ignores a
  step's slice for that pad, and the Mikro writes whole-pad steps.


## A11. Mikro step order, pad copy / paste, preset manager, and more

Shipped in stages: 0.18.1, 0.19, 0.20, 0.21, 0.22.

**1. Mikro step order (0.18.1).** In STEP mode the Mikro's steps follow the
pad numbers: step 1 = pad 1 (bottom left), along the row and up, step 16 top
right; ‹ › page 17–32 the same way. The FX grid keeps the plugin's on-screen
order.

**2. Copy / paste between pads, undo (0.19).** Copy takes the sample, slice
markers, sensitivity and slice count, every per-pad parameter, the main lane
and the slice sequence (not the MIDI note). Paste = everything; Paste special
= Sound only / Steps only / Settings only. Also Swap and Clear. Plugin:
right-click a pad, Cmd-C / Cmd-V, Alt-drag copies, Shift-drag swaps. Mikro:
hold DUPLICATE, hit source then target (SHIFT + DUPLICATE swaps); Push 3 the
same with Duplicate. Sample buffers are shared, not reloaded. Undo / redo
(Cmd-Z / Cmd-Shift-Z, 50 steps) covers paste, swap, clear, preset loads,
slice edits and step edits (parameter moves are Live's undo).

**3. Preset manager (0.20).** Kit (all pads, sequences, snapshots, bank, FX,
mixer; not controller settings or MIDI map), Pad (what Copy takes) and
Sequence (patterns only) presets: `.tbkit` / `.tbpad` / `.tbseq`, a zip of
`preset.xml` + `audio/` FLAC copies (24-bit, original rate, de-duplicated by
hash). Embedded audio unpacks to `~/Music/Twisted Breaks/Library/Audio`.
Browser view PRESETS: folders under `~/Music/Twisted Breaks/Presets`, search,
tags, favourites, sort, preview, load, Save / Save As (name, tags, note),
rename, delete to Trash, show in Finder, import, drag-in, prev / next, kit
name with a modified mark. Collect Samples copies every used sample into the
Library and repoints the pads. Mikro SHIFT + BROWSER and Push 3 browse with
the encoder. Kit loads while Live plays land on the next bar; old sounds keep
playing until the new ones are ready. Versioned format; corrupt or newer files
are refused with a message and change nothing; missing audio loads as MISSING.
Three small factory kits from the repo's own test audio.

**4. TWIST, round robin, break tempo (0.21).** TWIST randomises the
selected pad's sound settings (pitch, start, pan, reverse, character, slice,
gain: the pad has no filter or drive of its own) within an amount, with a
lock per setting; one undo step. Round robin / velocity layers:
up to 4 extra samples per pad, played in turn or by velocity. Break tempo:
the detected BPM of each break shown on the pad page, and a FIT setting that
plays a one-shot / slice break at Live's tempo.

**5. Export and song mode (0.22).** Export the selected pad's pattern or the
whole kit for N bars to WAV (rendered offline at Live's tempo), and drag a
MIDI clip of the main sequence into Live. Song mode chains snapshots (slot,
bars) and steps through them while Live plays.

## A12. Record your jamming, stop means stop, stretch modes, macro editor, AKAIZER

Stages: 0.22.1 (1-2), 0.23 (3), 0.24 (4), 0.25 (5).

1. **MIDI out of what you play.** Pad hits from the mouse, Push, the Mikro and
   note-repeat rolls go out on channel 1 as the pads' notes (velocity, note
   off); Mikro KEYBOARD slices on channel 2, notes 36-51. Plugin MIDI out
   (VST3) and a virtual port "Twisted Breaks Notes" (AU / any). Incoming MIDI
   isn't echoed; sequencer steps only with "Include sequencer steps". Channel
   2 notes 36-51 coming in play slices of the selected pad.
2. **Stop means stop.** Live stopping stops every break (5 ms fades; FX tails
   ring). While stopped, pads play while held only.
3. **STRETCH per pad**: Off / Repitch / Beats / Texture / Akai cyclic; better
   tempo detection, ORIGINAL BPM, pitch without speed.
4. **Macro editor**: each macro's recipe editable (every FX setting, MILD /
   HARD), add / remove FX, rename, reset; saved in the set, kits, .tbmacro.
5. **AKAIZER** (replaces STUTTER in the grid): cyclic time-stretch / pitch shift
   after the Akai S-series (Classic / Revised splices, CYCLE, PITCH, STRETCH,
   MIX); per-pad Akai stretch mode and RENDER AKAI.

**0.25.1**: the Notes port can be the plugin track's own MIDI input (echo guard).

## A13a. Controllers found on their own (0.25.2)

> Can the plugin detect the Maschine Mikro or Push when plugged in without me
> having to select it? Then the manual option can go and LIGHTS can get a
> better name.

* Push 3 and the Mikro MK3 are always looked for: Push via its User MIDI port
  (taken only when USER is pressed, so Live mode is untouched), the Mikro over
  USB HID every 2 s (left alone while NI's Maschine app holds it).
* The Push 3 mode / Mikro MK3 mode switches are gone; old sets' saved "off"
  is ignored. The last-clicked window still owns each controller.
* LIGHTS is renamed **CONTROLLERS**: a status line for Push and the Mikro,
  pad pressure, the Mikro test (pauses the Mikro while it runs), the iRig pad
  lights and MIDI OUT.

## A13. STRETCH in the pad settings (0.26)

> I want the time stretch to be in the pad settings menu rather than a right
> click on the pad; maybe it could replace the loop switch.

* A STRETCH selector (OFF / PITCH / BEAT / TEX / AKAI / AK.R) on the PLAY page
  in SYNC's place, for one-shots, slices and loops; the break's own tempo above
  it (amber when typed in). Click it or right-click the selector: x2, /2, type
  in, work out again, BEATS gaps, grain / cycle. Double-click: OFF.
* Loops: OFF = free running (old SYNC off), PITCH = the old SYNC (identical),
  BEAT / TEX / AKAI = locked to the bar grid with the pitch kept. A typed-in
  tempo sets a loop's length in beats.
* No new parameters: `stretch`, `sync`, `fit` all kept; the selector writes
  them together as one undo step; old sets read the same.
* The right-click Stretch submenu is gone.
