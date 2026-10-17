# TWISTED BREAKS

A jungle / drum & bass performance sampler, break slicer, sequencer and
multi-FX instrument for Ableton Live — built in C++ with JUCE 8 as a real
**VST3** and **Audio Unit** plugin.

Skinned as a forty-year-old Soviet power-plant control console (Russian
in the header, English labels everywhere else): oxidised
enamel, rust, bakelite label plates, amber annunciator pads, a green-phosphor
CRT and a red emergency-stop panic button.

![Twisted Breaks console](docs/screenshots/phase4-slice.png)

## Install (no building needed)

Every green build publishes a release. Open **Releases** on the right of the
repo page, download **TwistedBreaks-macOS.zip** from the newest one, and
follow the `INSTALL.txt` inside: copy the `.vst3` and `.component` into your
plug-in folders, clear macOS quarantine, rescan in Ableton.

**Operator's manual:** [docs/manual/index.html](docs/manual/index.html) (open it in a browser).

## Status

| Phase | What | State |
|---|---|---|
| 1 | Plugin shell, MIDI, host transport, master section, skin | **done** |
| 2 | Sample playback: load, waveform editor, pitch, gain, pan, start/end, reverse | **done** |
| 3 | Pad modes (one-shot / loop), tempo-synced loops, choke groups, mute / solo, velocity, MIDI learn | **done** |
| 4 | Transient detection, slice markers, SLICE mode, SPREAD to pads | **done** |
| 4.1 | SWAP groups: hand off between breaks at slice points | **done** — 175/175 automated checks |
| 5 | Four group buses + mixer | **done** |
| 5.1 | iRig Pads lights: controller pads show what's playing | **done** |
| 5.2 | FIXED velocity key per pad | **done** — 216/216 automated checks |
| 6 | 16 performance FX per group | **done** — 267/267 automated checks |
| 7 | 32-step sequencer + live recording | **done** — 296/296 automated checks |
| 7.1 | Per-pad slice sequencer | **done** — 333/333 automated checks |
| 8 | Snapshots, break bank, find missing samples | **done** — 362/362 automated checks |
| 9 | Step features: chance, ratchet, pitch, reverse, slice, FX, conditions | **done** — 399/399 automated checks |
| 10 | MUTATE: controlled randomisation of both sequencers | **done** — 426/426 automated checks |
| 11 | Performance macros | **done** — 441/441 automated checks |
| 12 | RESAMPLE + vintage CHARACTER modes | **done** — 464/464 automated checks |
| 13 | Scene A / B + crossfader | **done** — 485/485 automated checks |
| 0.14 | Sequence pads: launch a pad's sequence by hitting it, SWAP between sequences; 0.14.1: launched sequences follow Live's playhead | **done** |
| 0.15 | Push 3 mode: play and program the plugin from Push 3 in User mode; per-pad pressure | **done** |
| 0.15.1 | Maschine Mikro MK3 over USB, step 1: a hardware test (LIGHTS > Mikro MK3 test) | **done** — tested on Jamie's Mikro |
| 0.16 | Mikro MK3 mode: pads, steps, slices, FX, snapshots, encoder + screen, strip, note repeat | **done** |
| 0.16.1 | Pad matrix numbered like hardware: 01 bottom left, 16 top right (Push and Mikro follow) | **done** |
| 0.17 | Sequencer follows Live only (no RUN key); Mikro / Push Play-Stop-Rec drive Live (Twisted Breaks Remote port); build breaks slice by slice on the Mikro | **done** |
| 0.17.1 | Mikro pads fixed: it reports pads top-left first, so pad 1 (bottom left) now plays 01 | **done** |
| 0.18 | Whole-pad steps and slice steps: pick a pad (whole) or a slice on the Mikro, hold PAD MODE in STEP to pick the pad, per-pad WHOLE ONLY | **done** — 615/615 automated checks |
| 0.18.1 | Mikro steps numbered like the pads: step 1 bottom left, 16 top right | **done** — 616/616 automated checks |
| 0.19 | Copy / paste / swap / clear between pads (menu, keys, Alt- / Shift-drag, Mikro and Push DUPLICATE) and a 50-step pad undo | **done** — 644/644 automated checks |
| 0.20 | Preset manager: kit / pad / sequence presets with their samples inside (FLAC), browser (filters, search, tags, favourites, listen), Collect Samples, factory kits, Mikro / Push browsing | **done** — 672/672 automated checks |
| 0.21 | TWIST (randomise a pad's sound around where you left it, amount + locks), layers (round robin / velocity / random, up to 4 per pad), fit to Live's tempo | **done** — 683/683 automated checks |
| 0.22 | Export: WAV (a pad or the kit, N bars at Live's tempo, rendered offline from a copy) and MIDI clips of the sequencer, drag into Live; song mode (a chain of snapshots × bars, on Live's bar lines) | **done** — 690/690 automated checks |
| 0.22.1 | MIDI out of what you play (plugin MIDI out + "Twisted Breaks Notes" port) to record jamming in Live; Live stopping stops every break | **done** — 700/700 automated checks |
| 0.23 | STRETCH per pad: Repitch / Beats / Texture / Akai Classic / Akai Revised for one-shots, slices, steps and slice sequences; tempo from the markers, set / double / halve | **done** — 712/712 automated checks |
| 0.24 | Macro editor: every effect setting of each macro (MILD / HARD or leave), add / remove effects, rename, hold to hear, reset, .tbmacro files | **done** — 720/720 automated checks |
| 0.25 | AKAIZER: the Akai S-series cyclic pitch shift as a group FX in STUTTER's place (Classic / Revised, pitch, cycle); RENDER AKAI on a pad; reference renders in docs/audio/akaizer | **done** — 724/724 automated checks |
| 0.25.1 | Record your jamming on the plugin's own track (Notes port as the track's input; the plugin skips its own notes coming back) | **done** |
| 0.25.2 | Push 3 and the Mikro MK3 are found on their own when plugged in (no mode to switch on); LIGHTS is now **CONTROLLERS**, with each one's status | **done** — 731/731 automated checks |

Sections that aren't built yet are visibly marked offline on the panel
(dark lamps and taped-on tags).

### What works now
* **Load samples** (WAV / AIFF, also FLAC) onto any of the 16 pads: drag a
  file onto a pad, onto the screen, or press **LOAD**. Decoding happens in the
  background, so loading never glitches audio. **CLEAR** empties a pad.
* Pads show the sample name, length and channels; a missing file shows
  **MISSING** in red and keeps its path.
* **Waveform editor** on the CRT for the selected pad: drag the **S** / **E**
  markers to set start and end, wheel or pinch to zoom, drag to scroll,
  double-click to zoom out, with a live playback cursor. Non-destructive.
* **SOUND page** per pad: **GAIN, PAN, PITCH** (+/-12 semitones), **START,
  END, REVERSE**.
* 567 Ableton-automatable parameters with stable IDs (`pad01_gain` ...
  `pad16_fixvel`, `grp_a_level` ... `grp_d_bypass`, `grp_a_lpf` ...
  `grp_d_reverse_size`).
* Sample-exact playback with Hermite interpolation, velocity sensitivity,
  automatic sample-rate conversion (a 44.1 kHz break plays correctly in a
  48 kHz set), click-free starts, ends, retriggers and panic.
* **PLAY page** per pad (the SOUND / PLAY keys switch pages):
  * **MODE**: ONE (plays once) or LOOP (repeats while the pad / note is
    held, with a crossfaded loop point), or SLICE (see below).
  * **SYNC**: loops lock to Ableton's tempo and bar grid. The loop length
    in beats is worked out from the audio (a 4-beat break stays 4 beats),
    triggering mid-bar enters at the right point, and it re-locks after
    loop-brace jumps and tempo changes.
  * **RATE**: 1/4, 1/2, 1x, 2x loop speed.
  * **CHOKE** 1-4: a hit stops the other pads in the same group (3 ms fade).
  * **VEL**: velocity sensitivity, 0% (always full) to 100%.
  * **FIX**: fixed velocity, every hit at full level (automatable
    `pad01_fixvel`).
  * **M / S**: mute and solo.
  * **NOTE**: drag or scroll to change the pad's MIDI note, or LEARN and
    play a note on your controller.
* **SLICE page** (or the SLICER key in the top rail):
  * **AUTO SLICE** finds the hits in a break (kicks, snares, hats) and puts
    a numbered marker on each, just before the attack. **SENS** sets how
    quiet a hit it picks up; **COUNT** (4 / 8 / 16) keeps the strongest.
  * **EQUAL** cuts COUNT equal slices instead.
  * On the screen: drag markers, double-click to add, double-click a marker
    to delete, click a slice to select it (and hear it in SLICE mode).
    **RESET** undoes your edits, **UNSLICE** removes them, **FIT** zooms out.
  * **SLICE mode** (PLAY page, MODE) plays the slice chosen by the **SLICE**
    knob (automatable: `pad01_slice`).
  * **SPREAD** puts each slice on its own pad from the selected pad onwards,
    in SLICE mode, sharing one copy of the audio — drag a break in, AUTO
    SLICE, SPREAD, and play it from your controller.
* **SWAP page**: put pads holding sliced breaks in the same **SWAP group**
  (1-4). One plays at a time, latched. Hit another and it takes over at the
  playing break's **next slice marker** (or next 1/16, beat or bar: **AT**),
  carrying on from the same point in its own break and starting right on
  its own hit. The waiting pad flashes. Hit the playing pad again to stop.
* **Group buses A-D**: every pad goes to a group (**GROUP** keys on the
  SOUND page; tiles show "GRP B"). The **MIXER** key swaps the screen for
  four channel strips: **fader**, LED meter, **PAN**, **M**ute, **S**olo and
  FX **BYPASS** (FX arrive in Phase 6). Click a strip's name plate to send
  the selected pad there. All automatable (`grp_a_level` ... `grp_d_bypass`).
* **CONTROLLERS** (bottom-left, next to SELECTED PAD; was LIGHTS): shows
  whether Push 3 and the Mikro MK3 were found, and holds MIDI OUT. Pick your **iRig Pads** and
  its pads light up: green while a pad plays (brightness follows the sound),
  dim green for loaded pads, flashing red while a SWAP is queued. Works out
  of the box on the iRig's default scene; **Learn pad layout** handles any
  other scene (hit each pad as it lights red). **Test lights** and **Swap
  red / green** in the same menu. Saved with the set.
* **Performance FX** (bottom middle): pick a group (GRP A-D), then the 16
  keys switch that group's effects: MUTE, LPF, HPF, BPF, CRUSH, RATE,
  DRIVE, TAPE, STUTTER, REPEAT, GATE, PITCH (drop), DELAY, REVERB, COMB,
  REVERSE, in that order in the chain. Each key is LOCKED (click on / off)
  or MOMENTARY (held), marked L / M. STUTTER, REPEAT and REVERSE land on the
  next 1/16, 1/8 ... of Live's grid; DELAY and REVERB are throws whose tails
  ring on. The **FX** key opens the FX editor for the picked key (right-
  click a key to edit without switching it): its settings, LOCKED /
  MOMENTARY, ALL OFF. MIDI notes **C3-D#4** play FX 1-16 of the selected
  group; everything is automatable. PANIC stops tails and switches FX off.
* **SEQUENCER** (top rail): a 32-step lane for every pad. Click a cell for a
  step, drag up / down on it for velocity, click it again to clear it.
  **LEN** 1-32, **STEP** 1/8 / 1/16 / 1/32: automatable. It plays exactly
  while Live plays (0.17: no RUN key; the old `Sequencer Run` parameter
  is ignored) and follows Live's transport exactly (sample-accurate, restarts and loop-brace
  jumps land on the right step). **ARM** a pad (click its lane number to
  select it) and hits from the mouse or MIDI are recorded while Live plays,
  quantised to **REC Q** (1/4, 1/8, 1/16, 1/32; finer than the step size
  is kept as micro-timing). Patterns are saved with the set.
  **Step features (0.9):** the STEP row under the grid edits the selected
  step (click a cell; right-click or shift-click to select without
  switching it off): **CHANCE** (rolled each time round), **RAT**chet 1-4,
  **PITCH** ±24, **SLICE** (play slice 1-16 of the pad's break), **REV**,
  **MICRO**, **FX** (that effect held on the pad's group for the step;
  switch **HIT** off to throw the FX without retriggering the pad) and
  **COND**: 1:2 ... 4:4 (pass A of every B, on Live's timeline), FILL /
  !FILL (hold **FILL**, or map / automate `Sequencer Fill`), FIRST / !FIRST.
* **SEQUENCE PADS** (0.14): set a pad's MODE to **SEQ1** or **SEQL** and
  hitting it plays its sequencer lane once or looped (its lane no longer
  runs by itself). It always plays in time with Live's playhead (the step
  under it, lined up with the bar); **LAUNCH** (NOW / 1/16 / BEAT / BAR)
  sets when it comes in, **LEN** gives it its own length. Hit a looping one again to stop
  it on the next boundary. Put SEQ pads in a **SWAP** group and they cut
  each other's sequences on the next STEP, 1/16, beat or bar, carrying on
  from the same step (or from step 1 with **FROM STEP 1**).
* **PUSH 3** (0.15): found on its own (0.25.2); press **USER** on
  Push. The top 4 rows are the 32 pads (A left, B right), the bottom 4 the
  selected pad's 32 steps (tap, hold + encoders to edit, hold + tap for a
  range); encoders on PAD / FX / SEQ / MIX pages; the touch strip is the
  crossfader; per-pad pressure goes to each pad's **Pressure** setting
  (filter, drive, stutter, pitch). Press USER again to give Push back to
  Live. In Live's MIDI settings turn **Track** off for Push's User Port.
* **MIKRO MK3** (0.16): close NI's Maschine app and plug it in; it is
  found on its own (0.25.2). The plugin talks to the Mikro over USB and owns its
  pads, lights, touch strip and screen. **PAD MODE**: the 16 pads of the
  scene on view (**GROUP** flips A / B); **STEP**: the selected pad's steps
  (‹ › for 17-32, hold + encoder to edit); **KEYBOARD**: its 16 slices;
  **PERFORM**: the 16 FX; **SCENE**: snapshots 1-8 (SHIFT stores). The
  encoder + screen edit the **PLUG-IN** (pad), **SWING** (sequencer) and
  **VOLUME** (mix) pages; the strip is the crossfader; **NOTE REPEAT**
  rolls held pads (SHIFT: 1/8 / 1/16 / 1/32); **VARIATION** = MUTATE,
  **TAP** = FILL, **SAMPLING** = RESAMPLE. **Slice by slice** (0.17): hit a
  slice on KEYBOARD, tap steps in STEP to place it (hold KEYBOARD from STEP
  for a quick pick); steps light in their slice's colour. **Whole-pad
  steps** (0.18): pick a pad in PAD MODE (or hold PAD MODE in STEP) and the
  steps you tap play the whole pad, looped / one-shot until choked;
  **WHOLE** in the SEQUENCER view makes a pad's steps always whole. **PLAY / STOP /
  REC** drive Live through the **Twisted Breaks Remote** MIDI port: in
  Live turn Remote on for it and MIDI-map Play, Stop and Record once
  (notes 94 / 93 / 95, channel 16; Push 3's Play / Record send the same).
* **SCENES** (0.13, the strip under the pad matrix): two decks of 16 pads,
  **A** and **B**, each with its own samples, settings and patterns, both
  playing all the time. **VIEW A / B** picks which 16 the matrix, editor,
  sequencer and your controller show (the `Scene View` parameter: map it to
  a button). The **crossfader** blends them (`Crossfader`: map it to a
  fader, or automate it); **CUT A / CUT B** slam to one side while held;
  **CURVE** SMOOTH (both full in the middle) or SHARP (cuts at the edges).
  MIDI notes C1-D#2 play the scene on view. Groups, FX, macros, the bank,
  snapshots and RESAMPLE are shared by both scenes.
* **RESAMPLE** (0.12, the 7th top-rail key): **REC** records what you hear
  (**SOURCE** MASTER) or one group for **LENGTH** 1, 2, 4 or 8 bars, from
  the next bar line (**START** BAR) or straight away, onto the selected pad:
  saved as a 24-bit WAV in *Music / Twisted Breaks / Resamples* (so the set
  reloads it), set up as a tempo-synced loop (**AS LOOP**) and sliced
  (**AUTO SLICE**). Resample a mangled break, then chop it again. REC is
  also the MIDI-mappable `Resample` parameter.
* **CHARACTER** (same view, per pad): vintage sampler modes, **CLEAN**,
  **SP-1200** (26 kHz, 12 bit, no interpolation), **MPC60** (40 kHz, 12 bit,
  soft low-pass), **S950** (31 kHz, 12 bit, darker low-pass), **LO-FI**
  (11 kHz, 8 bit). Like the real machines, pitching a pad down makes it
  grittier. **ALL PADS** copies a pad's character to the whole kit.
* **MACROS** (0.11, the **MACRO** tab of the PERFORMANCE FX panel): eight
  one-key moves, each several effects at once with their own settings:
  **DESTROY** (hard drive, crushed bits, stepped sample-rate), **AMEN
  MANGLE** (beat-repeat chops gated on the grid), **REWIND** (the last beat
  backwards through tape), **DARKEN** (low-pass, warm tape, dark room),
  **TAPE STOP** (slows to a halt over 1/2 bar or a bar), **PITCH DIVE**
  (fast drop into a 1/4 delay), **JUNGLE ROLL** (1/16-1/32 stutter roll,
  lifted) and **SCI-FI** (resonator, 3/16 echo, resonant band-pass). Hold a
  key (or **LATCH** to toggle); **AMOUNT** goes from mild to hard and can be
  ridden while held; **GROUP** picks A-D or ALL. Let go and your own effects
  are back exactly as you had them. MIDI notes **E4-B4** play macros 1-8,
  and each is an automatable `Macro ...` parameter.
* **MUTATE** (0.10, the tab under STEP in the SEQUENCER view): one press
  (**GO**, or MIDI-map the `Mutate` parameter) makes a variation of the
  patterns that keeps the groove: hits on the beat never move, nothing goes
  off the grid, new hits are quiet ghosts beside existing ones, rolls and
  FX throws favour the end of the bar. Choose what may change (**SLICE**,
  **PITCH**, **PATT**ern, **RAT**chet, **REV**, **FX**, **CHNC**), how much
  (**AMOUNT**), **SCOPE** (selected pad or all, main lanes and SLICE SEQ
  slots), when it lands (**AT** now / next beat / next bar, exactly on the
  boundary) or **AUTO** every 1, 2, 4 or 8 bars. Each press is a fresh
  variation of your pattern (**DRIFT** builds on the last one instead);
  **UNDO** flips back and forth, **RESET** returns your pattern. Editing
  by hand makes your edit the new original.
* **Slice sequencer** (pad editor **SEQ** tab): rearrange a sliced break.
  Switch **SLICE SEQ** on and the pad plays its own pattern of slices when
  hit (hit again to stop). Untouched, the pattern replays the break exactly
  as recorded, at any tempo. On the screen: 32 steps, each a slice, a
  REST or a TIE (let it ring); drag a step up / down to pick its slice and
  set REV, PITCH, VEL, gate LEN, RATCHET, CHANCE and MICRO for the selected
  step. SHUFFLE (beats stay put), RANDOM, REVERSE, HALF, ROTATE, RESET, hold
  FILL for rolls, STEP REC to write slices by tapping SPREAD pads. 8 SLOTs
  per pad (automatable, switch at the pattern start), LEN, RATE (1/8 ...
  1/16T), SWING. Works with SWAP groups (hand-off on the next step), choke,
  groups and FX.
* **SNAPSHOTS** (top rail): 8 slots. **STORE** then click a slot to capture
  every pad (sample, slices, all settings), the mixer, every effect and
  both sequencers; click a slot to recall it **NOW**, on the **NEXT BEAT** or
  **NEXT BAR**, exactly on the boundary sample. Also MIDI Program Change
  1-8 and the automatable `Snapshot` parameter (write snapshot changes into
  your arrangement). The master section stays as it is.
* **Break bank** (same view): 8 breaks. Pads with **FOLLOW BANK** (SWAP page,
  or ALL SLICE PADS FOLLOW) play the active break; switch it (click, or
  automate `Break Bank`) and every following pad - and every slice
  sequence - changes break on the next beat / bar.
* **FIND MISSING**: point it at a folder and every missing sample is found
  (sub-folders too) and reloaded with its slices.
* Triggered by mouse (click higher = harder) or MIDI notes **36–51**
  (C1–D#2, an Ableton Drum Rack layout), sample-accurately.
* Host **BPM, bar.beat.16th, time signature, play state** and a **32-step
  playhead** from Ableton.
* Master **level**, zero-latency **limiter**, needle **peak meter**, **PANIC**
  (red STOP button, automatable `Panic` parameter, or MIDI CC 120).
* Saved in the Live Set: all parameters, sample paths, slice markers (and
  the RESET point), slicer settings and the MIDI note map;
  changes made in the plugin window are reported to Live, so save,
  duplicate-track and undo all pick them up (AU and VST3).
* Resizable window (75 %-150 %).

## Build it yourself (optional)

One-time setup:

```bash
xcode-select --install          # Apple command-line tools (clang, git)
brew install cmake              # or install CMake from cmake.org
```

Build (JUCE 8.0.4 is downloaded automatically on first configure):

```bash
git clone https://github.com/TwistedGenetics/twisted-breaks.git
cd twisted-breaks
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DTB_COPY_AFTER_BUILD=ON
cmake --build build --config Release -j 8
```

`-DTB_COPY_AFTER_BUILD=ON` installs the plugins straight into your user
plug-in folders. Builds are universal (Apple Silicon + Intel).

Run the automated tests:

```bash
./build/TwistedBreaksTests_artefacts/Release/TwistedBreaksTests
```

Prefer Xcode? `cmake -B build-xcode -S . -G Xcode` and open
`build-xcode/TwistedBreaks.xcodeproj`.

Use a local JUCE checkout instead of downloading:
`cmake -B build -S . -DTB_JUCE_PATH=/path/to/JUCE`

### Where the binaries appear

| Format | Build output | Installed to (with `TB_COPY_AFTER_BUILD`) |
|---|---|---|
| VST3 | `build/TwistedBreaks_artefacts/Release/VST3/TwistedBreaks.vst3` | `~/Library/Audio/Plug-Ins/VST3/` |
| AU | `build/TwistedBreaks_artefacts/Release/AU/TwistedBreaks.component` | `~/Library/Audio/Plug-Ins/Components/` |

### Automatic builds (GitHub Actions)

Every push builds universal VST3 + AU on a macOS runner, runs the test suite,
validates the AU with `auval` and the VST3 with `pluginval`, and uploads
**TwistedBreaks-macOS.zip** as a workflow artifact (Actions tab → latest run →
Artifacts).

### Signing notes

* Plugins you build yourself run as-is — the linker ad-hoc signs them.
* Plugins **downloaded** from the Actions artifact are quarantined by macOS.
  After copying them into the plug-in folders, clear it:
  ```bash
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/TwistedBreaks.vst3
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/TwistedBreaks.component
  ```
* Distributing to other people later needs an Apple Developer ID certificate
  plus notarisation (`codesign --options runtime` + `xcrun notarytool`).

## Load it in Ableton Live

1. Settings → Plug-Ins → turn on **Use Audio Units v2** and **Use VST3 Plug-in
   System Folders** → **Rescan**.
2. Browser → Plug-Ins → *Twisted Genetics* → **TwistedBreaks** (it's an
   instrument). Drop it on a MIDI track.
3. If the AU doesn't appear after rebuilding: `killall -9 AudioComponentRegistrar`
   and rescan.

Full test plan: [docs/ABLETON_TEST_CHECKLIST.md](docs/ABLETON_TEST_CHECKLIST.md).

## Project layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the architecture,
threading model, host-sync strategy, voice design, slicing, FX, state format,
Ableton pitfalls and the phase roadmap.

```
Source/Core     parameters, transport sync, lock-free UI⇄audio bridge
Source/Engine   all real-time DSP
Source/UI       the control-room skin and components
Tests           headless test runner (+ editor snapshot renderer)
```
