#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/*
    Parameters
    ----------
    Every host-automatable parameter is declared here, and ONLY here.

    STABLE ID RULES (do not break these once a version ships):
      * IDs are lower_snake_case strings and never change once released.
        Ableton stores automation against these IDs inside .als files.
      * Never reuse an ID for a different meaning; deprecate instead.
      * The integer passed to juce::ParameterID is the "version hint" (the
        plugin version in which that parameter first appeared). AU needs it
        to keep parameter ordering stable when new parameters are added.
      * Per-pad IDs follow  pad{NN}_{name}   e.g. pad01_gain  (NN = 01..16)
        Per-group IDs follow grp{X}_{name}   e.g. grpa_level  (X = a..d)
        Per-group FX IDs follow grp{X}_fx{NN}_{name}

    Only the parameters a performer would genuinely automate are exposed.
    Things like slice marker positions, sample paths and sequencer steps are
    *state*, not parameters, and live in the ValueTree so Ableton's
    automation list stays usable.
*/
namespace tb::params
{
    // ---- Version hints -----------------------------------------------------
    inline constexpr int kV1 = 1;   // parameters introduced in 0.1.x
    inline constexpr int kV2 = 2;   // parameters introduced in 0.2.x
    inline constexpr int kV3 = 3;   // parameters introduced in 0.3.x

    // ---- Master section (Phase 1) ------------------------------------------
    inline constexpr const char* masterGain    = "master_gain";     // dB
    inline constexpr const char* masterLimiter = "master_limiter";  // bool
    inline constexpr const char* masterPanic   = "master_panic";    // bool, rising edge = panic

    inline constexpr float masterGainMinDb = -60.0f;
    inline constexpr float masterGainMaxDb =   6.0f;

    // ---- Per pad (Phase 2) ---------------------------------------------------
    inline constexpr const char* padGain    = "gain";      // dB
    inline constexpr const char* padPan     = "pan";       // -1 .. +1
    inline constexpr const char* padPitch   = "pitch";     // semitones
    inline constexpr const char* padStart   = "start";     // 0 .. 1 of the sample
    inline constexpr const char* padEnd     = "end";       // 0 .. 1 of the sample
    inline constexpr const char* padReverse = "reverse";   // bool

    // ---- Per pad (Phase 3) ---------------------------------------------------
    inline constexpr const char* padMode    = "mode";      // choice: One-shot / Loop / Slice
    inline constexpr const char* padSync    = "sync";      // bool: loops follow host tempo
    inline constexpr const char* padRate    = "rate";      // choice: 0.25x / 0.5x / 1x / 2x
    inline constexpr const char* padChoke   = "choke";     // choice: Off / 1..4
    inline constexpr const char* padVelSens = "velsens";   // 0..1
    inline constexpr const char* padMute    = "mute";      // bool
    inline constexpr const char* padSolo    = "solo";      // bool

    enum PadMode { modeOneShot = 0, modeLoop = 1, modeSlice = 2 };
    inline constexpr float loopRates[] = { 0.25f, 0.5f, 1.0f, 2.0f };

    inline constexpr float padGainMinDb  = -48.0f;
    inline constexpr float padGainMaxDb  =  12.0f;
    inline constexpr float padPitchRange =  12.0f;     // ± semitones

    /** "pad01_gain" etc. pad is 0-based. */
    inline juce::String padId (int pad, const char* name)
    {
        return "pad" + juce::String (pad + 1).paddedLeft ('0', 2) + "_" + name;
    }

    /** Builds the full parameter layout for the AudioProcessorValueTreeState. */
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
