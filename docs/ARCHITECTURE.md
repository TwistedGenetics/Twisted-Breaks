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

## 6. Transient slicing (Phase 4)

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

## 7. The 16 performance effects (Phase 6)

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
| 4 | `TransientDetector`, `SliceEngine`, slice editing UI, equal/transient slicing | Amen slices on every hit at default sensitivity |
| 5 | Four `GroupBus`es, mixer view, routing | Group mute/solo/pan correct |
| 6 | 16 performance FX per group, LOCKED/MOMENTARY, MIDI-mappable | All FX click-free, synced FX on the grid |
| 7 | 32-step sequencer, live record with quantise | Sequencer sample-accurate vs. Live's metronome after loop wraps |
| 8 | Snapshots (4+ slots, quantised switching), StateManager, missing-sample recovery | Full Live Set round-trip, snapshot switch with no dropouts |
| 9+ | Scene A/B + crossfader, per-step probability/ratchets, MUTATE, macros, resample, vintage modes | — |
