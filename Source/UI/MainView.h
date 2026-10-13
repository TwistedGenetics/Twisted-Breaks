#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/PadGrid.h"
#include "UI/TransportDisplay.h"
#include "UI/WaveformEditor.h"
#include "UI/PadEditorPanel.h"
#include "UI/MixerPanel.h"
#include "UI/FxEditorPanel.h"
#include "UI/Controls.h"
#include "UI/NeedleMeter.h"
#include "UI/FXGrid.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    //==========================================================================
    /** Emergency stop: red mushroom button in a hazard-striped collar. */
    class PanicButton final : public juce::Button
    {
    public:
        PanicButton();
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void flash() { flashFrames = 12; repaint(); }
        void tick() { if (flashFrames > 0) { --flashFrames; repaint(); } }
    private:
        int flashFrames = 0;
    };

    /** Highest development phase that is built (nav keys up to this are live). */
    inline constexpr int kCurrentPhase = 6;

    /** Section selector key in the top rail. Lamp lit = selected. */
    class SelectorKey final : public juce::Button
    {
    public:
        SelectorKey (const juce::String& name, int phase);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        const int phase;
    private:
        juce::String latinName;
    };

    /** MIDI input lamp, last note received and the selected pad's mapping. */
    class StatusStrip final : public juce::Component
    {
    public:
        explicit StatusStrip (TwistedBreaksProcessor&);
        void refresh (int selectedPad);
        void paint (juce::Graphics&) override;
    private:
        TwistedBreaksProcessor& processor;
        uint32_t lastCount = 0;
        float    midiLamp  = 0.0f;
        int      lastNote  = -1, lastVel = 0, selPad = 0, selNote = 36;
        juce::String message;      // replaces SELECTED PAD while LIGHTS is learning
    };

    //==========================================================================
    /*
        MainView
        --------
        The whole control console at a fixed logical size (1200 x 800). The
        editor scales it as a unit, so layout code never deals with scaling.

        Static artwork (enamel, rust, rivets, plates, captions) is rendered
        once into `background`; only lamps, displays and meters repaint on the
        30 Hz timer, which keeps CPU use low inside Ableton.
    */
    class MainView final : public juce::Component,
                           private juce::Timer
    {
    public:
        static constexpr int kWidth  = 1200;
        static constexpr int kHeight = 800;

        explicit MainView (TwistedBreaksProcessor&);
        ~MainView() override;

        void paint (juce::Graphics&) override;
        void resized() override;

        /** What fills the screen area: the CRT, the group MIXER or the FX editor. */
        enum class View { crt, mixer, fx };
        void showView (View);
        View getView() const noexcept { return view; }

        /** MIXER view on / off (the mixer takes the CRT's place). */
        void showMixer (bool show) { showView (show ? View::mixer : View::crt); }
        bool isMixerShown() const noexcept { return view == View::mixer; }

        /** LIGHTS menu: controller LEDs (iRig Pads). */
        void showLightsMenu();

    private:
        void updateNavLamps();
        void timerCallback() override;
        void rebuildBackground();

        TwistedBreaksProcessor& processor;

        PadGrid          padGrid;
        TransportDisplay transportDisplay;
        WaveformEditor   waveform;
        PadEditorPanel   padEditor;
        MixerPanel       mixer;
        FxEditorPanel    fxEditor;
        View             view = View::crt;
        NeedleMeter      meter;
        FXGrid           fxGrid;
        StatusStrip      status;

        juce::Slider  masterKnob;
        ToggleSwitch  limiterSwitch { "LIMITER", "Master limiter (zero latency, -0.3 dBFS ceiling)" };
        PanicButton   panicButton;
        PushKey       lightsKey { "LIGHTS", "Light the pads of your iRig Pads to show what's playing", juce::Colour (0xff7dff5a) };
        juce::OwnedArray<SelectorKey> navKeys;

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
        std::unique_ptr<SliderAttachment> masterAttachment;
        std::unique_ptr<ButtonAttachment> limiterAttachment;

        juce::Image background;
        uint32_t shownNoteMapVersion = 0;
        juce::TooltipWindow tooltips { this, 600 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
    };
}
