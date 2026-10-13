#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace tb::ui
{
    /** Bat-handle toggle switch on an ON / OFF plate, caption engraved below. */
    class ToggleSwitch final : public juce::Button
    {
    public:
        ToggleSwitch (const juce::String& caption, const juce::String& tooltip);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    private:
        juce::String caption;
    };

    /**
        Square black push key with a lamp. Momentary by default; when used as
        a latching key (setClickingTogglesState) or as a page selector, the
        lamp shows the toggle state in the given colour.
    */
    class PushKey final : public juce::Button
    {
    public:
        PushKey (const juce::String& label, const juce::String& tooltip,
                 juce::Colour lampColour = juce::Colour (0xffffa928));
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void setBlinking (bool shouldBlink) { blinking = shouldBlink; repaint(); }
        void tick();                         // call ~30 Hz for blinking
    private:
        juce::Colour lampColour;
        bool blinking = false;
        int  blinkPhase = 0;
    };

    /**
        Multi-position rotary switch (chicken-head pointer) for choice
        parameters: labels sit around the knob, click a label to jump to it,
        click the knob to step, drag up / down or use the wheel to turn it.
        Bound to the parameter with a ParameterAttachment, so changes are
        proper host gestures (automation, undo, Ableton's dirty-state).
    */
    class RotarySelector final : public juce::Component,
                                 public juce::SettableTooltipClient
    {
    public:
        RotarySelector (const juce::StringArray& labels);

        void attach (juce::RangedAudioParameter* param);
        int  getIndex() const noexcept { return index; }

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        juce::Point<float> knobCentre() const;
        float knobRadius() const;
        float angleFor (int i) const;
        juce::Rectangle<float> labelBounds (int i) const;
        void choose (int newIndex);

        juce::StringArray labels;
        int index = 0;
        int dragStartIndex = 0;
        bool dragging = false;
        std::unique_ptr<juce::ParameterAttachment> attachment;
    };

    /**
        MIDI note readout for a pad: drag up / down or wheel to change the
        note, LEARN to take the next note played on a controller.
    */
    class NoteSelector final : public juce::Component,
                               public juce::SettableTooltipClient
    {
    public:
        NoteSelector();

        std::function<void (int note)> onNoteChanged;   // from drag / wheel
        std::function<void()>          onLearn;         // LEARN pressed

        void setNote (int note);
        void setLearning (bool isLearning);
        void tick() { learnKey.tick(); }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        juce::Rectangle<float> readout() const;
        int note = 36, dragStartNote = 36;
        float wheelAccum = 0.0f;
        PushKey learnKey { "LEARN", "Play a note on your controller to assign it to this pad" };
    };
}
