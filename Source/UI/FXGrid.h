#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "Core/EngineBridge.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        FXGrid  (Phase 6)
        ------
        The 4 x 4 performance-FX keys, for one group at a time:

            [ A ][ B ][ C ][ D ]          which group the keys (and MIDI) act on
            [MUTE ][ LPF ][ HPF ][ BPF ]
            [CRUSH][ RATE][DRIVE][ TAPE]
            [STUTT][REPT ][ GATE][PITCH]
            [DELAY][REVRB][ COMB][ REV ]

        Each key is LOCKED (click on, click off) or MOMENTARY (on while the
        mouse is held), shown as L / M in its corner and set in the FX
        editor. The lamp shows what the engine is really doing: lit = on
        (from the mouse, automation or MIDI), flashing = waiting for its
        musical boundary (STUTTER, REPEAT, REVERSE).

        Clicking a key also picks it for the FX editor (FX view); right-click
        picks it without switching it.
    */
    class FXGrid final : public juce::Component,
                          public juce::SettableTooltipClient
    {
    public:
        explicit FXGrid (TwistedBreaksProcessor&);

        void refresh();     // ~30 Hz
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        int  getGroup() const noexcept    { return group; }
        void setGroup (int g);
        int  getSelected() const noexcept { return selected; }
        void setSelected (int fxId);

        /** Fired when the group or the picked effect changes. */
        std::function<void (int group, int fxId)> onSelectionChanged;

        /** Key / group-key rectangles (exposed for tests). */
        juce::Rectangle<float> keyBounds (int fxId) const;
        juce::Rectangle<float> groupBounds (int g) const;

    private:
        void setFxParam (int fxId, bool on);
        bool isOn (int fxId) const;

        TwistedBreaksProcessor& processor;
        int group = 0, selected = 1;
        int held = -1;                     // momentary key held by the mouse
        uint32_t shownEngaged = 0, shownArmed = 0, shownLocked = 0;
        bool blinkPhase = false;
        juce::Image cache;
    };
}
