#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

namespace tb::ui
{
    /*
        PadComponent
        ------------
        One of the 16 pads, styled as a backlit annunciator tile from a
        control-room alarm panel: engraved frosted glass in a screwed steel
        bezel that glows amber when the pad sounds.

        Interaction:
          * mouse down  -> trigger (velocity from click height: top = hard)
          * mouse up    -> release (used by gate/loop modes from Phase 2)
          * any click also selects the pad for the editor sections
          * drop a WAV / AIFF on the pad to load it
    */
    class PadComponent final : public juce::Component,
                                 public juce::SettableTooltipClient,
                                 public juce::FileDragAndDropTarget
    {
    public:
        explicit PadComponent (int padIndex);
        /** Scenes: the tile now stands for another engine pad (same position). */
        void setPadIndex (int padIndex);
        int  getPadIndex() const noexcept { return index; }

        // Callbacks wired by PadGrid
        std::function<void (int pad, float velocity)> onPress;
        std::function<void (int pad)>                 onRelease;
        std::function<void (int pad)>                 onSelect;
        std::function<void (int pad, const juce::File&)> onFileDropped;

        void setActivity (float level);          // 0..1 from the audio engine
        void setSelected (bool shouldBeSelected);
        void setLabels (const juce::String& upper, const juce::String& name,
                        int midiNote, const juce::String& tag, bool warning, bool hasSample);

        /** muted: pad's own mute. silenced: muted or another pad is soloed. */
        void setMuteSolo (bool muted, bool soloed, bool silenced);

        /** A SWAP hand-off onto (or a stop of) this pad is waiting to land: the bezel flashes. */
        void setQueued (bool);

        /** Output group 0..3, shown as A..D on the tile. */
        void setGroup (int);

        bool isInterestedInFileDrag (const juce::StringArray&) override;
        void fileDragEnter (const juce::StringArray&, int, int) override;
        void fileDragExit (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray&, int, int) override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> glassArea() const;
        void rebuildOverlay();

        int index;                  // engine pad (0..31); index % 16 = position in the grid
        float activity  = 0.0f;
        bool  selected  = false;
        bool  pressed   = false;
        bool  warning   = false;   // missing / unreadable sample
        bool  hasSample = false;
        bool  fileHover = false;

        juce::String upperLabel, nameLabel;
        int  note  = 36;
        juce::String tag;            // e.g. "LOOP C2"
        bool muted = false, soloed = false, silenced = false;
        bool queued = false, queuedFlash = false;
        int  group = 0;

        juce::Image overlay;     // bezel, screws, grime, cracks — baked on resize

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadComponent)
    };
}
