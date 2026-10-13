# TWISTED BREAKS

A jungle / drum & bass performance sampler, break slicer, sequencer and
multi-FX instrument for Ableton Live — built in C++ with JUCE 8 as a real
**VST3** and **Audio Unit** plugin.

Skinned as a forty-year-old Soviet power-plant control console (Russian
in the header, English labels everywhere else): oxidised
enamel, rust, bakelite label plates, amber annunciator pads, a green-phosphor
CRT and a red emergency-stop panic button.

![Phase 1 console](docs/screenshots/phase1.png)

## Status

| Phase | What | State |
|---|---|---|
| 1 | Plugin shell, MIDI, host transport, master section, skin | **done** — 40/40 automated checks |
| 2 | One fully working sample pad | next |
| 3–8 | 16 pads, slicing, groups, FX, sequencer, snapshots | planned — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |

In Phase 1 each pad plays a built-in **calibration voice** (row 1 sub kicks,
row 2 snares, row 3 hats, row 4 ticks) so the whole signal path can be tested
in Ableton before sample loading exists. Sections that aren't built yet are
visibly marked offline on the panel (dark lamps + taped-on tags).

### What works now
* 16 pads, triggered by mouse (click higher = harder) or MIDI notes **36–51**
  (C1–D#2, i.e. an Ableton Drum Rack layout)
* Sample-accurate MIDI timing, retrigger crossfades, click-free voice stops
* Host **BPM, bar.beat.16th, time signature, play state** and a **32-step
  playhead** read from Ableton
* Master **level** (automatable), zero-latency **limiter** (-0.3 dBFS ceiling),
  moving-needle **peak meter** with limit/clip lamps, output **oscilloscope**
* **PANIC** (red STOP button) — button, automatable `Panic` parameter, or MIDI CC 120
* State (parameters + MIDI note map) saved in the Live Set
* Resizable window (75 %–150 %)

## Build on macOS

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
