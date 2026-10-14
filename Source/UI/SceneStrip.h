#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "UI/Controls.h"
#include "UI/SequencerPanel.h"      // DragReadout

class TwistedBreaksProcessor;

namespace tb::ui
{
    /** Horizontal crossfader bound to the "Crossfader" parameter. Double-click: centre. */
    class Crossfader final : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        explicit Crossfader (juce::RangedAudioParameter&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        float valueAt (float x) const;
        juce::Rectangle<float> slot() const;
        void setLit (float a, float b) { if (a != litA || b != litB) { litA = a; litB = b; repaint(); } }
    private:
        juce::RangedAudioParameter& param;
        juce::ParameterAttachment att;
        float value = 0.5f, grabOffset = 0.0f, litA = 1.0f, litB = 1.0f;
    };

    /*
        SceneStrip  (Phase 13, under the pad matrix)
        ----------
            VIEW [A][B]   [CUT A] [A ====|==== B] [CUT B]   CURVE SMOOTH

        VIEW picks which 16 pads the matrix, editor, sequencer and the
        controller show (the Scene View parameter). Both scenes always play;
        the crossfader blends them (Crossfader parameter: map it to a fader).
        CUT A / CUT B slam fully to one side while held. CURVE: SMOOTH (both
        full in the middle, each fades over its far half) or SHARP (cuts at
        the last 5 %).
    */
    class SceneStrip final : public juce::Component
    {
    public:
        explicit SceneStrip (TwistedBreaksProcessor&);
        ~SceneStrip() override;
        void resized() override;
        void paint (juce::Graphics&) override;
        void refresh();     // ~30 Hz

        juce::Button& viewButton (int scene) { return scene == 0 ? viewA : viewB; }
        juce::Component& cutButton (int scene) { return scene == 0 ? cutA : cutB; }
        Crossfader& fader() { return *xfader; }

    private:
        void setView (int scene);

        TwistedBreaksProcessor& processor;
        PushKey viewA { "A", "Show scene A (pads 01-16)", juce::Colour (0xffffa928) };
        PushKey viewB { "B", "Show scene B (pads B01-B16)", juce::Colour (0xffffa928) };
        PushKey cutA  { "CUT A", "Hold: only scene A", juce::Colour (0xffff3a1c) };
        PushKey cutB  { "CUT B", "Hold: only scene B", juce::Colour (0xffff3a1c) };
        std::unique_ptr<Crossfader> xfader;
        DragReadout curveRead { "CURVE", "SMOOTH: both full in the middle. SHARP: cuts at the edges (scratch style)" };
        int shownView = -1;
    };
}
