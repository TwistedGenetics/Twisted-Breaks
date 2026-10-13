#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Core/TransportSync.h"

namespace tb::ui
{
    /*
        TransportDisplay
        ----------------
        Host-sync readouts styled as segment-tube indicators:
            BPM  |  BAR . BEAT . 16th  |  run / host lamps
        plus a 32-lamp step row that tracks Ableton's playhead in 16ths —
        the same grid the Phase 7 sequencer will run on.
    */
    class TransportDisplay final : public juce::Component
    {
    public:
        explicit TransportDisplay (const TransportUiState& state);

        void refresh();          // called by MainView's timer
        void paint (juce::Graphics&) override;

    private:
        const TransportUiState& ui;

        bool   host = false, playing = false;
        double bpm  = 120.0, ppq = 0.0;
        int    bar = 0, beat = 0, step = 0, num = 4, den = 4;
    };
}
