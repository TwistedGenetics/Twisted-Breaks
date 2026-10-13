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
    *state*, not parameters, and live in the ValueTree (see StateManager in
    later phases) so Ableton's automation list stays usable.
*/
namespace tb::params
{
    // ---- Version hints -----------------------------------------------------
    inline constexpr int kV1 = 1;   // parameters introduced in 0.1.x

    // ---- Master section (Phase 1) ------------------------------------------
    inline constexpr const char* masterGain    = "master_gain";     // dB
    inline constexpr const char* masterLimiter = "master_limiter";  // bool
    inline constexpr const char* masterPanic   = "master_panic";    // bool, rising edge = panic

    // Range constants shared by the processor and the UI.
    inline constexpr float masterGainMinDb = -60.0f;
    inline constexpr float masterGainMaxDb =   6.0f;

    /** Builds the full parameter layout for the AudioProcessorValueTreeState. */
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
