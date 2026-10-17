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
    inline constexpr int kV10 = 10; // parameters introduced in 0.8.x (snapshots, break bank)
    inline constexpr int kV11 = 11; // parameters introduced in 0.9.x (step features)
    inline constexpr int kV12 = 12; // parameters introduced in 0.10.x (MUTATE)
    inline constexpr int kV13 = 13; // parameters introduced in 0.11.x (macros)
    inline constexpr int kV14 = 14; // parameters introduced in 0.12.x (resample, character)
    inline constexpr int kV15 = 15; // parameters introduced in 0.13.x (scenes: pads 17-32, crossfader)
    inline constexpr int kV16 = 16; // parameters introduced in 0.14.x (sequence pads)
    inline constexpr int kV17 = 17; // parameters introduced in 0.15.x (Push 3 mode)
    inline constexpr int kV18 = 18; // parameters introduced in 0.18.x (whole-pad steps)
    inline constexpr int kV19 = 19; // parameters introduced in 0.21.x (TWIST, layers, fit to tempo)
    inline constexpr int kV20 = 20; // parameters introduced in 0.22.x (song mode)
    inline constexpr int kV21 = 21; // parameters introduced in 0.23.x (stretch modes)
    inline constexpr int kV22 = 22; // parameters introduced in 0.25.x (AKAIZER)

    // ---- Scenes A / B (Phase 13) ------------------------------------------------
    inline constexpr const char* xfade      = "xfade";        // float 0 (A) .. 1 (B), centre = both full
    inline constexpr const char* xfadeCurve = "xfade_curve";  // choice: Smooth / Sharp
    inline constexpr const char* sceneView  = "scene_view";   // choice: A / B (what the screen and controller show)
    enum XfadeCurve { curveSmooth = 0, curveSharp = 1 };
    /** Scene gains for a crossfader position (0..1). Smooth: both full in the middle, each fades
        out over its far half. Sharp: both full until the last 5 % (cuts, scratch style). */
    inline void xfadeGains (float x, int curve, float& a, float& b) noexcept
    {
        x = juce::jlimit (0.0f, 1.0f, x);
        if (curve == curveSharp)
        {
            a = x > 0.95f ? (1.0f - x) / 0.05f : 1.0f;
            b = x < 0.05f ? x / 0.05f : 1.0f;
        }
        else
        {
            a = juce::jmin (1.0f, 2.0f * (1.0f - x));
            b = juce::jmin (1.0f, 2.0f * x);
        }
    }

    // ---- RESAMPLE (Phase 12) ----------------------------------------------------
    inline constexpr const char* rsmpSource = "rsmp_source";  // choice: Master, Group A..D
    inline constexpr const char* rsmpBars   = "rsmp_bars";    // choice: 1, 2, 4, 8 bars
    inline constexpr const char* rsmpStart  = "rsmp_start";   // choice: Now / Next Bar
    inline constexpr const char* rsmpLoop   = "rsmp_loop";    // bool: the new pad plays as a synced loop
    inline constexpr const char* rsmpSlice  = "rsmp_slice";   // bool: AUTO SLICE the result
    inline constexpr const char* rsmpRec    = "rsmp_rec";     // bool: rising edge = REC (MIDI-mappable)
    inline constexpr int rsmpBarCounts[] = { 1, 2, 4, 8 };

    // ---- Performance macros (Phase 11): "macro_<key>" on / off per macro (MacroDefs) ----
    inline constexpr const char* macroAmount = "macro_amount";   // int 0..100 %: MILD .. HARD
    inline constexpr const char* macroGroup  = "macro_group";    // choice: A, B, C, D, All
    inline constexpr const char* macroLatch  = "macro_latch";    // bool: keys / notes toggle instead of hold

    // ---- MUTATE (Phase 10) ------------------------------------------------------
    inline constexpr const char* mutAmount  = "mut_amount";   // int 0..100 %
    inline constexpr const char* mutScope   = "mut_scope";    // choice: Selected Pad / All Pads
    inline constexpr const char* mutAuto    = "mut_auto";     // choice: Off, 1, 2, 4, 8 bars
    inline constexpr const char* mutAt      = "mut_at";       // choice: Now / Next Beat / Next Bar (MUTATE key)
    inline constexpr const char* mutDrift   = "mut_drift";    // bool: build on the last mutation instead of the original
    inline constexpr const char* mutGo      = "mut_go";       // bool: rising edge = MUTATE (MIDI-mappable)
    /** What MUTATE may change, in key order (ids "mut_t_<key>"). Append only. */
    inline constexpr const char* mutTargetKeys[]  = { "slice", "pitch", "pattern", "ratchet", "reverse", "fx", "chance" };
    inline constexpr const char* mutTargetNames[] = { "Slices", "Pitch", "Pattern", "Ratchet", "Reverse", "FX", "Chance" };
    inline constexpr const char* mutTargetKeyText[] = { "SLICE", "PITCH", "PATT", "RAT", "REV", "FX", "CHNC" };   // key captions
    inline constexpr bool        mutTargetDefault[] = { true, false, true, true, true, false, true };
    inline constexpr int kNumMutTargets = 7;
    enum MutTarget { mutTSlice = 0, mutTPitch, mutTPattern, mutTRatchet, mutTReverse, mutTFx, mutTChance };
    inline constexpr int mutAutoBars[] = { 0, 1, 2, 4, 8 };
    inline juce::String mutTargetId (int t) { return juce::String ("mut_t_") + mutTargetKeys[t]; }

    // ---- Snapshots & break bank (Phase 8) ----------------------------------------
    inline constexpr const char* snapRecall = "snapshot";     // int 0..8: 0 = none; changing it recalls that slot
    inline constexpr const char* snapAt     = "snap_at";      // choice: Now / Next Beat / Next Bar
    inline constexpr const char* bankActive = "bank_active";  // int 1..8: break the FOLLOW BANK pads play
    inline constexpr const char* bankAt     = "bank_at";      // choice: Now / Next Beat / Next Bar
    enum SwitchAt { switchNow = 0, switchBeat = 1, switchBar = 2 };

    // ---- Sequencer (Phase 7) ---------------------------------------------------
    inline constexpr const char* seqRun    = "seq_run";      // bool: pattern plays with Live's transport
    inline constexpr const char* seqLength = "seq_length";   // int 1..32 steps
    inline constexpr const char* seqRes    = "seq_res";      // choice: 1/8, 1/16, 1/32 per step
    inline constexpr const char* seqQuant  = "seq_quant";    // choice: live-record quantise 1/4, 1/8, 1/16, 1/32
    inline constexpr const char* seqFill   = "seq_fill";     // bool: FILL held (steps with FILL / !FILL conditions)
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

    // ---- Per pad (0.8: break bank) -----------------------------------------------
    inline constexpr const char* padFollowBank = "bank";   // bool: play the active bank break instead of its own sample

    // ---- Per pad (0.12: vintage sampler character) ---------------------------------
    inline constexpr const char* padCharacter = "char";    // choice: Clean / SP-1200 / MPC60 / S950 / Lo-Fi (PadVoice::character)

    // ---- Per pad (0.14: sequence pads, spec addition A6) -------------------------
    inline constexpr const char* padLaunch   = "launch";   // choice: Now / 1/16 / Beat / Bar: when a hit starts the pad's sequence
    inline constexpr const char* padSeqLen   = "seqlen";   // int 0..32: 0 = the sequencer's LEN
    inline constexpr const char* padSwapFrom = "swapfrom"; // choice: Same Step / Step 1 (a sequence taking over in a SWAP group)

    // ---- 0.15: Push 3 mode (A7) ---------------------------------------------------
    inline constexpr const char* padPress = "press";       // choice: what Push 3 pad pressure does to this pad
    enum PressTarget { pressFilter = 0, pressDrive, pressStutter, pressPitch, pressOff };   // append only

    // ---- 0.18: whole-pad steps (A10) ------------------------------------------------
    inline constexpr const char* padStepsWhole = "whole";  // bool: this pad's steps always play the whole pad (never a slice)
    inline const char* const kPressNames[] = { "Filter", "Drive", "Stutter", "Pitch", "Off" };

    // ---- 0.21: layers, fit to tempo, TWIST (A11 part 4) ---------------------------------
    inline constexpr const char* padFit    = "fit";      // bool: one-shots / slices play at Live's tempo (from the break's own BPM)
    inline constexpr const char* padLayers = "layers";   // choice: how the pad's extra samples take turns
    enum LayerMode { layersOff = 0, layersRoundRobin, layersVelocity, layersRandom };   // append only
    inline const char* const kLayerModeNames[] = { "Off", "Round Robin", "Velocity", "Random" };
    inline constexpr int kMaxLayers = 4;                 // extra samples per pad

    inline constexpr const char* twistAmount = "twist_amount";   // int 0..100 %
    inline constexpr const char* twistGo     = "twist_go";       // bool: rising edge = TWIST the selected pad (MIDI-mappable)
    inline constexpr const char* twistTargetKeys[]  = { "pitch", "start", "pan", "rev", "char", "slice", "gain" };
    inline constexpr const char* twistTargetNames[] = { "Pitch", "Start", "Pan", "Reverse", "Character", "Slice", "Gain" };
    inline constexpr int kNumTwistTargets = 7;
    inline juce::String twistLockId (int t) { return juce::String ("twist_lock_") + twistTargetKeys[t]; }

    // ---- 0.23: STRETCH (A12 part 3) -----------------------------------------------------
    inline constexpr const char* padStretch = "stretch";  // choice: how a one-shot / slice follows Live's tempo
    enum StretchMode { stretchOff = 0, stretchRepitch, stretchBeats, stretchTexture, stretchAkaiClassic, stretchAkaiRevised };   // append only
    inline const char* const kStretchNames[] = { "Off", "Repitch", "Beats", "Texture", "Akai Classic", "Akai Revised" };
    inline constexpr const char* padOrigBpm = "obpm";     // float 0 (detect) or 40..300: the break's own tempo
    inline constexpr const char* padGap     = "sgap";     // choice: BEATS gap fill
    inline const char* const kGapNames[] = { "None", "Loop", "Back & Forth" };
    inline constexpr const char* padGrain   = "grain";    // float ms: TEXTURE grain / AKAI cycle

    // ---- 0.22: song mode (A11 part 5) ---------------------------------------------------
    inline constexpr const char* songOn = "song_on";     // bool: the snapshot chain follows Live's bars
    enum Launch { launchNow = 0, launchSixteenth = 1, launchBeat = 2, launchBar = 3 };

    // ---- Group buses (Phase 5): "grp_a_level" ... "grp_d_bypass" --------------
    inline constexpr const char* grpLevel   = "level";     // dB
    inline constexpr const char* grpPan     = "pan";       // -1 .. +1
    inline constexpr const char* grpMute    = "mute";      // bool
    inline constexpr const char* grpSolo    = "solo";      // bool
    inline constexpr const char* grpBypass  = "bypass";    // bool: skip the group's FX chain

    inline constexpr float grpLevelMinDb = -60.0f;
    inline constexpr float grpLevelMaxDb =   6.0f;

    enum PadMode { modeOneShot = 0, modeLoop = 1, modeSlice = 2, modeSeqOne = 3, modeSeqLoop = 4 };   // append only
    inline constexpr bool isSeqMode (int mode) noexcept { return mode == modeSeqOne || mode == modeSeqLoop; }
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
