#include "Core/Parameters.h"
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
            const String prefix = "Pad " + String (pad + 1).paddedLeft ('0', 2) + " ";

            NormalisableRange<float> gainRange (padGainMinDb, padGainMaxDb, 0.01f);
            gainRange.setSkewForCentre (-9.0f);
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padGain), kV2 }, prefix + "Gain", gainRange, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return dbText (v, padGainMinDb); })
                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padPan), kV2 }, prefix + "Pan",
                NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return panText (v); })
                    .withValueFromStringFunction (panFromText)));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padPitch), kV2 }, prefix + "Pitch",
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
                ParameterID { padId (pad, padStart), kV2 }, prefix + "Start",
                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentAttrs));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padEnd), kV2 }, prefix + "End",
                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentAttrs));

            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padReverse), kV2 }, prefix + "Reverse", false));

            // ---- Phase 3 ----
            // NOTE: choice lists are part of the saved/automation format. Never
            // reorder or remove entries; only append.
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padMode), kV3 }, prefix + "Mode",
                StringArray { "One-shot", "Loop", "Slice" }, modeOneShot));

            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padSync), kV3 }, prefix + "Loop Sync", true));

            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padRate), kV3 }, prefix + "Loop Rate",
                StringArray { "0.25x", "0.5x", "1x", "2x" }, 2));

            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padChoke), kV3 }, prefix + "Choke",
                StringArray { "Off", "1", "2", "3", "4" }, 0));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { padId (pad, padVelSens), kV3 }, prefix + "Velocity Sens",
                NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; })
                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue() / 100.0f; })));

            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padMute), kV3 }, prefix + "Mute", false));
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padSolo), kV3 }, prefix + "Solo", false));

            // ---- Phase 4 ----
            layout.add (std::make_unique<AudioParameterInt> (
                ParameterID { padId (pad, padSlice), kV4 }, prefix + "Slice", 1, 16, 1));

            // ---- 0.4.1: SWAP groups (slice-point hand-off between breaks) ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padSwap), kV5 }, prefix + "Swap Group",
                StringArray { "Off", "1", "2", "3", "4" }, 0));
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padSwapAt), kV5 }, prefix + "Swap At",
                StringArray { "Slice", "1/16", "Beat", "Bar" }, swapAtSlice));

            // ---- Phase 5: output group ----
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { padId (pad, padGroup), kV6 }, prefix + "Group",
                StringArray { "A", "B", "C", "D" }, 0));

            // ---- 0.5.2: fixed velocity (MPC "full level") ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padFixVel), kV7 }, prefix + "Fixed Velocity", false));

            // ---- 0.7.1: slice sequencer ----
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { padId (pad, padSliceSeq), kV9 }, prefix + "Slice Seq", false));
            layout.add (std::make_unique<AudioParameterInt> (
                ParameterID { padId (pad, padSliceSlot), kV9 }, prefix + "Slice Seq Slot", 1, 8, 1));
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

        // ---- Phase 6: performance FX, per group: on / off + settings ----
        for (int g = 0; g < kNumGroups; ++g)
        {
            const String gname = "Group " + String::charToString ((juce_wchar) ('A' + g)) + " ";
            for (int f = 0; f < fx::kNumFx; ++f)
            {
                const auto& d = fx::def (f);
                layout.add (std::make_unique<AudioParameterBool> (ParameterID { fx::paramId (g, f), kV8 },
                                                                  gname + "FX " + d.label, false));
                for (int k = 0; k < d.numParams; ++k)
                {
                    const auto& pd = d.params[k];
                    const String pname = gname + "FX " + d.label + " " + String (pd.label).toLowerCase();
                    if (pd.isChoice())
                    {
                        StringArray items;
                        for (int c = 0; c < pd.numChoices; ++c) items.add (pd.choices[c]);
                        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { fx::paramId (g, f, k), kV8 },
                                                                            pname, items, roundToInt (pd.def)));
                    }
                    else
                    {
                        layout.add (std::make_unique<AudioParameterFloat> (
                            ParameterID { fx::paramId (g, f, k), kV8 }, pname,
                            NormalisableRange<float> (0.0f, 1.0f, 0.001f), pd.def,
                            AudioParameterFloatAttributes()
                                .withStringFromValueFunction ([&pd] (float v, int) { return fx::valueText (pd, v); })));
                    }
                }
            }
        }

        return layout;
    }
}
