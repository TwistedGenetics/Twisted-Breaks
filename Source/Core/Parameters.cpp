#include "Core/Parameters.h"
#include "Core/MacroDefs.h"
#include "Core/EngineBridge.h"
#include "Core/FxDefs.h"

namespace tb::params
{
    namespace
    {
        using namespace juce;

        String dbText (float v, float minDb)
        {
            return v <= minDb + 0.01f ? String ("-inf dB") : String (v, 1) + " dB";
        }

        String panText (float v)
        {
            const int amount = roundToInt (std::abs (v) * 100.0f);
            if (amount == 0) return "C";
            return (v < 0.0f ? "L" : "R") + String (amount);
        }

        float panFromText (const String& t)
        {
            const auto s = t.trim().toUpperCase();
            if (s.startsWith ("L")) return -s.substring (1).getFloatValue() / 100.0f;
            if (s.startsWith ("R")) return  s.substring (1).getFloatValue() / 100.0f;
            return 0.0f;
        }

        void addPadParameters (AudioProcessorValueTreeState::ParameterLayout& layout, int pad)
        {
            const String prefix = "Pad " + padLabel (pad) + " ";      // "Pad 01 ..." / "Pad B01 ..."
            // Scene B's pads (17-32) are all new in 0.13: their own version hint keeps
            // the AU parameter order of older sets.
            const auto vh = [pad] (int v) { return pad >= kPadsPerScene ? kV15 : v; };

            NormalisableRange<float> gainRange (padGainMinDb, padGainMaxDb, 0.01f);
            gainRange.setSkewForCentre (-9.0f);
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padGain), vh (kV2) }, prefix + "Gain", gainRange, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return dbText (v, padGainMinDb); })
                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padPan), vh (kV2) }, prefix + "Pan",
                NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return panText (v); })
                    .withValueFromStringFunction (panFromText)));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padPitch), vh (kV2) }, prefix + "Pitch",
                NormalisableRange<float> (-padPitchRange, padPitchRange, 0.01f), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        return (v >= 0.0f ? "+" : "") + String (v, 2) + " st";
                    })
                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));

            auto percentAttrs = AudioParameterFloatAttributes()
                .withStringFromValueFunction ([] (float v, int) { return String (v * 100.0f, 1) + " %"; })
                .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue() / 100.0f; });

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padStart), vh (kV2) }, prefix + "Start",
                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentAttrs));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padEnd), vh (kV2) }, prefix + "End",
                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentAttrs));

            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padReverse), vh (kV2) }, prefix + "Reverse", false));

            // ---- Phase 3 ----
            // NOTE: choice lists are part of the saved/automation format. Never
            // reorder or remove entries; only append.
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padMode), vh (kV3) }, prefix + "Mode",
                StringArray { "One-shot", "Loop", "Slice", "Seq One", "Seq Loop", "Loop Cont" }, modeOneShot));

            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padSync), vh (kV3) }, prefix + "Loop Sync", true));

            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padRate), vh (kV3) }, prefix + "Loop Rate",
                StringArray { "0.25x", "0.5x", "1x", "2x" }, 2));

            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padChoke), vh (kV3) }, prefix + "Choke",
                StringArray { "Off", "1", "2", "3", "4" }, 0));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padVelSens), vh (kV3) }, prefix + "Velocity Sens",
                NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; })
                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue() / 100.0f; })));

            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padMute), vh (kV3) }, prefix + "Mute", false));
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padSolo), vh (kV3) }, prefix + "Solo", false));

            // ---- Phase 4 ----
            layout.add (std::make_unique<AudioParameterInt> (
                ParameterID { padId (pad, padSlice), vh (kV4) }, prefix + "Slice", 1, 16, 1));

            // ---- 0.4.1: SWAP groups (slice-point hand-off between breaks) ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padSwap), vh (kV5) }, prefix + "Swap Group",
                StringArray { "Off", "1", "2", "3", "4" }, 0));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padSwapAt), vh (kV5) }, prefix + "Swap At",
                StringArray { "Slice", "1/16", "Beat", "Bar" }, swapAtSlice));

            // ---- Phase 5: output group ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padGroup), vh (kV6) }, prefix + "Group",
                StringArray { "A", "B", "C", "D" }, 0));

            // ---- 0.5.2: fixed velocity (MPC "full level") ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padFixVel), vh (kV7) }, prefix + "Fixed Velocity", false));

            // ---- 0.7.1: slice sequencer ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padSliceSeq), vh (kV9) }, prefix + "Slice Seq", false));
            layout.add (std::make_unique<AudioParameterInt> (
                ParameterID { padId (pad, padSliceSlot), vh (kV9) }, prefix + "Slice Seq Slot", 1, 8, 1));

            // ---- 0.8: break bank ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padFollowBank), vh (kV10) }, prefix + "Follow Bank", false));

            // ---- 0.12: vintage character ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padCharacter), vh (kV14) }, prefix + "Character",
                StringArray { "Clean", "SP-1200", "MPC60", "S950", "Lo-Fi" }, 0));

            // ---- 0.14: sequence pads (A6) ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padLaunch), kV16 }, prefix + "Launch",
                StringArray { "Now", "1/16", "Beat", "Bar" }, launchBeat));
            layout.add (std::make_unique<AudioParameterInt> (
                ParameterID { padId (pad, padSeqLen), kV16 }, prefix + "Seq Length", 0, 32, 0,
                AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v == 0 ? String ("GLOBAL") : String (v); })));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padSwapFrom), kV16 }, prefix + "Swap From",
                StringArray { "Same Step", "Step 1" }, 0));

            // ---- 0.15: Push 3 pad pressure (A7) ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padPress), kV17 }, prefix + "Pressure",
                StringArray (kPressNames, 5), pressFilter));

            // ---- 0.18: whole-pad steps only (A10) ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padStepsWhole), kV18 }, prefix + "Whole Steps", false));

            // ---- 0.21: fit to tempo, layers (A11) ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padFit), kV19 }, prefix + "Fit To Tempo", false));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padLayers), kV19 }, prefix + "Layers",
                StringArray (kLayerModeNames, 4), layersOff));

            // ---- 0.23: STRETCH (A12) ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padStretch), kV21 }, prefix + "Stretch", StringArray (kStretchNames, 6), stretchOff));
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padOrigBpm), kV21 }, prefix + "Original BPM", NormalisableRange<float> (0.0f, 300.0f, 0.01f), 0.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 1.0f ? String ("Auto") : String (v, 2) + " BPM"; })));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padGap), kV21 }, prefix + "Stretch Gaps", StringArray (kGapNames, 3), 1));
            NormalisableRange<float> grainRange (5.0f, 200.0f, 0.1f);
            grainRange.setSkewForCentre (30.0f);
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padGrain), kV21 }, prefix + "Grain / Cycle", grainRange, 30.0f,
                AudioParameterFloatAttributes().withLabel ("ms")));
        }
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        // Master output level. Skewed so the useful -20..+6 dB region gets most
        // of the knob travel.
        NormalisableRange<float> gainRange (masterGainMinDb, masterGainMaxDb, 0.01f);
        gainRange.setSkewForCentre (-12.0f);

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { masterGain, kV1 }, "Master Level", gainRange, 0.0f,
            AudioParameterFloatAttributes()
                .withLabel ("dB")
                .withStringFromValueFunction ([] (float v, int)
                {
                    return v <= masterGainMinDb + 0.01f ? String ("-inf")
                                                        : String (v, 1);
                })));

        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { masterLimiter, kV1 }, "Master Limiter", true));

        // Exposed so a performer can map PANIC to a MIDI controller or clip
        // automation in Ableton. Any off->on transition stops everything.
        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { masterPanic, kV1 }, "Panic", false));

        for (int p = 0; p < kNumPads; ++p)
            addPadParameters (layout, p);

        // ---- Phase 5: group buses A..D ----
        for (int g = 0; g < kNumGroups; ++g)
        {
            const String name = "Group " + String::charToString ((juce_wchar) ('A' + g)) + " ";

            NormalisableRange<float> lvl (grpLevelMinDb, grpLevelMaxDb, 0.01f);
            lvl.setSkewForCentre (-12.0f);
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { groupId (g, grpLevel), kV6 }, name + "Level", lvl, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return dbText (v, grpLevelMinDb); })
                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { groupId (g, grpPan), kV6 }, name + "Pan",
                NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return panText (v); })
                    .withValueFromStringFunction (panFromText)));

            layout.add (std::make_unique<AudioParameterBool> (ParameterID { groupId (g, grpMute),   kV6 }, name + "Mute", false));
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { groupId (g, grpSolo),   kV6 }, name + "Solo", false));
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { groupId (g, grpBypass), kV6 }, name + "FX Bypass", false));
        }

        // ---- Phase 7: sequencer ----
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { seqRun, kV9 }, "Sequencer Run", true));
        layout.add (std::make_unique<AudioParameterInt>  (ParameterID { seqLength, kV9 }, "Sequencer Length", 1, 32, 16));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { seqRes, kV9 }, "Sequencer Step",
                                                            StringArray { "1/8", "1/16", "1/32" }, 1));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { seqQuant, kV9 }, "Record Quantise",
                                                            StringArray { "1/4", "1/8", "1/16", "1/32" }, 2));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { seqFill, kV11 }, "Sequencer Fill", false));

        // ---- Phase 13: scenes + crossfader (pads 17-32 come from the pad loop) ----
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { xfade, kV15 }, "Crossfader",
            NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
            {
                return std::abs (v - 0.5f) < 0.002f ? String ("A=B") : (v < 0.5f ? "A " + String (roundToInt (100 - 2 * (0.5f - v) * 100)) : "B " + String (roundToInt (100 - 2 * (v - 0.5f) * 100)));
            })));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { xfadeCurve, kV15 }, "Crossfader Curve",
                                                            StringArray { "Smooth", "Sharp" }, curveSmooth));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { sceneView, kV15 }, "Scene View",
                                                            StringArray { "A", "B" }, 0));

        // ---- Phase 12: RESAMPLE ----
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { rsmpSource, kV14 }, "Resample Source",
                                                            StringArray { "Master", "Group A", "Group B", "Group C", "Group D" }, 0));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { rsmpBars, kV14 }, "Resample Length",
                                                            StringArray { "1 Bar", "2 Bars", "4 Bars", "8 Bars" }, 1));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { rsmpStart, kV14 }, "Resample Start",
                                                            StringArray { "Now", "Next Bar" }, 1));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { rsmpLoop, kV14 }, "Resample As Loop", true));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { rsmpSlice, kV14 }, "Resample Auto Slice", true));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { rsmpRec, kV14 }, "Resample", false));

        // ---- Phase 11: performance macros ----
        for (int m = 0; m < macro::kNumMacros; ++m)
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { macro::paramId (m), kV13 }, String ("Macro ") + macro::def (m).name, false));
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { macroAmount, kV13 }, "Macro Amount", 0, 100, 60,
            AudioParameterIntAttributes().withLabel ("%")));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { macroGroup, kV13 }, "Macro Group",
                                                            StringArray { "A", "B", "C", "D", "All" }, 4));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { macroLatch, kV13 }, "Macro Latch", false));

        // ---- Phase 10: MUTATE ----
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { mutAmount, kV12 }, "Mutate Amount", 0, 100, 35,
            AudioParameterIntAttributes().withLabel ("%")));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { mutScope, kV12 }, "Mutate Scope",
                                                            StringArray { "Selected Pad", "All Pads" }, 1));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { mutAuto, kV12 }, "Mutate Auto",
                                                            StringArray { "Off", "1 Bar", "2 Bars", "4 Bars", "8 Bars" }, 0));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { mutAt, kV12 }, "Mutate At",
                                                            StringArray { "Now", "Next Beat", "Next Bar" }, switchBar));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { mutDrift, kV12 }, "Mutate Drift", false));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { mutGo, kV12 }, "Mutate", false));
        for (int t = 0; t < kNumMutTargets; ++t)
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { mutTargetId (t), kV12 },
                                                              String ("Mutate ") + mutTargetNames[t], mutTargetDefault[t]));

        // ---- Phase 8: snapshots + break bank ----
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { snapRecall, kV10 }, "Snapshot", 0, 8, 0,
            AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v == 0 ? String ("-") : String (v); })));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { snapAt, kV10 }, "Snapshot Switch",
                                                            StringArray { "Now", "Next Beat", "Next Bar" }, switchBar));
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { bankActive, kV10 }, "Break Bank", 1, 8, 1));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bankAt, kV10 }, "Break Bank Switch",
                                                            StringArray { "Now", "Next Beat", "Next Bar" }, switchBeat));

        // ---- Phase 6: performance FX, per group: on / off + settings ----
        for (int g = 0; g < kNumGroups; ++g)
        {
            const String gname = "Group " + String::charToString ((juce_wchar) ('A' + g)) + " ";
            for (int f = 0; f < fx::kNumFx; ++f)
            {
                const auto& d = fx::def (f);
                const int vh = f == fx::akaizer ? kV22 : kV8;          // AKAIZER took STUTTER's place in 0.25
                layout.add (std::make_unique<AudioParameterBool> (ParameterID { fx::paramId (g, f), vh },
                                                                  gname + "FX " + d.label, false));
                for (int k = 0; k < d.numParams; ++k)
                {
                    const auto& pd = d.params[k];
                    const String pname = gname + "FX " + d.label + " " + String (pd.label).toLowerCase();
                    if (pd.isChoice())
                    {
                        StringArray items;
                        for (int c = 0; c < pd.numChoices; ++c) items.add (pd.choices[c]);
                        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { fx::paramId (g, f, k), vh },
                                                                            pname, items, roundToInt (pd.def)));
                    }
                    else
                    {
                        layout.add (std::make_unique<AudioParameterFloat> (
                            ParameterID { fx::paramId (g, f, k), vh }, pname,
                            NormalisableRange<float> (0.0f, 1.0f, 0.001f), pd.def,
                            AudioParameterFloatAttributes()
                                .withStringFromValueFunction ([&pd] (float v, int) { return fx::valueText (pd, v); })));
                    }
                }
            }
        }

        // ---- 0.25: STUTTER's old parameters stay (sets that automated them still open); they do nothing now.
        for (int g = 0; g < kNumGroups; ++g)
        {
            const auto gk = String ("grp_") + String::charToString ((juce_wchar) ('a' + g)) + "_stutter";
            const String gname = "Group " + String::charToString ((juce_wchar) ('A' + g)) + " ";
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { gk, kV8 }, gname + "FX STUTTER (old)", false));
            layout.add (std::make_unique<AudioParameterChoice> (ParameterID { gk + "_size", kV8 }, gname + "FX STUTTER size (old)",
                                                                StringArray (fx::kSizes, 5), 3));
        }

        // ---- 0.21: TWIST (A11) ----
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { twistAmount, kV19 }, "Twist Amount", 0, 100, 50,
                                                         AudioParameterIntAttributes().withLabel ("%")));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { twistGo, kV19 }, "Twist", false));
        for (int t = 0; t < kNumTwistTargets; ++t)
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { twistLockId (t), kV19 },
                                                              String ("Twist Lock ") + twistTargetNames[t], false));

        // ---- 0.22: song mode (A11) ----
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { songOn, kV20 }, "Song", false));

        return layout;
    }
}
