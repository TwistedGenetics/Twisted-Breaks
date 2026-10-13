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

**Status: planned for Phase 8** (shares the quantised switching of
snapshots).

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
