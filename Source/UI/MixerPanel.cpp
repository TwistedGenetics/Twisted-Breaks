#include "UI/MixerPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr int kTop    = 40;    // strips start under the title plate
        constexpr int kMargin = 14;

        String groupName (int g) { return "GROUP " + String::charToString ((juce_wchar) ('A' + g)); }

        /** dB -> 0..1 on the LED scale (-48 .. +6 dB). */
        float meterPos (float linear)
        {
            if (linear <= 0.0f) return 0.0f;
            const float db = Decibels::gainToDecibels (linear, -100.0f);
            return jlimit (0.0f, 1.0f, (db + 48.0f) / 54.0f);
        }
    }

    MixerPanel::MixerPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        auto& apvts = processor.getParameters();

        for (int g = 0; g < kNumGroups; ++g)
        {
            auto& s = strips[(size_t) g];
            const auto gn = groupName (g);

            s.fader.setSliderStyle (Slider::LinearVertical);
            s.fader.setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
            s.fader.setTooltip (gn + " level (double-click: 0 dB)");
            s.faderAtt = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (apvts, params::groupId (g, params::grpLevel), s.fader);
            s.fader.setDoubleClickReturnValue (true, 0.0);
            s.fader.onValueChange = [this, g] { repaint (stripBounds (g).withTop (getHeight() - 40)); };

            s.pan.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            s.pan.setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
            s.pan.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            s.pan.setTooltip (gn + " pan (double-click: centre)");
            s.panAtt = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (apvts, params::groupId (g, params::grpPan), s.pan);
            s.pan.setDoubleClickReturnValue (true, 0.0);

            for (auto* k : { &s.mute, &s.solo, &s.bypass })
                k->setClickingTogglesState (true);
            s.muteAtt   = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::groupId (g, params::grpMute),   s.mute);
            s.soloAtt   = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::groupId (g, params::grpSolo),   s.solo);
            s.bypassAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::groupId (g, params::grpBypass), s.bypass);

            for (Component* comp : std::initializer_list<Component*> { &s.fader, &s.pan, &s.mute, &s.solo, &s.bypass })
                addAndMakeVisible (comp);
        }
    }

    MixerPanel::~MixerPanel()
    {
        for (auto& s : strips)
        {
            s.faderAtt.reset(); s.panAtt.reset();
            s.muteAtt.reset(); s.soloAtt.reset(); s.bypassAtt.reset();
        }
    }

    //==========================================================================
    Rectangle<int> MixerPanel::stripBounds (int g) const
    {
        const int w = (getWidth() - 2 * kMargin) / kNumGroups;
        return { kMargin + g * w, kTop, w, getHeight() - kTop - 10 };
    }

    Rectangle<int> MixerPanel::headerBounds (int g) const
    {
        return stripBounds (g).reduced (6, 0).withHeight (22).translated (0, 6);
    }

    Rectangle<int> MixerPanel::meterBounds (int g) const
    {
        const auto s = stripBounds (g);
        return { s.getX() + 52, s.getY() + 52, 9, s.getHeight() - 78 };
    }

    void MixerPanel::resized()
    {
        for (int g = 0; g < kNumGroups; ++g)
        {
            auto& s = strips[(size_t) g];
            const auto b = stripBounds (g);
            s.fader.setBounds (b.getX() + 8, b.getY() + 46, 40, b.getHeight() - 66);

            auto col = Rectangle<int> (b.getX() + 68, b.getY() + 34, b.getWidth() - 76, b.getHeight() - 38);
            s.pan.setBounds (col.removeFromTop (50).reduced (4, 0).withTrimmedTop (10));
            col.removeFromTop (6);
            s.mute.setBounds (col.removeFromTop (24).reduced (2, 0));
            col.removeFromTop (4);
            s.solo.setBounds (col.removeFromTop (24).reduced (2, 0));
            col.removeFromTop (4);
            s.bypass.setBounds (col.removeFromTop (24).reduced (2, 0));
        }
        background = {};
    }

    //==========================================================================
    int MixerPanel::groupOfPad (int pad) const
    {
        return roundToInt (processor.getParameters().getRawParameterValue (params::padId (pad, params::padGroup))->load());
    }

    int MixerPanel::padsInGroup (int g) const
    {
        int n = 0;
        for (int p = 0; p < kNumPads; ++p)
            if (groupOfPad (p) == g) ++n;
        return n;
    }

    void MixerPanel::refresh()
    {
        auto& bridge = processor.getBridge();
        for (int g = 0; g < kNumGroups; ++g)
        {
            auto& s = strips[(size_t) g];

            // LED bar: instant attack, ~300 ms fall.
            const float target = meterPos (bridge.groupPeak[(size_t) g].exchange (0.0f));
            const float next = jmax (target, s.meter - 0.06f);
            if (std::abs (next - s.meter) > 0.001f)
            {
                s.meter = next;
                repaint (meterBounds (g));
            }

            const int count = padsInGroup (g);
            if (count != s.padCount) { s.padCount = count; repaint (stripBounds (g).withHeight (46)); }
        }
    }

    void MixerPanel::mouseUp (const MouseEvent& e)
    {
        // Click a strip's header plate: route the selected pad to that group.
        for (int g = 0; g < kNumGroups; ++g)
            if (headerBounds (g).contains (e.getPosition()) && e.mouseWasClicked())
                if (auto* prm = processor.getParameters().getParameter (params::padId (selectedPad, params::padGroup)))
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) g));
                    prm->endChangeGesture();
                    repaint();
                }
    }

    //==========================================================================
    void MixerPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random rnd (5505);
            const auto all = getLocalBounds().toFloat();
            paint::plate (bg, all, c::steel, 4.0f);
            {
                Graphics::ScopedSaveState ss (bg);
                bg.reduceClipRegion (getLocalBounds());
                paint::ageArea (bg, all, rnd, 1.3f);
                paint::edgeWear (bg, all, rnd, 1.5f);
            }
            const auto r = all.reduced (9.0f);
            for (auto pt : { r.getTopLeft(), r.getTopRight(), r.getBottomLeft(), r.getBottomRight() })
                paint::rivet (bg, pt, 4.2f, rnd);

            paint::labelPlate (bg, { 22.0f, 10.0f, 190.0f, 22.0f }, "GROUP MIXER", {}, paint::PlateStyle::bakelite, rnd);

            for (int k = 0; k < kNumGroups; ++k)
            {
                const auto sb = stripBounds (k).toFloat().reduced (2.0f, 0.0f);
                paint::recess (bg, sb, 4.0f);

                // dB scale beside the LED bar.
                const auto m = meterBounds (k).toFloat();
                bg.setFont (monoFont (9.0f));
                for (float db : { 6.0f, 0.0f, -12.0f, -24.0f, -48.0f })
                {
                    const float y = m.getBottom() - (db + 48.0f) / 54.0f * m.getHeight();
                    bg.setColour (c::bakelite.withAlpha (0.55f));
                    bg.drawLine (m.getRight() + 1.0f, y, m.getRight() + 4.0f, y, 1.0f);
                }
                bg.setColour (c::bakelite.withAlpha (0.8f));
                bg.setFont (industrialFont (11.0f));
                bg.drawText ("PAN", Rectangle<float> (sb.getX() + 66.0f, sb.getY() + 34.0f, sb.getWidth() - 72.0f, 12.0f),
                             Justification::centred, false);
            }
        }
        g.drawImage (background, getLocalBounds().toFloat());

        const int selGroup = groupOfPad (selectedPad);
        for (int k = 0; k < kNumGroups; ++k)
        {
            const auto& s = strips[(size_t) k];
            const auto hb = headerBounds (k).toFloat();

            // Header: black plate, lamp lit if the selected pad is routed here.
            paint::plate (g, hb, Colour (0xff1d1e1b), 2.0f);
            paint::lamp (g, { hb.getX() + 12.0f, hb.getCentreY() }, 4.5f, c::amber, c::amberDim, k == selGroup ? 1.0f : 0.15f);
            g.setColour (c::plateText);
            g.setFont (industrialFont (14.0f));
            g.drawText (groupName (k), hb.withTrimmedLeft (22.0f), Justification::centredLeft, false);

            const auto sb = stripBounds (k).toFloat();
            g.setColour (c::bakelite.withAlpha (0.65f));
            g.setFont (industrialFont (10.5f, false));
            g.drawText (String (jmax (0, s.padCount)) + (s.padCount == 1 ? " PAD" : " PADS"),
                        Rectangle<float> (sb.getX() + 8.0f, sb.getY() + 30.0f, 60.0f, 12.0f), Justification::centredLeft, false);

            // LED bar: green, amber above -12 dB, red above 0 dB.
            const auto m = meterBounds (k).toFloat();
            g.setColour (Colours::black.withAlpha (0.85f));
            g.fillRect (m);
            const int segs = 22;
            const float segH = m.getHeight() / (float) segs;
            for (int i = 0; i < segs; ++i)
            {
                const float pos = ((float) i + 0.5f) / (float) segs;
                const float db  = pos * 54.0f - 48.0f;
                const Colour lit = db > 0.0f ? c::red : (db > -12.0f ? c::amber : c::green);
                const bool on = pos <= s.meter;
                g.setColour (on ? lit : lit.withAlpha (0.12f));
                g.fillRect (m.getX() + 1.5f, m.getBottom() - (float) (i + 1) * segH + 1.0f, m.getWidth() - 3.0f, segH - 1.5f);
            }

            // Level readout under the fader.
            const float db = (float) s.fader.getValue();
            g.setColour (c::vfd);
            g.setFont (monoFont (11.0f));
            g.drawText (db <= params::grpLevelMinDb + 0.01f ? String ("-inf") : String (db, 1),
                        Rectangle<float> (sb.getX() + 4.0f, sb.getBottom() - 20.0f, 60.0f, 14.0f), Justification::centred, false);
        }
    }
}
