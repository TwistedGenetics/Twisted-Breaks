#include "UI/FxEditorPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "Core/FxDefs.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr int kCtlTop = 106;
        constexpr int kCellW  = 104;
    }

    FxEditorPanel::FxEditorPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        for (auto& k : knobs)
        {
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::TextBoxBelow, true, 88, 16);
            k.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            addChildComponent (k);
        }

        onKey.setClickingTogglesState (true);
        lockedKey.onClick    = [this] { processor.setFxLocked (fxId, true);  refresh(); };
        momentaryKey.onClick = [this] { processor.setFxLocked (fxId, false); refresh(); };
        allOffKey.onClick    = [this]
        {
            for (int f = 0; f < fx::kNumFx; ++f)
            {
                if (auto* prm = processor.getParameters().getParameter (fx::paramId (group, f)))
                    if (prm->getValue() > 0.5f)
                    {
                        prm->beginChangeGesture();
                        prm->setValueNotifyingHost (0.0f);
                        prm->endChangeGesture();
                    }
                processor.getEngine().clearFxLatch (group, f);
            }
        };

        for (auto* k : { &onKey, &lockedKey, &momentaryKey, &allOffKey })
            addAndMakeVisible (k);

        rebind();
    }

    FxEditorPanel::~FxEditorPanel()
    {
        for (auto& a : knobAtt) a.reset();
        for (auto& s : selectors) if (s) s->attach (nullptr);
        onAtt.reset();
    }

    void FxEditorPanel::setTarget (int g, int f)
    {
        g = jlimit (0, 3, g);
        f = jlimit (0, fx::kNumFx - 1, f);
        if (g == group && f == fxId) return;
        group = g; fxId = f;
        rebind();
        repaint();
    }

    void FxEditorPanel::rebind()
    {
        auto& apvts = processor.getParameters();
        const auto& d = fx::def (fxId);

        for (int k = 0; k < 3; ++k)
        {
            knobAtt[(size_t) k].reset();
            if (selectors[(size_t) k]) { selectors[(size_t) k]->attach (nullptr); removeChildComponent (selectors[(size_t) k].get()); selectors[(size_t) k].reset(); }
            knobs[(size_t) k].setVisible (false);

            if (k >= d.numParams) continue;
            const auto& pd = d.params[k];
            const auto id  = fx::paramId (group, fxId, k);
            if (pd.isChoice())
            {
                StringArray labels;
                for (int i = 0; i < pd.numChoices; ++i) labels.add (pd.choices[i]);
                selectors[(size_t) k] = std::make_unique<RotarySelector> (labels);
                selectors[(size_t) k]->setTooltip (String (d.name) + ": " + pd.label);
                selectors[(size_t) k]->attach (apvts.getParameter (id));
                addAndMakeVisible (*selectors[(size_t) k]);
            }
            else
            {
                knobAtt[(size_t) k] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (apvts, id, knobs[(size_t) k]);
                knobs[(size_t) k].setDoubleClickReturnValue (true, pd.def);
                knobs[(size_t) k].setTooltip (String (d.name) + ": " + pd.label + " (double-click: default)");
                knobs[(size_t) k].setVisible (true);
            }
        }

        onAtt.reset();
        onAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, fx::paramId (group, fxId), onKey);

        resized();
        refresh();
    }

    void FxEditorPanel::refresh()
    {
        const bool locked = processor.isFxLocked (fxId);
        lockedKey.setToggleState (locked, dontSendNotification);
        momentaryKey.setToggleState (! locked, dontSendNotification);
    }

    Rectangle<int> FxEditorPanel::controlCell (int k) const
    {
        return { 24 + k * kCellW, kCtlTop, kCellW - 8, getHeight() - kCtlTop - 14 };
    }

    void FxEditorPanel::resized()
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto cell = controlCell (k);
            knobs[(size_t) k].setBounds (cell.reduced (6, 0));
            if (selectors[(size_t) k]) selectors[(size_t) k]->setBounds (cell.withTrimmedTop (-6));
        }

        const int x = getWidth() - 196;
        onKey.setBounds        (getWidth() - 96, 42, 76, 34);
        lockedKey.setBounds    (x,       kCtlTop - 20, 176, 28);
        momentaryKey.setBounds (x,       kCtlTop + 12, 176, 28);
        allOffKey.setBounds    (x,       kCtlTop + 52, 176, 28);
        background = {};
    }

    void FxEditorPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random rnd (6606);
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
            paint::labelPlate (bg, { 22.0f, 10.0f, 190.0f, 22.0f }, "FX EDITOR", {}, paint::PlateStyle::bakelite, rnd);
            paint::recess (bg, { 22.0f, 42.0f, (float) getWidth() - 132.0f, 34.0f }, 4.0f);
            paint::recess (bg, { 16.0f, (float) kCtlTop - 22.0f, 3.0f * kCellW + 8.0f, (float) (getHeight() - kCtlTop + 12) }, 4.0f);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        const auto& d = fx::def (fxId);
        g.setColour (c::vfd);
        g.setFont (monoFont (17.0f));
        g.drawText ("GROUP " + String::charToString ((juce_wchar) ('A' + group)) + "  -  FX "
                      + String (fxId + 1).paddedLeft ('0', 2) + "  " + d.name,
                    Rectangle<float> (34.0f, 42.0f, (float) getWidth() - 150.0f, 34.0f), Justification::centredLeft, true);

        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (13.0f));
        for (int k = 0; k < d.numParams; ++k)
            g.drawText (d.params[k].label, controlCell (k).withY (kCtlTop - 18).withHeight (14).toFloat(), Justification::centred, false);
        if (d.numParams == 0)
            g.drawText ("NO SETTINGS - IT JUST CUTS THE GROUP", Rectangle<float> (24.0f, (float) kCtlTop + 30.0f, 3.0f * kCellW, 20.0f),
                        Justification::centred, false);

        g.setColour (c::bakelite.withAlpha (0.6f));
        g.setFont (monoFont (10.5f));
        g.drawText ("MIDI C3-D#4 = FX 1-16", Rectangle<float> ((float) getWidth() - 196.0f, (float) getHeight() - 24.0f, 176.0f, 14.0f),
                    Justification::centredLeft, true);
    }
}
