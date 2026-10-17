#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/PadGrid.h"
#include "UI/TransportDisplay.h"
#include "UI/WaveformEditor.h"
#include "UI/PadEditorPanel.h"
#include "UI/MixerPanel.h"
#include "UI/FxEditorPanel.h"
#include "UI/SequencerPanel.h"
#include "UI/SliceSeqPanel.h"
#include "UI/SnapshotPanel.h"
#include "UI/PresetPanel.h"
#include "UI/ResamplePanel.h"
#include "UI/SceneStrip.h"
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
    inline constexpr int kCurrentPhase = 13;

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
        // A7: while Push 3 is in User mode, the strip shows its page, pad and the knob you touch.
        bool pushShown = false;
        juce::String pushCaption, pushValue, pushCaption2, pushValue2;
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
        enum class View { crt, mixer, fx, sequencer, sliceSeq, snapshots, resample, presets };
        void showView (View);
        View getView() const noexcept { return view; }

        /** MIXER view on / off (the mixer takes the CRT's place). */
        void showMixer (bool show) { showView (show ? View::mixer : View::crt); }
        bool isMixerShown() const noexcept { return view == View::mixer; }

        /** LIGHTS menu: controller LEDs (iRig Pads) and Push 3 mode. */
        void showLightsMenu();

        void mouseDown (const juce::MouseEvent&) override;   // (all children) this instance takes Push
        /** Cmd-C / Cmd-V (selected pad), Cmd-Z / Cmd-Shift-Z (A11). */
        bool keyPressed (const juce::KeyPress&) override;

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
        SequencerPanel   sequencer;
        SliceSeqPanel    sliceSeq;
        SnapshotPanel    snapshotPanel;
        ResamplePanel    resamplePanel;
        SceneStrip       sceneStrip;
        PresetPanel      presetPanel;
        KitBar           kitBar;
        bool             shownCtlBrowse = false;
        int              shownScene = 0;
        uint32_t         shownSeqVersion = 0;
        uint32_t         shownPushSelection = 0;
        uint32_t         shownMikroSelection = 0;
        View             view = View::crt;
        NeedleMeter      meter;
        FXGrid           fxGrid;
        StatusStrip      status;

        juce::Slider  masterKnob;
        ToggleSwitch  limiterSwitch { "LIMITER", "Master limiter (zero latency, -0.3 dBFS ceiling)" };
        PanicButton   panicButton;
        PushKey       lightsKey { "LIGHTS", "Light the pads of your iRig Pads, and Push 3 mode (play the plugin from Push in User mode)", juce::Colour (0xff7dff5a) };
        juce::OwnedArray<SelectorKey> navKeys;
        PushKey       undoKey { "UNDO", "Undo the last pad edit: paste, swap, clear, sample, slices or steps (Cmd-Z)", juce::Colour (0xffffa928) };
        PushKey       redoKey { "REDO", "Redo (Cmd-Shift-Z)", juce::Colour (0xffffa928) };
        uint32_t      shownUndoVersion = 0xffffffff;

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
