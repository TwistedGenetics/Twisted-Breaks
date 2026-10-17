# TWISTED BREAKS — Architecture

Version 1 design for a jungle / drum & bass performance sampler, break slicer,
sequencer and multi-FX instrument (JUCE 8, C++20, VST3 + AU, Ableton Live first).

This document answers the ten "first task" questions from the spec. Phase 1 is
implemented and tested; everything marked *(Phase N)* is the agreed design that
later phases build to.

---

## 1. Software architecture

Three layers, with one rule: **the audio thread never waits for anything.**

```
┌───────────────────────────── MESSAGE THREAD ─────────────────────────────┐
│  PluginEditor ─ MainView ─ PadGrid / WaveformEditor / FXGrid / Sequencer │
│        │                    View / MixerView / SnapshotView / Transport  │
│        │ (reads atomics, pushes commands)                                │
│  StateManager ── SnapshotManager ── SampleLoader (background thread)     │
└────────┬───────────────────────────────▲─────────────────────────────────┘
         │ SPSC queues / atomic swaps    │ atomics + lock-free FIFOs
┌────────▼───────────────────────────────┴───── AUDIO THREAD ──────────────┐
│ PluginProcessor::processBlock                                            │
│   TransportSync ──► TransportState (one per block, passed by const&)     │
│   AudioEngine                                                            │
│     ├─ MidiRouter (note→pad map, CC→FX/panic)                            │
│     ├─ Sequencer ─ SequencerLane ×16 ─ SequencerEvent        (Phase 7)   │
│     ├─ PadEngine ×16 ─ PadVoice ×N ─ SampleBuffer (shared_ptr, RT-safe)  │
│     │                  SliceEngine (slice table per pad)    (Phase 2–4)  │
│     ├─ GroupBus ×4 ─ FXChain (16 effects each)              (Phase 5–6)  │
│     └─ MasterSection (level, limiter, meters)                            │
└──────────────────────────────────────────────────────────────────────────┘
```

Key decisions

| Concern | Decision | Why |
|---|---|---|
| Host-facing shell | `PluginProcessor` is thin; all DSP lives in `AudioEngine` | Engine can be unit-tested with no host |
| Cross-thread comms | `EngineBridge`: SPSC queues (UI→audio), atomics/FIFOs (audio→UI) | No locks on the audio thread |
| Sample data | Immutable `SampleBuffer` objects swapped in via an atomic pointer; old ones freed on the message thread | Loading never blocks or allocates in `processBlock` |
| Timing | Everything musical is expressed in PPQ via `TransportState` | Sample-accurate sync with Ableton |
| Parameters vs state | APVTS for automatable controls only; a `ValueTree` for patterns, slices, sample paths, snapshots | Keeps Ableton's automation list usable |
| UI | Fixed 1200×800 logical console scaled as one unit; static art baked once | Low CPU in a busy Live Set |

## 2. Directory tree

```
twisted-breaks/
├─ CMakeLists.txt              plugin + test runner targets
├─ cmake/Sources.cmake         one shared source list
├─ .github/workflows/build.yml macOS universal build, tests, auval, pluginval
├─ docs/
│  ├─ ARCHITECTURE.md          this file
│  ├─ ABLETON_TEST_CHECKLIST.md
│  └─ screenshots/
├─ Tests/TestMain.cpp          headless test runner + editor snapshot
└─ Source/
   ├─ PluginProcessor.{h,cpp}  host shell, state save/restore
   ├─ PluginEditor.{h,cpp}     scaling wrapper around MainView
   ├─ Core/
   │  ├─ Parameters.{h,cpp}    stable parameter IDs + layout
   │  ├─ TransportSync.{h,cpp} host playhead → TransportState
   │  ├─ EngineBridge.h        lock-free UI⇄audio plumbing
   │  ├─ StateManager          (Phase 8) ValueTree schema + migration
   │  └─ SnapshotManager       (Phase 8)
   ├─ Engine/
   │  ├─ AudioEngine.{h,cpp}   owns all real-time DSP
   │  ├─ CalibrationVoice.*    Phase 1 test voice (replaced by PadVoice)
   │  ├─ MasterSection.*       level, limiter, metering
   │  ├─ PadEngine, PadVoice   (Phase 2–3)
   │  ├─ SampleBuffer, SampleLoader (Phase 2)
   │  ├─ SliceEngine, TransientDetector (Phase 4)
   │  ├─ GroupBus, FXChain     (Phase 5–6)
   │  ├─ Sequencer, SequencerLane, SequencerEvent (Phase 7)
   │  └─ Effects/              (Phase 6) FilterFX, BitCrusherFX, SampleRateFX,
   │                           DistortionFX, TapeFX, StutterFX, BeatRepeatFX,
   │                           GateFX, PitchDropFX, DelayFX, ReverbFX, CombFX,
   │                           ReverseFX, MuteFX
   └─ UI/
      ├─ Palette.h, PanelPainter.*, ReactorLookAndFeel.*   the skin
      ├─ MainView.*            console layout, master controls, nav rail
      ├─ PadGrid.*, PadComponent.*
      ├─ TransportDisplay.*, NeedleMeter.*, ScopeDisplay.*  (→ WaveformEditor)
      ├─ FXGrid.*
      └─ SequencerView, MixerView, SnapshotView (Phases 5–8)
```

## 3. What runs on the audio thread

**Audio thread (real-time, no allocation / locks / file I/O / GUI):**
`processBlock` → `TransportSync::update` → `AudioEngine::process` →
MIDI routing, pad voices, slice playback, sequencer step scheduling,
group buses, all 16 effects, master section, meter/scope publishing.

**Message thread:** editor and all components, parameter attachments,
`get/setStateInformation` (JUCE calls these off the audio thread),
snapshot capture, user edits to slices/patterns (published as new immutable
objects).

**Background thread (Phase 2, implemented):** `SampleLoader` — file reading,
format decoding and waveform-overview generation (transient analysis joins it
in Phase 4). `SampleManager` hands the finished, immutable `SampleBuffer` to
the audio thread through a lock-free queue. Every buffer is also held in a
message-thread keep-alive list, so reference counts can rise and fall on the
audio thread but never reach zero there; the list frees a buffer only when
nothing else holds it. A test counts heap allocations and frees made inside
`processBlock` across the whole suite: zero. Audio stays at the file's own
sample rate and voices compensate in their playback increment, so loading
never resamples.

Allocation happens only in `prepareToPlay` (voices, scratch buffers, FX delay
lines and beat-repeat/reverse capture buffers sized for the longest synced
division at the lowest supported tempo).

## 4. Host-sync strategy

* Once per block `TransportSync` reads `AudioPlayHead::getPosition()` and builds
  a `TransportState`: playing, BPM, time signature, PPQ at sample 0,
  `ppqPerSample`, last-bar-start PPQ, bar and beat.
* Any sample's musical position is `ppqAtBlockStart + offset × ppqPerSample`,
  so events land on the exact sample, not the block boundary.
* `nextBoundary(divisionPpq, from)` returns the sample offset of the next
  1/32, 1/16, beat or bar line inside the block. The sequencer, stutter, beat
  repeat, reverse capture and quantised snapshot switching are all built on
  this one tested function.
* Edge detection: `justStarted`, `justStopped`, and `jumped` (block didn't start
  where the previous block predicted — Ableton loop-brace wraps, relocating).
  On start or jump the sequencer re-derives its step from PPQ instead of
  counting, so it can never drift; beat-repeat buffers re-arm.
* The 32-step grid is anchored to PPQ 0 (Ableton bar 1), so patterns line up
  with the arrangement wherever playback starts.
* No playhead (some hosts while stopped): last tempo is held, transport treated
  as stopped; pads still play from MIDI and mouse.
* The plugin reports **zero latency** (no look-ahead anywhere), so Ableton's
  delay compensation never offsets pad response.

## 5. Sample voice design (Phase 2–3)

```
PadEngine (one per pad)
 ├─ settings: gain, pan, pitch, start, end, reverse, mode, loop rate,
 │            choke group, output group, velocity sensitivity
 ├─ slice table (from SliceEngine)
 └─ voices[kVoicesPerPad]  — PadVoice
       state: SampleBuffer ref, read position (double), increment,
              direction, region [start,end), loop flag,
              anti-click envelope (2 ms in / 5 ms out), velocity gain
```

* **Retrigger:** the sounding voice gets a 5 ms release while a fresh voice
  starts — the same pattern Phase 1's `CalibrationVoice` already uses and
  tests.
* **Choke:** triggering a pad tells every other pad in the same choke group to
  release its voices (5 ms fade).
* **Interpolation:** 4-point Hermite by default; the future "classic sampler
  character" modes add linear/drop-sample and pitch-linked rate + bit depth.
* **Loop mode:** crossfades 64 samples across the loop seam; host-sync option
  sets the rate so the region spans N beats at the host tempo (0.25×–2×).
* **Slice mode:** each pad (or MIDI note) plays region *n* of the slice table.
* **Future-proofing:** the voice count per pad is a constant; raising it plus
  a steal policy (oldest / quietest) gives polyphonic one-shots; layers and
  velocity layers become arrays of `SampleBuffer` refs per pad.

### Loops and tempo sync (Phase 3, implemented)

* LOOP pads repeat their START..END region while held (note-off / mouse-up
  releases with a 5 ms fade). The seam is crossfaded over up to 256 samples
  using audio just outside the region, so the join is inaudible; a region
  starting at the top of the file loops sample-exact.
* With SYNC on, the region's length in beats comes from the audio itself:
  the power-of-two beat count that puts its own tempo in 90-180 BPM. It does
  not depend on the current Live tempo, so it never flips between half and
  double time while you change tempo.
* The voice is phase-locked: each block it is told the host PPQ and checks
  its read position against `phase = (ppq / period) mod 1`. The increment is
  exact, so it normally never corrects; after a loop-brace wrap, relocate or
  tempo change it snaps back onto the grid. A loop triggered mid-bar enters
  at the matching point of the break.
* Choke groups release other pads in the group with a 3 ms fade. Mute and
  solo act as smoothed gain, so they never click.

## 6. Transient slicing (Phase 4, implemented)

As built (refined from the plan below after testing on a synthetic break):

* Spectral flux is computed in **two bands** (below ~250 Hz and above), each
  normalised by its own loudest onset but never by less than a fixed floor,
  and each with its own moving-median threshold. A kick under a ringing
  snare, or under constant hat noise, still stands out in its band; a
  recording with no hits stays below threshold instead of being scaled up.
* Onsets are **ranked by loudness jump** (RMS 20 ms after vs before), so
  COUNT 8 keeps kicks and snares over ghost hats.
* Slices start ~1 ms before the attack (on a zero crossing when there is
  one in that gap) and voices fade in over 0.5 ms, so the fade is finished
  before the hit — the transient keeps its punch.
* Slice tables (16 starts) travel to the audio thread by value through the
  same lock-free command queue as samples. Pads using the same file share
  one decoded buffer (SPREAD, or a reopened set).

Original plan:

Runs on the loader thread, never in `processBlock`, and never alters audio:

1. Mixdown to mono, then a **spectral-flux onset function**: 1024-point FFT,
   hop 256, sum of positive magnitude increases per bin, weighted toward the
   high band (snares/hats) with a low band term (kicks).
2. Normalise; adaptive threshold = moving median + *sensitivity* offset.
3. Peak-pick with a minimum gap (default 40 ms) so flams don't double-slice.
4. **Refine** each onset to the exact attack: search back ≤ 10 ms in the
   time-domain envelope for the last zero crossing before the rise, so slices
   start clean with no pre-click.
5. Keep the 16 strongest onsets (always including sample start), sorted.
6. The result is a **slice table** (`Array<int64>` sample positions) stored in
   state. Drag/add/delete edit the table; RESET restores the last detection;
   EQUAL SLICE replaces it with 16 equal regions.

## 6b. SWAP groups (0.4.1, implemented)

Hand-off between breaks at slice points (spec addition A1 in
[SPEC_ADDITIONS.md](SPEC_ADDITIONS.md)). All in `AudioEngine`:

* Per group (1–4): the playing pad (`swapPlaying`) and at most one
  `PendingSwap { pad, level, remaining samples }`. Group pads latch.
* A hit in a group with something playing computes how many output samples
  until the next landing point: the playing voice's next slice marker in
  its playback direction (distance / increment), or the next 1/16 / beat /
  bar from the transport's PPQ.
* `renderVoices` splits the span at the earliest pending landing, so the
  hand-off is sample-accurate even mid-block, then `fireSwap` releases the
  outgoing voice (1 ms) and starts the incoming one with `startAt`.
* `startAt` = the incoming break's grid position (synced, Live playing) or
  the same fraction through (otherwise), snapped to its nearest own marker
  within a 1/32 note. `PadVoice` keeps the difference as `phaseOffset`, so
  `syncTo` holds it there instead of snapping back onto the grid.

## 6c. Group buses (Phase 5, implemented)

* Every pad has a GROUP parameter (`pad01_group`, A–D, default A so older
  sets sound the same). Its voices render into that group's `GroupBus`
  input, not straight into the master.
* `AudioEngine::renderSpan` does voices → bus inputs → `GroupBus::mixInto`
  (FX hook, then level / pan) → master, in pieces no longer than the bus
  capacity. So the buses work at any host block size and at the sample-
  accurate splits made for MIDI and SWAP hand-offs, without allocating.
* Each bus: `grp_a_level` (dB, −60 = off … +6), `grp_a_pan` (balance),
  `grp_a_mute`, `grp_a_solo` (solo between groups), `grp_a_bypass` (skips the
  Phase 6 FX chain). Gains glide over 20 ms; a bus nothing played into this
  span costs only the glide bookkeeping.
* Post-fader peaks go to `EngineBridge::groupPeak` for the mixer's LED bars.
* UI: the MIXER key swaps the CRT for `MixerPanel` (four strips: header plate
  that routes the selected pad when clicked, fader, LED bar, PAN, M, S,
  BYPASS). The SOUND page has A / B / C / D keys; tiles show "GRP B".

## 6d. Sequencer (Phase 7, implemented)

* `Core/SequencerData`: 16 lanes x 32 steps, each step ONE packed atomic
  word (32 bits in 0.7 / 0.8, 64 bits from 0.9: see 6g), shared by UI,
  audio and state code with no locks or queues. Arm flags live there too.
* Steps sit on Live's absolute grid: step k starts at k x STEP ppq, played
  as `k mod LEN`, so starts, stops, relocates and loop-brace wraps need no
  special handling. Each block, `collectSequencerEvents` lists the steps due
  (including micro-timed ones up to half a step early), sorted by sample;
  `process` merges them with the MIDI events, so everything triggers at its
  exact sample.
* Live record: an armed pad's MIDI / mouse hit, while Live plays, is
  quantised to REC Q, written to the nearest step plus micro-timing, and
  that absolute step is skipped once so a hit made just early isn't doubled.
* UI: `SequencerPanel` in the screen area (16 x 32 grid, velocity drag,
  playhead, LEN / STEP / REC Q readouts, RUN, ARM, CLEAR). Recorded steps
  bump the data version; the UI marks the host state dirty.

## 6g. Step features (Phase 9, implemented)

* The step word grew to 64 bits (`SeqStep`): on, velocity, micro, CHANCE,
  RATCHET 1-4, PITCH, REV, SLICE 0-16, FX 0-16, COND, and HIT (bit 47 is
  "no hit", so 0 = hit). Old 32-bit words and saved hex strings read in
  unchanged; the snapshot frame carries the 64-bit words.
* `collectSequencerEvents`: CONDITION is decided from the pattern pass on
  Live's timeline (`floor(k / LEN)`), so A:B steps repeat with a loop brace
  and survive relocates; FIRST = the pass Live started in. CHANCE is rolled
  once, when the step's first hit comes due, and remembered per pad
  (`seqRollStep`) so ratchets follow it across block edges. RATCHET hits are
  `stepPpq / r` apart, each its own `padHit` event carrying the packed step.
* Pitch / reverse / slice: `triggerPad` builds the voice settings for that
  hit (`tweakedSettings`); the voice is flagged custom so the per-block
  settings update only touches its gains (the same path slice-sequencer hits
  use). SWAP and SLICE SEQ pads ignore these three (they own their playback).
* FX: `stepFxOn` / `stepFxOff` events (ON sorts before OFF on a tie) hold the
  effect on the pad's group (`fxStepHeld`, OR'ed with MIDI held / latched /
  parameter state in `applyFx`). Each holder records its end ppq, so only
  the last step holding an effect lets it go: back-to-back steps run
  unbroken. Live stopping, RUN off or PANIC releases them.
* FILL = the `seq_fill` parameter (kV11), excluded from snapshots.
* UI: the SEQUENCER view covers the transport box too (BPM / position move
  into its STEP row). STEP row = DragReadout models over the selected step;
  editing an empty step switches it on.

## 6h. MUTATE (Phase 10, implemented)

* `Core/Mutator` (message thread) holds the ORIGINAL (both sequencers, every
  lane + every SLICE SEQ pad's current slot, as packed words). Any change it
  didn't make itself (edit, live recording, snapshot recall, slot change:
  detected by the data versions) makes the current pattern the new original.
* Rules (`mutateLane`, `mutateSlices`, pure functions): on-beat hits are
  anchors (only a ratchet, never on the downbeat); off-beat hits may drop;
  ghosts only next to hits, quieter than the lane's average; micro-timing is
  never touched; rolls / FX throws favour the last beat; slice sequences
  trade slices between off-beat steps and re-hit tied steps. AMOUNT scales
  every chance (0 % = identity). Targets are seven bool parameters.
* A mutation is a `MutationFrame` (lane / slot masks + packed words) armed
  through an atomic pointer. `processBlock` finds its boundary with
  `switchOffset` (now / next beat / next bar, or every N bars from the song
  start for AUTO; a loop wrap landing on the boundary counts) and splits the
  block there (the same split as snapshots, now up to two cuts per block), so
  it lands on the exact sample. Frames retire by block epoch; with no audio
  running the message thread applies the frame itself.
* UNDO = the state the last frame replaced (UNDO again re-does); RESET = the
  original. DRIFT mutates the current pattern instead of the original.
* AUTO: during the last beat of every N bars the Mutator arms a frame for
  that bar line; a loop brace wrap re-arms it. `mut_go` (rising edge) is the
  MIDI-mappable MUTATE; it's excluded from snapshots.
* UI: the sequencer's bottom area has STEP / MUTATE tabs.

## 6i. Performance macros (Phase 11, implemented)

* `Core/MacroDefs`: eight recipes (DESTROY, AMEN MANGLE, REWIND, DARKEN,
  TAPE STOP, PITCH DIVE, JUNGLE ROLL, SCI-FI), each a list of (effect,
  setting, MILD value, HARD value). `macro_amount` picks the point between
  (choices round to an index).
* Engine: `applyFx (g)` = (parameter XOR MIDI latch) | MIDI held | step
  held, then every active macro targeting the group (`macro_group` A-D /
  ALL) ORs in its effects and overwrites their settings in a per-group
  copy (`appliedFxParams`), so the user's own settings are untouched and
  return the moment the macro is released. Active macros = parameters XOR
  MIDI latch | MIDI held; notes E4-B4 (76-83) hold them, or toggle with
  `macro_latch`. PANIC clears the MIDI state. Snapshots skip the macro keys.
* UI: the PERFORMANCE FX panel's fifth tab, MACRO (keys hold / toggle the
  `macro_<key>` parameters; lamps follow `EngineBridge::macroActive`).

## 6j. RESAMPLE and vintage CHARACTER (Phase 12, implemented)

* CHARACTER (`PadVoice::character`, per-pad `char` parameter): the read
  path emulates the machine storing the sample at its own rate and bit
  depth: position -> the machine's sample grid (k = pos x rate / fileRate),
  drop-sample (SP-1200, LO-FI) or linear between its samples (MPC60, S950),
  quantised to 12 / 8 bits, then a 2-pole output low-pass (MPC60 ~15 kHz,
  S950 ~9 kHz). Because it's done at read time, pitching down holds each
  stored sample longer: the pitched-down SP / MPC grit. CLEAN is unchanged
  (Hermite). Custom voices (slice hits, tweaked steps) follow it too.
* RESAMPLE recorder (`AudioEngine`): a 32-second stereo buffer and a group
  tap allocated in prepare(). Armed from the message thread; starts on the
  next bar-line sample (or the block it's armed in), records the master
  after the limiter, or one group's post-fader output (`GroupBus::mixInto`
  tap), for N bars at the current tempo; states idle / armed / recording /
  done in atomics.
* `Core/Resampler` (message thread): on `done` copies the take, writes a
  24-bit WAV (background thread) to Music / Twisted Breaks / Resamples,
  loads it onto the target pad through SampleManager (so sets reload it),
  sets LOOP + SYNC (AS LOOP) and AUTO SLICEs it. `rsmp_rec` (rising edge) is
  the MIDI-mappable REC.
* UI: the 7th top-rail key RESAMPLE (`ResamplePanel`).

## 6k. Scenes A / B + crossfader (Phase 13, implemented)

* The engine runs 32 pads (`kNumPads`): scene = pad / 16 (`kPadsPerScene`).
  Everything per-pad (voices, samples, both sequencers, snapshot frames,
  MUTATE masks) simply covers 32; groups A-D, FX, macros, master, bank and
  RESAMPLE are shared. Pads 17-32 have their own parameters (`pad17_*` ..,
  named "Pad B01 ..."), all with version hint kV15 so older sets keep their
  AU parameter order.
* Crossfader (`xfade` 0..1, `xfade_curve`): `params::xfadeGains` gives a
  gain per scene, multiplied into each pad's gain in `computeVoiceSettings`
  (so it sits before the group FX, and the voices' 20 ms gain glide keeps it
  click-free). SMOOTH: both full in the middle, each fades over its far half
  (the default centre leaves old sets unchanged). SHARP: cut in the last 5 %.
  UI CUT keys override it while held (`setCrossfaderCut`).
* `scene_view`: what the matrix, editor, sequencer lanes and the iRig show.
  `padForNote` looks in the scene on view first, so notes 36-51 play what you
  see; a note learned for one pad plays it in any view. Note swaps on learn
  stay within a scene. PadLights maps hardware pads by position in the scene
  on view. SPREAD never crosses into the other scene.
* Snapshots capture both scenes, never the crossfader or the view.
* UI: `PadGrid` keeps 16 tiles and re-points them (`PadComponent::setPadIndex`);
  `SceneStrip` (VIEW A / B, CUT A, crossfader, CUT B, CURVE) under the matrix.

## 6l. Sequence pads (0.14, spec addition A6)

* MODE SEQ ONE / SEQ LOOP: the pad's main-sequencer lane is skipped by the
  free-running pass and plays only from a `LaneRun` (anchor = clock ppq of
  step 1 on the song grid, `gridAnchor` = floor(ppq / LEN) * LEN, so it
  plays the step under Live's playhead; `start` = the LAUNCH boundary,
  nothing before it plays; an end cut; an optional next segment for SEQ
  ONE re-hits; a generation). SEQ ONE plays LEN steps from `start`. Lanes run on the slice sequencer's clock (Live's ppq, or the
  free clock when stopped), so they work with Live stopped.
* `emitStep` is shared by free and launched lanes (conditions, chance with
  ratchets following, FX holds, velocity scaled by the hit). Launched
  events carry `owner` + `ownerGen`; a stop / relaunch / hand-off bumps the
  generation and re-emits from the current sample, so stale steps are
  dropped. `laneStop` events cut the sound on a stop; `advanceLanes` moves
  segments on at block end and publishes `laneStep` + queued state.
* SWAP: `swapOwner` / `samplesUntilSwap` know running lanes (AT SLICE = the
  next STEP); `fireSwap` stops the outgoing lane and launches the incoming
  one at SAME STEP (its grid anchor: the step under the playhead) or
  STEP 1 (anchor = the hand-off sample).
* Loop-brace jumps re-lock grid anchors to the new playhead position and
  shift `start` / ends by the jump; Live starting or
  stopping (a clock domain change) ends launched sequences.

## 6m. Push 3 mode (0.15, spec addition A7)

* `PushProtocol.h`: Push 2/3 User-mode numbers (pads 36-99 row-major from
  bottom left, button CCs, relative encoders 71-79 with touch notes 0-8,
  strip note 12 + pitch bend, sysex header 00 21 1D 01 01, mode echo 0x0A,
  palette read 0x04) and nearest-colour mapping onto Push's own palette.
* `PushHub` (one per process, `shared()`): opens the User port by name,
  retries every 2 s, dispatches MIDI to the owning `PushController` under a
  lock (never the audio thread) and drops sends from non-owners. Owner =
  the instance whose window was clicked last (`MainView` mouse listener ->
  `claim()`).
* `PushController` (per instance): on the MIDI thread it follows User mode
  (Push's mode echo, or the USER key if there is none), plays pads into
  `bridge.controllerPadEvents` (its own SPSC queue; the engine drains it
  after the UI queue, same `recordHit` / `triggerPad` path) and writes
  `bridge.padPressure`; encoder / jog ticks go to atomics, everything else
  into an ordered SPSC inbox. A 50 Hz message-thread timer drains it
  (selection, steps, parameters with gestures, FILL / MUTATE / RESAMPLE)
  and diffs the 64 pad + button LEDs against what was last sent (full
  redraw on entering User mode and every 5 s). Leaving User mode or losing
  ownership releases held pads, FILL, gestures and held steps.
* Pressure: the engine smooths `padPressure` per block and calls
  `PadVoice::setPressure` with the pad's `press` parameter (kV17): 2-pole
  low-pass 18 kHz -> 300 Hz, tanh drive, stutter (1/16, 1/32, 1/64 from the
  press point, 32-sample edges) or pitch down an octave. Synced loops skip
  `syncTo` while stuttering or bent and re-lock afterwards.

## 6n. Maschine Mikro MK3 mode (0.16, spec addition A8)

* `MikroProtocol.h`: the Mikro's USB HID reports (buttons / encoder /
  strip report 0x01, pads 0x02, lights 0x80, screen 0xE0) from the
  open-source drivers, plus a 5x7 font for the 128 x 32 screen.
* `MikroUsb` (macOS IOKit HID): opens VID 0x17CC / PID 0x1700 non-
  exclusively, input reports on its own CFRunLoop thread.
* `MikroHub` (one per process): a thread (re)opens the Mikro every 2 s and
  writes the newest lights / screen halves (the message thread never waits
  on USB; 5 refused writes = treat as gone). Input goes to the owning
  `MikroController` under a lock; owner = the window clicked last.
* `MikroController`: on the USB thread it parses reports, switches mode /
  FIXED VEL / roll rate at once, and pushes pad hits, slice hits and roll
  on/off into `bridge.mikroEvents` (its own SPSC queue); the rest goes to
  an inbox drained at 50 Hz on the message thread (selection, steps,
  parameters with gestures, FX, snapshots, MUTATE / FILL / RESAMPLE), which
  rebuilds lights and screen and sends them only when they change. Any
  loss (unplugged, another owner) releases held pads, rolls, FILL, FX and
  gestures; a new owner or reconnect resets the input parser.
* Engine: `controllerSliceHit` (a slice, or the plain sound, straight to a
  voice) and `RollRun` per pad: `addRollEvents` inserts `rollHit` events on
  every 1/8 / 1/16 / 1/32 of the clock (Live's grid, or free), skipping
  grid points within 10 ms of the press that started the roll.

## 6e. Slice sequencer (Phase 7.1, implemented)

* `Core/SliceSeqData`: 16 pads x 8 slots x 32 steps, one 64-bit atomic per
  step (kind PLAY / REST / TIE, slice, rev, pitch, velocity, gate length,
  ratchet, chance, micro in 1/64 step) and one 32-bit atomic per slot
  (length, rate, swing, written). Unwritten slots play
  `defaultPattern()`: each slice on its nearest step plus micro-timing.
* Engine: a pad with SLICE SEQ on starts / stops its sequence when hit
  (the main sequencer only starts it). `addSliceEvents` lists the block's
  hits / releases on the slice clock (Live's PPQ, or a free clock when Live
  is stopped, restarted at the press so step 1 plays at once) and inserts
  them into the same sorted event list as MIDI and the main sequencer;
  events carry a generation so a stop / restart cancels stale ones, and
  inserts never land behind the play cursor. A hit plays from the slice
  start on through the break (or backwards from its end) until the next
  hit, a REST or its gate; when the break is already playing at exactly
  that spot (the as-recorded case) it just carries on, which is what makes
  the default pattern sample-exact.
* Slot changes land at the pattern start (re-anchoring if the step size
  differs); SWAP groups treat a running sequence as the playing pad and
  hand off on its next step; choke and PANIC stop sequences.
* UI: the pad editor's SEQ page (SLICE SEQ, SLOT, LEN, RATE, SWING) and
  `SliceSeqPanel` on the screen; edits materialise an unwritten slot first.
  Variations (`TwistedBreaksProcessor::sliceSeqOp`) run on the message thread.

## 6f. Snapshots and break bank (Phase 8, implemented)

* `SnapshotManager` (message thread): STORE captures every non-master
  parameter's value, the PADS tree, SEQ and SLICESEQ. RECALL decodes the
  samples it needs in the background (`SampleManager::requestBuffer`,
  shared with what's loaded), builds a `SnapshotFrame` (the engine's
  BlockParams for those values via `fillParamsFrom`, the buffers + slices,
  both sequencers as packed numbers, the parameters to write back) and arms
  it.
* `processBlock` finds the switch sample (`switchOffset`: now, or the next
  beat / bar line in this block); if it's part-way through, the block is
  split (buffer views, MIDI split into pre-sized buffers, the second half's
  transport shifted) so the switch is sample-exact. Applying: pad samples /
  slices (`AudioEngine::applySnapshotPads`), sequencer atomics, and the frame
  becomes the parameter override.
* `SnapshotManager::poll` then writes the parameters (host + UI follow),
  hands the buffers to `SampleManager::adoptPads`, drops the override (same
  values, so inaudible) and retires the frame, deleting it once the audio
  thread's block epoch has moved on twice.
* Program Change 1-8 and the `snapshot` parameter request recalls through
  the same path.
* `BreakBank` + engine: each pad's own sample and markers are kept apart from
  what it plays; FOLLOW BANK pads play the active bank slot. Bank changes
  are `SeqKind::bankSwitch` events (first on a tie), so they land on the
  beat / bar sample and pad regions are recomputed for the new length.
* `SampleManager::relocateMissing` searches a folder tree by file name and
  reloads, keeping slices.

## 7. The 16 performance effects (Phase 6, implemented)

As built (`Engine/FXChain`, `Core/FxDefs`, `UI/FXGrid`, `UI/FxEditorPanel`):

* One `FXChain` per `GroupBus`, run in place on the bus input before level /
  pan, in spec order. `FxDefs` is the single table (labels, settings, units,
  choice lists, IDs) used by the parameter layout, the DSP and the editor.
* Parameters per group: `grp_a_lpf` (on / off) and its settings
  (`grp_a_lpf_cutoff`, `_res`, `_drive` ...): 16 switches + 34 settings per
  group, 200 in all. FX MUTE's switch is `grp_a_fxmute` (`grp_a_mute` is the
  mixer's mute). A test asserts every ID is unique.
* An effect is on when its parameter (mouse / automation) XOR a MIDI latch
  is set, or while its MIDI note is held. MIDI notes C3–D#4 (60–75) not used
  by a pad drive FX 1–16 of the FX grid's selected group; LOCKED notes
  toggle a latch, MOMENTARY notes are on while held. Applied at the note's
  sample position. PANIC (any source) clears latches and tails; the on-screen
  PANIC button also switches the parameters off.
* Insert effects crossfade over 10 ms. STUTTER / REPEAT / REVERSE arm, then
  start on the next boundary of their size (sample-exact, from PPQ or a free
  clock when Live is stopped) with a 3 ms fade; STUTTER loops the slice that
  starts there, REPEAT the slice that just ended, REVERSE plays each chunk's
  predecessor backwards with 2 ms raised-cosine edges. Loop seams crossfade
  into the audio before the loop start. Each keeps 3 s of history.
* PITCH DROP reads its own history at a speed falling linearly from 1 to 0
  over its TIME (a tape stop), fading out as it stops.
* DELAY and REVERB are throws (send model): switching off stops the feed
  and the tail rings out; the chain idles only once the delay line has been
  silent for a whole delay time / the reverb output has died away.
* Idle groups skip the chain entirely; with everything on, 4 groups cost
  ~4 % of one core (measured in the test suite).

Original plan:

Each `GroupBus` owns one `FXChain` holding all 16 effect instances in fixed
order (MUTE … REVERSE). Every effect implements:

```cpp
struct PerformanceFX {
    virtual void prepare (double sr, int maxBlock) = 0;   // allocate here
    virtual void process (AudioBuffer&, const TransportState&) noexcept = 0;
    virtual void reset() noexcept = 0;
    SmoothedValue<float> wet;   // 0..1, 10 ms ramp on engage/disengage
};
```

* **Engage/disengage is a wet/dry crossfade**, so toggling any FX is
  click-free. Disengaged effects at wet = 0 are skipped (no CPU) except
  capture-based ones that must keep recording.
* **LOCKED / MOMENTARY** is a UI/MIDI-mapping property of each key; the engine
  just sees on/off per group via 64 automatable bools (`grpa_fx01_on` …).
* Filters: TPT state-variable (JUCE `dsp::StateVariableTPTFilter`) with
  pre-drive. Crush: quantise to 2^bits with TPDF-free hard steps (it's meant to
  be dirty). Rate: sample-and-hold with optional stepped ratios. Drive: soft
  (tanh) / hard clip / asymmetric saturation, 2× oversampled. Tape:
  saturation + one-pole HF roll-off (wow/flutter later). Gate: tempo-synced
  amplitude chopper with 2 ms edges.
* **Stutter / Beat Repeat / Reverse** share a per-group `CaptureBuffer`
  (circular, preallocated for 2 bars at 60 BPM). Engagement is quantised with
  `nextBoundary`; the repeat window is crossfaded 3 ms at each loop seam;
  Reverse plays the captured slice backwards with a raised-cosine window.
* **Pitch Drop:** variable-rate read from the capture buffer whose rate ramps
  from 1.0 toward 0 over a synced time — the classic tape-stop/jungle dive.
* **Delay:** synced feedback delay with filtered feedback. **Reverb:**
  `juce::dsp::Reverb` (Freeverb) with pre-filter. **Comb/Resonator:** short
  tuned delay (pitch in semitones), feedback, one-pole damping in the loop.

## 8. State and preset serialisation

Everything lives under one `ValueTree`, saved as XML inside the binary blob
Ableton stores in the `.als` file:

```xml
<TWISTED_BREAKS>
  <PARAM id="master_gain" value="0"/> …          APVTS (automatable)
  <META stateVersion="1" pluginVersion="0.1.0"/>
  <MIDI_MAP><PAD index="0" note="36"/> …</MIDI_MAP>            (Phase 1)
  <PADS><PAD index="0" path="/…/amen.wav" hash="…" missing="0"
            start="0" end="…" mode="slice" …>
           <SLICES positions="0,5512,11025,…"/></PAD> …</PADS>  (Phase 2–4)
  <PATTERNS length="32"><LANE pad="0" steps="1,0,…" vel="…"/></PATTERNS> (7)
  <SNAPSHOTS active="0"><SNAPSHOT slot="0"> …full copy… </SNAPSHOT></SNAPSHOTS> (8)
</TWISTED_BREAKS>
```

* **Stable parameter IDs** (`master_gain`, `pad01_gain`, `grpa_level`,
  `grpa_fx07_on` …) with JUCE version hints; never renamed.
* `stateVersion` gates migrations so old Live Sets always open.
* **Samples** are referenced by absolute path plus a path relative to the Live
  Set if known, and a size+hash fingerprint. Restoring kicks off background
  loads; a missing file marks the pad **MISSING** (red lamp, path shown, click
  to relocate) — the set still opens and every other setting is kept.
* **Snapshots** store a complete copy of pad settings, slices, group mixer,
  FX state and patterns. Switching builds the new engine state on the message
  thread, then hands it to the audio thread at the chosen boundary
  (immediate / next beat / next bar); voices crossfade.

## 9. JUCE / Ableton pitfalls (and how this design avoids them)

| Pitfall | Mitigation |
|---|---|
| Ableton calls `getStateInformation` often (every save, undo point) | Serialise cheaply from the ValueTree; never touch audio-thread state directly |
| `setStateInformation` can arrive before `prepareToPlay` | State restore never assumes a sample rate; sample loads resample on arrival |
| Block sizes vary and can be 1 sample (Live's automation splitting) | No logic assumes a block size; all timing is PPQ-based |
| Live's loop brace makes PPQ jump backward | `jumped` detection re-syncs sequencer/repeat |
| Transport stop ≠ silence: Live keeps calling `processBlock` | Voices ring out; sequencer stops scheduling; no reset on stop |
| AU parameter order must be stable | Version hints on every `ParameterID` |
| Too many automatable params makes Live sluggish/unusable | ~200 curated params; everything else is state |
| AU not showing after rebuild | AU cache: `killall -9 AudioComponentRegistrar`, then rescan |
| VST3 & AU in the same Live Set | Both must share state format — they do (same XML) |
| Unsigned/quarantined plugins blocked on macOS | Ad-hoc signing in CI; `xattr -dr com.apple.quarantine` for downloaded builds |
| Denormals in filters/reverb tails | `ScopedNoDenormals` in `processBlock` |
| Plugin window too large on laptops | Editor resizes 75–150 % with locked aspect |
| Live caches AU state and ignores plugin-window edits on save / duplicate / undo | Plugin calls `updateHostDisplay (nonParameterStateChanged)` after every UI gesture and sample / note-map change (debounced; never for automation) |
| Note numbering confusion (C1 vs C3) | UI shows names in Ableton's convention (36 = C1) |
| Sample rate change between sets | Everything re-prepared in `prepareToPlay`; samples resampled in background |

## 10. Staged roadmap

| Phase | Scope | Exit test |
|---|---|---|
| **1 ✅** | Compiling VST3/AU, stereo out, MIDI in, host transport, master section, skin | 40/40 automated checks, pluginval strictness 7 passes (incl. editor + fuzz), CI builds universal binaries |
| **2 ✅** | `SampleBuffer`, background `SampleLoader`, `SampleManager`, `PadVoice`; load WAV/AIFF (drag & drop / LOAD), waveform editor on the CRT, MIDI trigger, pitch, gain, pan, start/end, reverse on every pad | 90/90 automated checks incl. sample-exact output and zero allocations in `processBlock`; pluginval strictness 7 |
| **3 ✅** | Per-pad mode (one-shot / loop), tempo-synced loops phase-locked to PPQ, rates, choke groups, mute / solo, velocity sensitivity, MIDI learn | 126/126 automated checks incl. sync locked to the grid over 6 s, re-lock after jumps / tempo changes, seam crossfade, click-free choke |
| **4 ✅** | `TransientDetector` (two-band spectral flux), `SliceTable`, slice editing on the CRT, equal / transient slicing, SLICE mode, SPREAD to pads with shared audio | 158/158 checks: every kick and snare of a test break found, slices ≤ 2.5 ms before the attack, no false slices on a tone, slice playback, edits, SPREAD memory sharing, state |
| **4.1 ✅** | SWAP groups: latched pads hand off to each other at the next slice point (or 1/16 / beat / bar), continuing from the same point in the incoming break | 175/175 checks: lands within 2 samples of the marker, incoming starts on its own marker, stays on the grid, AT BAR, cancel, stop, transport stopped, state |
| **5 ✅** | Four `GroupBus`es (level / pan / mute / solo / FX-bypass hook), per-pad GROUP routing, MIXER view, group meters | 195/195 checks: exact -6 / -12 dB scaling, pan, mute, solo, re-routing, click-free mute glide, oversize host blocks render identically, MIXER fader / header / GROUP keys drive the parameters, state |
| **6 ✅** | 16 performance FX per group, LOCKED/MOMENTARY, MIDI notes + automation, FX editor view | 267/267 checks: each effect measured (filter slopes, crush levels, S&H runs, clip, roll-off, sample-exact stutter / repeat / reverse / delay timing, gate on the grid, pitch dive, comb pitch), click-free engage, throws, group independence, bypass, MIDI latch / momentary, mouse keys, state, ~4 % CPU with all 64 on |
| **7 ✅** | 32-step sequencer (16 lanes, velocity, length, step size, RUN), live record with quantise | 296/296 checks: steps sample-exact mid-block, length / step size, micro-timing, stopped / RUN off silent, jumps and restarts land on the right step, recording at 1/4–1/32 (finer kept as micro-timing) without double hits, mouse + MIDI, state, grid editing |
| **7.1 ✅** | **Per-pad slice sequencer** (spec addition A5) | 333/333 checks: untouched pattern replays the break sample-exactly, rearranged / REST / REV / RATCHET / gate / PITCH / CHANCE / MICRO / SWING / RATE timed to the sample, start / stop, free clock when stopped, slot switch at the pattern start, FILL, main-sequencer start, SWAP hand-off on the step, edit ops, screen editing, STEP REC, state |
| **8 ✅** | Snapshots (8 slots, NOW / BEAT / BAR, Program Change, automation), break bank (A2), find missing samples | 362/362 checks: recall restores params / samples / patterns, master untouched, NEXT BAR switch on the exact bar-line sample mid-block, bank switch on the beat sample-exact, state, relocate, UI, 240 switches in 2 min with the worst block at 0.5 ms and nothing leaking |
| **9 ✅** | Step features in the main sequencer: CHANCE, RATCHET, PITCH, REV, SLICE, FX (held for the step, FX-only), conditions (A:B, FILL, FIRST), FILL key / parameter, STEP row | 399/399 checks: ratchets sample-exact, chance statistics with all-or-nothing ratchets, every condition incl. starts mid-song, pitch / reverse / slice measured and held across blocks, FX held exactly one step and unbroken back-to-back, FX-only doesn't retrigger, stop releases, old 32-bit sets load, UI edits |
| **10 ✅** | MUTATE: controlled randomisation of both sequencers (slices, pitch, pattern, ratchet, reverse, FX, chance), AMOUNT, SCOPE, AUTO every 1-8 bars, NOW / BEAT / BAR, DRIFT, UNDO / RESET, Mutate parameter | 426/426 checks: 300 seeded mutations never touch an on-beat hit or move anything off the grid, ghosts only beside hits, targets respected, empty lanes stay empty, slice beats kept; original / undo / redo / reset / edit-becomes-original / drift accumulates; scope; parameter edge; NEXT BAR lands on the bar-line sample mid-block; AUTO 2 BARS on exactly bars 3 and 5; slice slots; UI |
| **11 ✅** | Performance macros: DESTROY, AMEN MANGLE, REWIND, DARKEN, TAPE STOP, PITCH DIVE, JUNGLE ROLL, SCI-FI; AMOUNT (mild .. hard), GROUP, LATCH, MIDI E4-B4 | 441/441 checks: every macro engages exactly its recipe in every group with its settings and releases; AMOUNT mild .. hard; single group; borrowed effects return exactly; MIDI hold / latch; DARKEN measured on noise; snapshots skip them; UI |
| **12 ✅** | RESAMPLE (master or a group, 1-8 bars from the bar line, onto a pad as a synced, sliced loop, saved as WAV) + vintage CHARACTER per pad (CLEAN, SP-1200, MPC60, S950, LO-FI) | 464/464 checks: character read path (bit grid, machine-rate steps, pitched-down holds twice as long, output filters measured), 12-bit values through a pad; the take equals one bar of the output from the bar-line sample, 24-bit WAV, loaded as a synced loop, sliced, group-only take, stopped / NOW, cancel, parameter, reload from the set; UI |
| **0.14 ✅** | Sequence pads (A6); 0.14.1 phase-locks launched lanes to Live's playhead: SEQ ONE / SEQ LOOP, LAUNCH, per-pad LEN, stop / restart, SWAP between sequences (STEP / 1/16 / BEAT / BAR, SAME STEP / STEP 1), mixed groups | 507/507 checks: launch on NOW / 1/16 / BEAT / BAR mid-block, ONE vs LOOP, idle until hit, stop + cancel, SEQ ONE restart, SEQ LEN, velocity, conditions from the launch, free clock, SWAP at STEP / 1/16 / BEAT / BAR with no doubled or missing step, SAME STEP / STEP 1, mixed group, choke, Live stop, loop-brace wrap, state, UI |
| **0.15 ✅** | Push 3 mode (A7): User-mode grid (32 pads + 32 steps / big pads), step edit, encoder pages, touch-strip crossfader, buttons, lights on Push's palette, instance ownership; per-pad pressure | 556/556 checks: port naming, palette read + mapping, mode echo / USER fallback, pad play + velocity + A/B layout, selection both ways, pressure, modifiers, step tap / edit / range / delete, pages + gestures + fine, strip, buttons, LED states + diffing, leaving User mode, two instances, state; pressure FILTER / DRIVE / PITCH / STUTTER / OFF |
| **0.16 ✅** | Mikro MK3 mode (A8): pads / steps / slices / FX / snapshots, encoder + screen pages, strip crossfader, note-repeat rolls, buttons, ownership, USB writes off the UI thread | 598/598 checks: protocol, pad layout, play + velocity + pressure, modifiers, GROUP, pages, strip, STEP edit + pages + lights, KEYBOARD slices, NOTE REPEAT rolls (grid-exact, no flam), PERFORM FX, SCENE snapshots, FILL, FIXED VEL, two instances, state, off clears; engine slice 9 + rolls |
| **0.16.1 ✅** | Pad matrix laid out like hardware (01 bottom left, 16 top right, MIDI 36 bottom left like a Drum Rack); Push grid and Mikro pads follow; steps and the FX grid still read from the top left | 599/599 checks: matrix tile positions, Push / Mikro mappings |
| **0.17 ✅** | A9: sequencer follows Live only (RUN ignored); Twisted Breaks Remote virtual port (Mikro Play / Stop / Rec, Push Play / Record -> Live, MIDI-mapped); Mikro chosen slice -> steps, slice colours, hold-KEYBOARD pick, slice recording | 608/608 checks: RUN ignored, chosen slice + step rules, slice colours, hold KEYBOARD, SHIFT clear, remote notes from Mikro and Push, PLAY light, slice recording |
| **0.18 ✅** | A10: whole-pad vs slice steps on the Mikro (last pick decides, hold PAD MODE in STEP, NO SLICES), per-pad WHOLE ONLY (`whole`, kV18; engine ignores step slices) | 615/615 checks: engine whole vs slice vs WHOLE ONLY, pad / slice picks, tap rules, hold PAD MODE, NO SLICES, reset on selection |
| **0.18.1 ✅** | Mikro STEP mode uses pad numbers for steps (step 1 bottom left, 16 top right); FX grid stays in screen order | 616/616 checks: bottom-left = step 1, top-right = step 16, step lights |
| **0.19 ✅** | A11 part 2: `PadOps` (copy / paste / swap / clear of a pad's sample, settings and steps; `SampleManager::assignPad` shares buffers); undo history of pad edits: explicit ops recorded at once, everything else picked up by a quiet-period watcher on the sequencer / slice-seq / sample versions | 644/644 checks: paste parts, swap, clear, undo / redo, watcher (steps, markers, loads), cap, project load resets, Mikro and Push DUPLICATE |
| **13 ✅** | Scene A / B (32 pads, each scene its own pads + patterns), crossfader (SMOOTH / SHARP, CUT keys, automatable), VIEW A / B (parameter), notes follow the view, iRig follows the view | 485/485 checks: both scenes play at once; crossfader gains (centre, ends, SMOOTH 3/4, SHARP edge), CUT; view-aware MIDI, learned notes; snapshots / state with scene B, old sets open with B empty; SPREAD stays in its scene; UI view switch keeps the position, sequencer follows, fader drag / double-click, CUT |
