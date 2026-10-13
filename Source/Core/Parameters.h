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
    inline constexpr int kV4 = 4;   // parameters introduced in 0.4.x
    inline constexpr int kV5 = 5;   // parameters introduced in 0.4.1 (SWAP groups)
    inline constexpr int kV6 = 6;   // parameters introduced in 0.5.x (group buses)
    inline constexpr int kV7 = 7;   // parameters introduced in 0.5.2 (fixed velocity)
    inline constexpr int kV8 = 8;   // parameters introduced in 0.6.x (performance FX)
    inline constexpr int kV9 = 9;   // parameters introduced in 0.7.x (sequencer)

    // ---- Sequencer (Phase 7) ---------------------------------------------------
    inline constexpr const char* seqRun    = "seq_run";      // bool: pattern plays with Live's transport
    inline constexpr const char* seqLength = "seq_length";   // int 1..32 steps
    inline constexpr const char* seqRes    = "seq_res";      // choice: 1/8, 1/16, 1/32 per step
    inline constexpr const char* seqQuant  = "seq_quant";    // choice: live-record quantise 1/4, 1/8, 1/16, 1/32
    inline constexpr double seqResPpq[]   = { 0.5, 0.25, 0.125 };
    inline constexpr double seqQuantPpq[] = { 1.0, 0.5, 0.25, 0.125 };

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

    // ---- Per pad (Phase 4) ---------------------------------------------------
    inline constexpr const char* padSlice   = "slice";     // int 1..16: slice played in SLICE mode

    // ---- Per pad (0.4.1: SWAP groups) ---------------------------------------
    inline constexpr const char* padSwap    = "swap";      // choice: Off / 1..4
    inline constexpr const char* padSwapAt  = "swapq";     // choice: Slice / 1/16 / Beat / Bar

    // ---- Per pad (Phase 5) ---------------------------------------------------
    inline constexpr const char* padGroup   = "group";     // choice: A / B / C / D

    // ---- Per pad (0.5.2) -------------------------------------------------------
    inline constexpr const char* padFixVel  = "fixvel";    // bool: every hit at full level

    // ---- Per pad (0.7.1: slice sequencer) ------------------------------------
    inline constexpr const char* padSliceSeq  = "sseq";    // bool: a hit starts / stops the pad's slice sequence
    inline constexpr const char* padSliceSlot = "sslot";   // int 1..8: pattern slot (switches at the pattern start)

    // ---- Group buses (Phase 5): "grp_a_level" ... "grp_d_bypass" --------------
    inline constexpr const char* grpLevel   = "level";     // dB
    inline constexpr const char* grpPan     = "pan";       // -1 .. +1
    inline constexpr const char* grpMute    = "mute";      // bool
    inline constexpr const char* grpSolo    = "solo";      // bool
    inline constexpr const char* grpBypass  = "bypass";    // bool: skip the group's FX chain

    inline constexpr float grpLevelMinDb = -60.0f;
    inline constexpr float grpLevelMaxDb =   6.0f;

    enum PadMode { modeOneShot = 0, modeLoop = 1, modeSlice = 2 };
    enum SwapAt  { swapAtSlice = 0, swapAtSixteenth = 1, swapAtBeat = 2, swapAtBar = 3 };
    inline constexpr float loopRates[] = { 0.25f, 0.5f, 1.0f, 2.0f };

    inline constexpr float padGainMinDb  = -48.0f;
    inline constexpr float padGainMaxDb  =  12.0f;
    inline constexpr float padPitchRange =  12.0f;     // ± semitones

    /** "pad01_gain" etc. pad is 0-based. */
    inline juce::String padId (int pad, const char* name)
    {
        return "pad" + juce::String (pad + 1).paddedLeft ('0', 2) + "_" + name;
    }

    /** "grp_a_level" etc. group is 0-based (0 = A). */
    inline juce::String groupId (int group, const char* name)
    {
        return juce::String ("grp_") + juce::String::charToString ((juce::juce_wchar) ('a' + group)) + "_" + name;
    }

    /** Builds the full parameter layout for the AudioProcessorValueTreeState. */
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
