#include "UI/SceneStrip.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    //==========================================================================
    //  Crossfader
    //==========================================================================
    Crossfader::Crossfader (RangedAudioParameter& p)
        : param (p), att (p, [this] (float v) { value = v; repaint(); })
    {
        setTooltip ("Crossfader: scene A <-> scene B (the Crossfader parameter). Double-click: centre");
        att.sendInitialUpdate();
    }

    Rectangle<float> Crossfader::slot() const
    {
        return getLocalBounds().toFloat().reduced (18.0f, 0.0f).withSizeKeepingCentre ((float) getWidth() - 36.0f, 8.0f);
    }

    float Crossfader::valueAt (float x) const
    {
        const auto s = slot();
        return jlimit (0.0f, 1.0f, (x - s.getX()) / s.getWidth());
    }

    void Crossfader::mouseDown (const MouseEvent& e)
    {
        const auto s = slot();
        const float capX = s.getX() + value * s.getWidth();
        grabOffset = std::abs (e.position.x - capX) < 12.0f ? e.position.x - capX : 0.0f;
        att.beginGesture();
        att.setValueAsPartOfGesture (valueAt (e.position.x - grabOffset));
    }

    void Crossfader::mouseDrag (const MouseEvent& e) { att.setValueAsPartOfGesture (valueAt (e.position.x - grabOffset)); }
    void Crossfader::mouseUp (const MouseEvent&)     { att.endGesture(); }
    void Crossfader::mouseDoubleClick (const MouseEvent&) { att.setValueAsCompleteGesture (0.5f); }

    void Crossfader::paint (Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        paint::recess (g, b, 4.0f);
        const auto s = slot();

        // Scene lamps at the ends: how much of each you hear.
        paint::lamp (g, { b.getX() + 9.0f, b.getCentreY() }, 4.0f, c::amber, c::amberDim, litA);
        paint::lamp (g, { b.getRight() - 9.0f, b.getCentreY() }, 4.0f, c::amber, c::amberDim, litB);

        g.setColour (Colour (0xff0c0d0c));
        g.fillRoundedRectangle (s, 3.0f);
        g.setColour (c::bakelite.withAlpha (0.35f));
        for (int i = 0; i <= 10; ++i)
        {
            const float x = s.getX() + s.getWidth() * (float) i / 10.0f;
            g.fillRect (x - 0.5f, s.getBottom() + 3.0f, 1.0f, i == 5 ? 6.0f : 3.0f);
        }

        // Cap.
        const float cx = s.getX() + value * s.getWidth();
        const auto cap = Rectangle<float> (18.0f, b.getHeight() - 8.0f).withCentre ({ cx, b.getCentreY() });
        paint::plate (g, cap, Colour (0xff8d8f87), 2.0f);
        g.setColour (Colours::black.withAlpha (0.8f));
        g.fillRect (cx - 1.0f, cap.getY() + 3.0f, 2.0f, cap.getHeight() - 6.0f);
    }

    //==========================================================================
    //  SceneStrip
    //==========================================================================
    SceneStrip::SceneStrip (TwistedBreaksProcessor& p) : processor (p)
    {
        xfader = std::make_unique<Crossfader> (*processor.getParameters().getParameter (params::xfade));

        viewA.setRadioGroupId (7401);
        viewB.setRadioGroupId (7401);
        viewA.onClick = [this] { setView (0); };
        viewB.onClick = [this] { setView (1); };

        // CUT keys: only while held.
        cutA.onStateChange = [this] { processor.getEngine().setCrossfaderCut (cutA.isDown() ? 0 : (cutB.isDown() ? 1 : -1)); };
        cutB.onStateChange = [this] { processor.getEngine().setCrossfaderCut (cutB.isDown() ? 1 : (cutA.isDown() ? 0 : -1)); };

        DragReadout::Model m;
        m.get = [this] { return roundToInt (processor.getParameters().getRawParameterValue (params::xfadeCurve)->load()); };
        m.set = [this] (int v)
        {
            if (auto* prm = processor.getParameters().getParameter (params::xfadeCurve))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (prm->convertTo0to1 ((float) v));
                prm->endChangeGesture();
            }
        };
        m.min = 0; m.max = 1; m.def = 0; m.cycleOnClick = true;
        m.text = [] (int v) { return v == 0 ? String ("SMOOTH") : String ("SHARP"); };
        curveRead.bind (m);

        for (Component* comp : std::initializer_list<Component*> { &viewA, &viewB, &cutA, &cutB, xfader.get(), &curveRead })
            addAndMakeVisible (comp);
        refresh();
    }

    SceneStrip::~SceneStrip() = default;

    void SceneStrip::setView (int scene)
    {
        if (auto* prm = processor.getParameters().getParameter (params::sceneView))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) scene));
            prm->endChangeGesture();
        }
        refresh();
    }

    void SceneStrip::resized()
    {
        const int h = getHeight();
        viewA.setBounds (40, 6, 36, h - 10);
        viewB.setBounds (78, 6, 36, h - 10);
        cutA.setBounds (124, 6, 62, h - 10);
        xfader->setBounds (190, 4, getWidth() - 190 - 66 - 74, h - 6);
        cutB.setBounds (getWidth() - 66 - 72, 6, 62, h - 10);
        curveRead.setBounds (getWidth() - 70, 0, 70, h);
    }

    void SceneStrip::paint (Graphics& g)
    {
        g.setColour (c::plateText.withAlpha (0.85f));
        g.setFont (industrialFont (12.0f));
        g.drawText ("VIEW", Rectangle<float> (0.0f, 0.0f, 38.0f, (float) getHeight()), Justification::centredLeft, false);
    }

    void SceneStrip::refresh()
    {
        const int v = roundToInt (processor.getParameters().getRawParameterValue (params::sceneView)->load());
        if (v != shownView)
        {
            shownView = v;
            viewA.setToggleState (v == 0, dontSendNotification);
            viewB.setToggleState (v == 1, dontSendNotification);
        }
        auto& e = processor.getEngine();
        xfader->setLit (e.getSceneGain (0), e.getSceneGain (1));
        cutA.setToggleState (e.getCrossfaderCut() == 0, dontSendNotification);
        cutB.setToggleState (e.getCrossfaderCut() == 1, dontSendNotification);
        curveRead.repaint();
    }
}
