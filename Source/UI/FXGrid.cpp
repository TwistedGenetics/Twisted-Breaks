#include "UI/FXGrid.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "Core/FxDefs.h"
#include "Core/MacroDefs.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr float kGroupRowH = 24.0f;
        constexpr float kGap = 5.0f;
    }

    FXGrid::FXGrid (TwistedBreaksProcessor& p) : processor (p)
    {
        group = processor.getEngine().getFxTargetGroup();
        setTooltip ("Performance FX for the chosen group. Click: on / off (LOCKED) or hold (MOMENTARY). "
                    "Right-click: edit without switching. MIDI notes C3-D#4 play FX 1-16, E4-B4 the MACROs.");

        auto& apvts = processor.getParameters();
        latchKey.setClickingTogglesState (true);
        latchAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::macroLatch, latchKey);
        auto paramModel = [this] (const char* id, int min, int max, int def, int scale, std::function<String (int)> text)
        {
            DragReadout::Model m;
            m.get = [this, id, scale] { return roundToInt (processor.getParameters().getRawParameterValue (id)->load() / (float) scale); };
            m.set = [this, id, scale] (int v)
            {
                if (auto* prm = processor.getParameters().getParameter (id))
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (v * scale)));
                    prm->endChangeGesture();
                }
            };
            m.min = min; m.max = max; m.def = def; m.text = std::move (text);
            return m;
        };
        amountRead.bind (paramModel (params::macroAmount, 0, 20, 12, 5, [] (int v) { return String (v * 5) + "%"; }));
        auto gm = paramModel (params::macroGroup, 0, 4, 4, 1, [] (int v) { return v >= 4 ? String ("ALL") : String::charToString ((juce_wchar) ('A' + v)); });
        gm.cycleOnClick = true;
        groupRead.bind (gm);
        for (Component* comp : std::initializer_list<Component*> { &amountRead, &groupRead, &latchKey })
            addChildComponent (comp);
    }

    Rectangle<float> FXGrid::groupBounds (int g) const
    {
        const float w = 38.0f;
        return { (float) g * (w + kGap), 0.0f, w, kGroupRowH - 4.0f };
    }

    Rectangle<float> FXGrid::macroTabBounds() const
    {
        const float x = groupBounds (3).getRight() + kGap;
        return { x, 0.0f, (float) getWidth() - x, kGroupRowH - 4.0f };
    }

    Rectangle<float> FXGrid::macroKeyBounds (int m) const
    {
        const auto area = getLocalBounds().toFloat().withTrimmedTop (kGroupRowH).withTrimmedBottom (46.0f);
        const float kw = (area.getWidth()  - kGap * 3.0f) / 4.0f;
        const float kh = (area.getHeight() - kGap) / 2.0f;
        return { area.getX() + (float) (m % 4) * (kw + kGap), area.getY() + (float) (m / 4) * (kh + kGap), kw, kh };
    }

    void FXGrid::showMacros (bool show)
    {
        if (show == macroPage) return;
        if (heldMacro >= 0) { setMacroParam (heldMacro, false); heldMacro = -1; }
        if (held >= 0)      { setFxParam (held, false); held = -1; }
        macroPage = show;
        for (Component* comp : std::initializer_list<Component*> { &amountRead, &groupRead, &latchKey })
            comp->setVisible (show);
        repaint();
    }

    void FXGrid::setMacroParam (int m, bool on)
    {
        if (auto* prm = processor.getParameters().getParameter (macro::paramId (m)))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (on ? 1.0f : 0.0f);
            prm->endChangeGesture();
        }
        processor.getEngine().clearMacroLatch();        // the keys decide from now on
    }

    Rectangle<float> FXGrid::keyBounds (int f) const
    {
        const auto area = getLocalBounds().toFloat().withTrimmedTop (kGroupRowH);
        const float kw = (area.getWidth()  - kGap * 3.0f) / 4.0f;
        const float kh = (area.getHeight() - kGap * 3.0f) / 4.0f;
        return { area.getX() + (float) (f % 4) * (kw + kGap), area.getY() + (float) (f / 4) * (kh + kGap), kw, kh };
    }

    void FXGrid::setGroup (int g)
    {
        g = jlimit (0, kNumGroups - 1, g);
        processor.setFxGroup (g);
        if (g == group) return;
        group = g;
        if (held >= 0) { setFxParam (held, false); held = -1; }
        repaint();
        if (onSelectionChanged) onSelectionChanged (group, selected);
    }

    void FXGrid::setSelected (int f)
    {
        f = jlimit (0, fx::kNumFx - 1, f);
        if (f == selected) return;
        selected = f;
        repaint();
        if (onSelectionChanged) onSelectionChanged (group, selected);
    }

    bool FXGrid::isOn (int f) const
    {
        return ((processor.getBridge().fxEngaged[(size_t) group].load() >> f) & 1u) != 0;
    }

    void FXGrid::setFxParam (int f, bool on)
    {
        if (auto* prm = processor.getParameters().getParameter (fx::paramId (group, f)))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (on ? 1.0f : 0.0f);
            prm->endChangeGesture();
        }
        processor.getEngine().clearFxLatch (group, f);     // the key decides from now on
    }

    void FXGrid::mouseDown (const MouseEvent& e)
    {
        const auto pos = e.position;
        for (int g = 0; g < kNumGroups; ++g)
            if (groupBounds (g).contains (pos)) { showMacros (false); setGroup (g); return; }
        if (macroTabBounds().contains (pos)) { showMacros (! macroPage); return; }

        if (macroPage)
        {
            for (int m = 0; m < macro::kNumMacros; ++m)
                if (macroKeyBounds (m).contains (pos))
                {
                    if (processor.getParameters().getRawParameterValue (params::macroLatch)->load() > 0.5f)
                    {
                        const bool active = ((processor.getBridge().macroActive.load() >> m) & 1u) != 0;
                        setMacroParam (m, ! active);                  // LATCH: toggle
                    }
                    else
                    {
                        heldMacro = m;
                        setMacroParam (m, true);                      // hold
                    }
                    repaint();
                    return;
                }
            return;
        }

        for (int f = 0; f < fx::kNumFx; ++f)
            if (keyBounds (f).contains (pos))
            {
                setSelected (f);
                if (e.mods.isPopupMenu()) return;          // right-click: edit only

                if (processor.isFxLocked (f))
                {
                    const bool paramOn = processor.getParameters().getRawParameterValue (fx::paramId (group, f))->load() > 0.5f;
                    setFxParam (f, ! (paramOn || isOn (f)));
                }
                else
                {
                    held = f;
                    setFxParam (f, true);
                }
                repaint();
                return;
            }
    }

    void FXGrid::mouseUp (const MouseEvent&)
    {
        if (heldMacro >= 0)
        {
            setMacroParam (heldMacro, false);
            heldMacro = -1;
            repaint();
        }
        if (held >= 0)
        {
            setFxParam (held, false);
            held = -1;
            repaint();
        }
    }

    void FXGrid::refresh()
    {
        auto& br = processor.getBridge();
        const auto engaged = br.fxEngaged[(size_t) group].load();
        const auto armed   = br.fxArmed[(size_t) group].load();
        uint32_t locked = 0;
        for (int f = 0; f < fx::kNumFx; ++f) if (processor.isFxLocked (f)) locked |= 1u << f;
        const bool blink = (Time::getMillisecondCounter() / 125) % 2 == 0;

        const int g = processor.getEngine().getFxTargetGroup();
        if (g != group) { group = g; repaint(); if (onSelectionChanged) onSelectionChanged (group, selected); }

        const auto macros = br.macroActive.load();
        if (macros != shownMacros) { shownMacros = macros; repaint(); }
        if (macroPage) { amountRead.repaint(); groupRead.repaint(); }

        if (engaged != shownEngaged || armed != shownArmed || locked != shownLocked || (armed != 0 && blink != blinkPhase))
        {
            shownEngaged = engaged; shownArmed = armed; shownLocked = locked; blinkPhase = blink;
            repaint();
        }
    }

    void FXGrid::resized()
    {
        // Static bodies of the keys (grime etc.), baked once.
        const auto b = getLocalBounds();
        if (b.isEmpty()) return;
        {
            auto row = b.withTrimmedTop (b.getHeight() - 40);
            amountRead.setBounds (row.removeFromLeft (64));
            row.removeFromLeft (8);
            groupRead.setBounds (row.removeFromLeft (52));
            row.removeFromLeft (10);
            latchKey.setBounds (row.withTrimmedTop (10));
        }
        const float s = 2.0f;
        cache = Image (Image::ARGB, roundToInt (b.getWidth() * s), roundToInt (b.getHeight() * s), true);
        Graphics g (cache);
        g.addTransform (AffineTransform::scale (s));
        Random rnd (606);
        for (int f = 0; f < fx::kNumFx; ++f)
        {
            const auto key = keyBounds (f);
            paint::plate (g, key, Colour (0xff1e1f1d), 3.0f);
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (key.toNearestInt());
            paint::ageArea (g, key, rnd, 0.9f);
        }
    }

    void FXGrid::paint (Graphics& g)
    {
        // Group selector.
        for (int k = 0; k < kNumGroups; ++k)
        {
            const auto r = groupBounds (k);
            paint::plate (g, r, k == group ? Colour (0xff3a3226) : Colour (0xff1d1e1b), 2.0f);
            paint::lamp (g, { r.getX() + 9.0f, r.getCentreY() }, 3.5f, c::amber, c::amberDim, k == group ? 1.0f : 0.12f);
            g.setColour (c::plateText.withAlpha (k == group ? 1.0f : 0.6f));
            g.setFont (industrialFont (12.5f));
            g.drawText (String::charToString ((juce_wchar) ('A' + k)), r.withTrimmedLeft (14.0f), Justification::centred, false);
        }
        {
            const auto r = macroTabBounds();
            const bool lit = shownMacros != 0;
            paint::plate (g, r, macroPage ? Colour (0xff3a2622) : Colour (0xff1d1e1b), 2.0f);
            paint::lamp (g, { r.getX() + 9.0f, r.getCentreY() }, 3.5f, c::red, c::redDim, lit ? 1.0f : (macroPage ? 0.35f : 0.12f));
            g.setColour (c::plateText.withAlpha (macroPage ? 1.0f : 0.6f));
            g.setFont (industrialFont (12.5f));
            g.drawText ("MACRO", r.withTrimmedLeft (14.0f), Justification::centred, false);
        }

        if (macroPage)
        {
            for (int m = 0; m < macro::kNumMacros; ++m)
            {
                const auto key = macroKeyBounds (m);
                const bool lit = (shownMacros >> m) & 1u;
                paint::plate (g, key, Colour (0xff221d1b), 3.0f);
                auto lampWin = key.reduced (5.0f).removeFromTop (7.0f);
                g.setColour (lit ? c::red : c::redDim.darker (0.5f));
                g.fillRoundedRectangle (lampWin, 2.0f);
                if (lit)
                {
                    g.setColour (c::red.withAlpha (0.16f));
                    g.fillRoundedRectangle (key.reduced (2.0f), 3.0f);
                }
                g.setColour (c::plateText.withAlpha (lit ? 1.0f : 0.85f));
                g.setFont (industrialFont (12.0f));
                g.drawFittedText (macro::def (m).label, key.reduced (3.0f).withTrimmedTop (10.0f).toNearestInt(),
                                  Justification::centred, 2, 0.7f);
            }
            return;
        }

        g.drawImage (cache, getLocalBounds().toFloat());

        for (int f = 0; f < fx::kNumFx; ++f)
        {
            const auto key = keyBounds (f);
            const bool on    = (shownEngaged >> f) & 1u;
            const bool armed = (shownArmed >> f) & 1u;
            const bool lit   = on || (armed && blinkPhase);

            // Lamp window across the top of the key.
            auto lampWin = key.reduced (5.0f).removeFromTop (7.0f);
            g.setColour (lit ? c::amber : c::amberDim.darker (0.4f));
            g.fillRoundedRectangle (lampWin, 2.0f);
            if (lit)
            {
                g.setColour (c::amber.withAlpha (0.18f));
                g.fillRoundedRectangle (key.reduced (2.0f), 3.0f);
            }

            g.setColour (c::plateText.withAlpha (lit ? 1.0f : 0.8f));
            g.setFont (industrialFont (jmin (14.0f, key.getHeight() * 0.32f)));
            g.drawFittedText (fx::def (f).label, key.reduced (3.0f).withTrimmedTop (10.0f).toNearestInt(),
                              Justification::centred, 1, 0.6f);

            // L / M mode tag.
            g.setColour (c::bakelite.withAlpha (0.55f));
            g.setFont (monoFont (9.0f));
            g.drawText (((shownLocked >> f) & 1u) ? "L" : "M", key.reduced (4.0f, 2.0f).removeFromBottom (10.0f),
                        Justification::bottomRight, false);

            if (f == selected)
            {
                g.setColour (c::vfd.withAlpha (0.9f));
                g.drawRoundedRectangle (key.expanded (1.5f), 3.5f, 1.5f);
            }
        }
    }
}
