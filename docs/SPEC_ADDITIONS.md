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
