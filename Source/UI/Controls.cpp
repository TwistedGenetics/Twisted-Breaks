#include "UI/Controls.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    //==========================================================================
    //  ToggleSwitch
    //==========================================================================
    ToggleSwitch::ToggleSwitch (const String& captionText, const String& tooltip)
        : Button (captionText), caption (captionText)
    {
        setClickingTogglesState (true);
        setTooltip (tooltip);
    }

    void ToggleSwitch::paintButton (Graphics& g, bool, bool)
    {
        const auto b   = getLocalBounds().toFloat();
        const bool on  = getToggleState();
        auto plateR    = b.withSizeKeepingCentre (b.getWidth() - 6.0f, b.getHeight() - 22.0f).withY (b.getY() + 2.0f);

        paint::plate (g, plateR, c::plateBlack, 3.0f);
        g.setColour (c::plateText);
        g.setFont (industrialFont (11.0f));
        g.drawText (String ("ON"),  plateR.removeFromTop (14.0f), Justification::centred, false);
        g.drawText (String ("OFF"), plateR.removeFromBottom (14.0f), Justification::centred, false);

        // Nut and bat handle.
        const auto ctr = plateR.getCentre();
        auto nut = Rectangle<float> (18.0f, 18.0f).withCentre (ctr);
        ColourGradient nutG (c::bareMetal.brighter (0.4f), nut.getX(), nut.getY(),
                             c::bareMetal.darker (0.7f), nut.getRight(), nut.getBottom(), false);
        g.setGradientFill (nutG);
        g.fillEllipse (nut);

        const float dir = on ? -1.0f : 1.0f;
        const auto tip  = ctr.translated (0.0f, dir * 16.0f);
        g.setColour (Colours::black.withAlpha (0.5f));
        g.drawLine ({ ctr.translated (2.0f, 2.0f), tip.translated (2.0f, 2.0f) }, 6.0f);
        ColourGradient bat (Colours::white.withAlpha (0.9f), ctr.x - 3.0f, ctr.y,
                            c::bareMetal.darker (0.5f), ctr.x + 3.0f, ctr.y, false);
        g.setGradientFill (bat);
        g.drawLine ({ ctr, tip }, 5.5f);
        g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (tip));

        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (11.5f));
        g.drawText (caption, b.withTop (b.getBottom() - 16.0f), Justification::centred, false);
    }

    //==========================================================================
    //  PushKey
    //==========================================================================
    PushKey::PushKey (const String& label, const String& tooltip, Colour lamp)
        : Button (label), lampColour (lamp)
    {
        setTooltip (tooltip);
    }

    void PushKey::tick()
    {
        if (! blinking) return;
        blinkPhase = (blinkPhase + 1) % 16;
        if (blinkPhase == 0 || blinkPhase == 8) repaint();
    }

    void PushKey::paintButton (Graphics& g, bool highlighted, bool down)
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        if (down) b.translate (0.0f, 1.0f);

        paint::plate (g, b, down ? Colour (0xff2c2e28) : Colour (0xff23251f), 3.0f);

        float lit = getToggleState() || down ? 1.0f : (highlighted ? 0.3f : 0.0f);
        if (blinking) lit = blinkPhase < 8 ? 1.0f : 0.15f;
        const float lampR = jmin (5.0f, b.getHeight() * 0.2f);
        paint::lamp (g, { b.getX() + lampR + 6.0f, b.getCentreY() }, lampR, lampColour,
                     lampColour.withMultipliedBrightness (0.28f).withMultipliedSaturation (0.7f), lit);

        g.setColour (c::plateText.withAlpha (isEnabled() ? 0.95f : 0.35f));
        g.setFont (industrialFont (jmin (14.0f, b.getHeight() * 0.55f)));
        g.drawText (getButtonText(), b.withTrimmedLeft (lampR * 2.0f + 6.0f), Justification::centred, false);
    }

    //==========================================================================
    //  RotarySelector
    //==========================================================================
    RotarySelector::RotarySelector (const StringArray& l) : labels (l) {}

    void RotarySelector::attach (RangedAudioParameter* param)
    {
        attachment.reset();
        if (param == nullptr) return;
        attachment = std::make_unique<ParameterAttachment> (*param, [this] (float v)
        {
            index = jlimit (0, labels.size() - 1, roundToInt (v));
            repaint();
        });
        attachment->sendInitialUpdate();
    }

    Point<float> RotarySelector::knobCentre() const
    {
        return { getWidth() * 0.5f, getHeight() * 0.58f };
    }

    float RotarySelector::knobRadius() const
    {
        return jmin (getWidth() * 0.2f, getHeight() * 0.24f);
    }

    float RotarySelector::angleFor (int i) const
    {
        // Spread positions over a 220-degree arc centred on "up".
        const float sweep = MathConstants<float>::pi * 1.22f;
        const int n = jmax (1, labels.size() - 1);
        return -sweep * 0.5f + sweep * (float) i / (float) n;
    }

    Rectangle<float> RotarySelector::labelBounds (int i) const
    {
        const auto p = knobCentre().getPointOnCircumference (knobRadius() + 15.0f, angleFor (i));
        return Rectangle<float> (34.0f, 13.0f).withCentre (p);
    }

    void RotarySelector::paint (Graphics& g)
    {
        const auto c = knobCentre();
        const float r = knobRadius();

        for (int i = 0; i < labels.size(); ++i)
        {
            const bool on = i == index;
            const auto a  = angleFor (i);
            g.setColour (c::bakelite.withAlpha (on ? 0.9f : 0.45f));
            g.drawLine ({ c.getPointOnCircumference (r + 2.0f, a), c.getPointOnCircumference (r + 6.0f, a) }, 1.5f);
            g.setColour (on ? c::amber : c::bakelite.withAlpha (0.6f));
            g.setFont (industrialFont (11.5f, on));
            g.drawText (labels[i], labelBounds (i), Justification::centred, false);
        }

        // Body: black bakelite disc with a raised chicken-head pointer.
        auto disc = Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillEllipse (disc.translated (1.5f, 2.5f));
        ColourGradient body (Colour (0xff45423e), disc.getX() + r * 0.5f, disc.getY() + r * 0.4f,
                             Colour (0xff0e0d0c), disc.getRight(), disc.getBottom(), true);
        g.setGradientFill (body);
        g.fillEllipse (disc);

        const float a = angleFor (index);
        Path pointer;
        pointer.addRoundedRectangle (-r * 0.28f, -r * 1.12f, r * 0.56f, r * 1.7f, r * 0.24f);
        pointer.applyTransform (AffineTransform::rotation (a).translated (c));
        g.setColour (Colours::black.withAlpha (0.5f));
        g.fillPath (pointer, AffineTransform::translation (1.0f, 2.0f));
        ColourGradient ptr (Colour (0xff4c4945), c.x - r, c.y - r, Colour (0xff141312), c.x + r, c.y + r, false);
        g.setGradientFill (ptr);
        g.fillPath (pointer);
        g.setColour (c::bakelite);
        g.drawLine ({ c.getPointOnCircumference (r * 0.25f, a), c.getPointOnCircumference (r * 1.02f, a) }, 2.2f);
    }

    void RotarySelector::choose (int newIndex)
    {
        newIndex = jlimit (0, labels.size() - 1, newIndex);
        if (newIndex == index || attachment == nullptr) return;
        index = newIndex;
        attachment->setValueAsPartOfGesture ((float) index);
        repaint();
    }

    void RotarySelector::mouseDown (const MouseEvent& e)
    {
        if (attachment == nullptr) return;
        attachment->beginGesture();
        dragging = false;
        dragStartIndex = index;

        for (int i = 0; i < labels.size(); ++i)
            if (labelBounds (i).expanded (3.0f).contains (e.position)) { choose (i); return; }
    }

    void RotarySelector::mouseDrag (const MouseEvent& e)
    {
        if (e.getDistanceFromDragStart() < 4) return;
        dragging = true;
        choose (dragStartIndex - e.getDistanceFromDragStartY() / 16);
    }

    void RotarySelector::mouseUp (const MouseEvent& e)
    {
        if (attachment == nullptr) return;
        const bool onLabel = [&] { for (int i = 0; i < labels.size(); ++i) if (labelBounds (i).expanded (3.0f).contains (e.position)) return true; return false; }();
        if (! dragging && ! onLabel)
            choose ((index + 1) % labels.size());        // click the knob: next position
        attachment->endGesture();
    }

    void RotarySelector::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
    {
        if (attachment == nullptr || std::abs (w.deltaY) < 1.0e-4f) return;
        attachment->beginGesture();
        choose (index + (w.deltaY > 0.0f ? 1 : -1));
        attachment->endGesture();
    }

    //==========================================================================
    //  NoteSelector
    //==========================================================================
    NoteSelector::NoteSelector()
    {
        setTooltip ("MIDI note for this pad. Drag up / down or use the wheel; LEARN takes the next note you play.");
        addAndMakeVisible (learnKey);
        learnKey.onClick = [this] { if (onLearn) onLearn(); };
    }

    void NoteSelector::setNote (int n)
    {
        if (n != note) { note = n; repaint(); }
    }

    void NoteSelector::setLearning (bool learning)
    {
        learnKey.setBlinking (learning);
    }

    Rectangle<float> NoteSelector::readout() const
    {
        return getLocalBounds().toFloat().removeFromTop (30.0f);
    }

    void NoteSelector::resized()
    {
        learnKey.setBounds (getLocalBounds().withTrimmedTop (34));
    }

    void NoteSelector::paint (Graphics& g)
    {
        const auto r = readout();
        paint::recess (g, r, 3.0f);
        g.setColour (c::vfd);
        g.setFont (monoFont (15.0f));
        g.drawText (note < 0 ? String ("--") : MidiMessage::getMidiNoteName (note, true, true, 3) + " " + String (note),
                    r, Justification::centred, false);
    }

    void NoteSelector::mouseDown (const MouseEvent&)
    {
        dragStartNote = note;
    }

    void NoteSelector::mouseDrag (const MouseEvent& e)
    {
        const int n = jlimit (0, 127, dragStartNote - e.getDistanceFromDragStartY() / 6);
        if (n != note && onNoteChanged) onNoteChanged (n);
    }

    void NoteSelector::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
    {
        wheelAccum += w.deltaY * 8.0f;
        const int steps = (int) wheelAccum;
        if (steps != 0)
        {
            wheelAccum -= (float) steps;
            if (onNoteChanged) onNoteChanged (jlimit (0, 127, note + steps));
        }
    }
}
