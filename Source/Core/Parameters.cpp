#include "Core/Parameters.h"

namespace tb::params
{
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

        return layout;
    }
}
