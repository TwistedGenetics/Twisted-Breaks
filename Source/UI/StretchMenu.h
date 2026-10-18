#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class TwistedBreaksProcessor;

namespace tb::ui::stretch
{
    /*
        STRETCH in the pad settings (A13, 0.26)
        ---------------------------------------
        One STRETCH per pad, on the PLAY page in SYNC's old place. These helpers
        read and write it (the selector writes `stretch`, `sync` and `fit`
        together, so old sets and automation keep working) and build the
        tempo menu under the readout.
    */

    /** The mode the pad plays with (params::StretchMode), sync / FIT included. */
    int effectiveMode (TwistedBreaksProcessor&, int pad);

    /** Sets STRETCH for a pad: one undo step, `sync` and `fit` follow. */
    void setMode (TwistedBreaksProcessor&, int pad, int mode);

    /** The tempo the pad's audio is taken to be at: typed in, or worked out (a LOOP pad from
        its length, a one-shot / slice break from its markers). 0 if there's no sample. */
    double ownTempo (TwistedBreaksProcessor&, int pad, bool& isSet);

    /** One line per mode, for menus and tooltips. */
    juce::String describe (int mode);

    /** The menu under the tempo readout: x2, /2, type in, detect, BEATS gaps, grain / cycle. */
    void showTempoMenu (TwistedBreaksProcessor&, int pad, juce::Component& target);
}
