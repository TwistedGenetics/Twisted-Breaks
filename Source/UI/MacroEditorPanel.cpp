#include "UI/MacroEditorPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr int kRowTop = 116, kRowH = 82;
    }

    MacroEditorPanel::MacroEditorPanel (TwistedBreaksProcessor& p) : processor (p), recipes (p.getBridge().macros)
    {
        for (int m = 0; m < MacroRecipes::kMacros; ++m)
        {
            macroKeys[(size_t) m] = std::make_unique<PushKey> (String (m + 1), "Edit macro " + String (m + 1));
            macroKeys[(size_t) m]->onClick = [this, m] { setMacro (m); };
            addAndMakeVisible (*macroKeys[(size_t) m]);
        }
        nameBox.setFont (monoFont (13.0f));
        nameBox.setColour (TextEditor::backgroundColourId, c::plateBlack);
        nameBox.setColour (TextEditor::textColourId, c::vfd);
        nameBox.setColour (TextEditor::outlineColourId, c::steelEdge.withAlpha (0.5f));
        nameBox.setInputRestrictions (24);
        nameBox.onTextChange = [this] { recipes.setName (macro, nameBox.getText().toUpperCase()); processor.markStateChanged(); };
        addAndMakeVisible (nameBox);

        hearKey.addMouseListener (this, false);
        resetKey.onClick = [this] { recipes.resetToFactory (macro); processor.markStateChanged(); rebuild(); };
        resetAll.onClick = [this] { for (int m = 0; m < MacroRecipes::kMacros; ++m) recipes.resetToFactory (m); processor.markStateChanged(); rebuild(); };
        saveKey.onClick = [this]
        {
            const auto dir = File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Twisted Breaks").getChildFile ("Macros");
            dir.createDirectory();
            chooser = std::make_unique<FileChooser> ("Save macro", dir.getChildFile (File::createLegalFileName (recipes.getName (macro).replace ("\n", " ")) + ".tbmacro"), "*.tbmacro");
            chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f == File()) return;
                if (auto xml = recipes.toValueTree (macro).createXml()) xml->writeTo (f.withFileExtension ("tbmacro"));
            });
        };
        loadKey.onClick = [this]
        {
            chooser = std::make_unique<FileChooser> ("Load a macro into " + String (macro + 1),
                                                     File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Twisted Breaks").getChildFile ("Macros"), "*.tbmacro");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
            {
                if (auto xml = parseXML (fc.getResult()))
                {
                    recipes.fromValueTree (macro, ValueTree::fromXml (*xml));
                    processor.markStateChanged();
                    rebuild();
                }
            });
        };
        useKey.setClickingTogglesState (false);
        useKey.onClick = [this] { recipes.setUsesFx (macro, fxSel, ! recipes.usesFx (macro, fxSel)); processor.markStateChanged(); rebuild(); };
        for (auto* k : { &hearKey, &resetKey, &resetAll, &saveKey, &loadKey, &useKey }) addAndMakeVisible (k);

        for (int k = 0; k < 3; ++k)
        {
            setKeys[(size_t) k].onClick = [this, k]
            {
                recipes.setSetsParam (macro, fxSel, k, ! recipes.setsParam (macro, fxSel, k));
                if (recipes.setsParam (macro, fxSel, k)) recipes.setUsesFx (macro, fxSel, true);
                processor.markStateChanged();
                rebuild();
            };
            addAndMakeVisible (setKeys[(size_t) k]);
            for (auto* kn : { &mildKnobs[(size_t) k], &hardKnobs[(size_t) k] })
            {
                kn->setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
                kn->setTextBoxStyle (Slider::TextBoxBelow, true, 84, 16);
                addAndMakeVisible (*kn);
            }
            mildKnobs[(size_t) k].onValueChange = [this, k] { recipes.setMild (macro, fxSel, k, (float) mildKnobs[(size_t) k].getValue()); processor.markStateChanged(); };
            hardKnobs[(size_t) k].onValueChange = [this, k] { recipes.setHard (macro, fxSel, k, (float) hardKnobs[(size_t) k].getValue()); processor.markStateChanged(); };
        }
        rebuild();
    }

    MacroEditorPanel::~MacroEditorPanel() { if (hearing) hear (false); }

    void MacroEditorPanel::setMacro (int m)
    {
        if (hearing) hear (false);
        macro = jlimit (0, MacroRecipes::kMacros - 1, m);
        // Start on the first effect it uses.
        for (int f = 0; f < MacroRecipes::kFx; ++f) if (recipes.usesFx (macro, f)) { fxSel = f; break; }
        rebuild();
    }

    void MacroEditorPanel::setFx (int f) { fxSel = jlimit (0, MacroRecipes::kFx - 1, f); rebuild(); }

    void MacroEditorPanel::hear (bool on)
    {
        hearing = on;
        if (auto* r = processor.getParameters().getParameter (macro::paramId (macro)))
        { r->beginChangeGesture(); r->setValueNotifyingHost (on ? 1.0f : 0.0f); r->endChangeGesture(); }
    }

    void MacroEditorPanel::mouseDown (const MouseEvent& e)
    {
        if (e.eventComponent == &hearKey)
        {
            hear (true);
            return;
        }
        if (e.eventComponent != this) return;
        for (int f = 0; f < MacroRecipes::kFx; ++f)
            if (fxChip (f).contains (e.position))
            {
                if (e.mods.isPopupMenu() || e.getNumberOfClicks() > 1)
                {
                    recipes.setUsesFx (macro, f, ! recipes.usesFx (macro, f));
                    processor.markStateChanged();
                }
                setFx (f);
                return;
            }
    }

    Rectangle<float> MacroEditorPanel::fxChip (int f) const
    {
        return { 14.0f + (float) (f % 4) * 64.0f, (float) kRowTop + (float) (f / 4) * 46.0f, 60.0f, 40.0f };
    }

    void MacroEditorPanel::rebuild()
    {
        for (int m = 0; m < MacroRecipes::kMacros; ++m)
            macroKeys[(size_t) m]->setToggleState (m == macro, dontSendNotification);
        if (! nameBox.hasKeyboardFocus (true)) nameBox.setText (recipes.getName (macro).replace ("\n", " "), false);
        const auto& d = fx::def (fxSel);
        useKey.setToggleState (recipes.usesFx (macro, fxSel), dontSendNotification);
        for (int k = 0; k < 3; ++k)
        {
            const bool exists = k < d.numParams;
            setKeys[(size_t) k].setVisible (exists);
            mildKnobs[(size_t) k].setVisible (exists);
            hardKnobs[(size_t) k].setVisible (exists);
            if (! exists) continue;
            const auto& pd = d.params[k];
            const bool on = recipes.setsParam (macro, fxSel, k);
            setKeys[(size_t) k].setToggleState (on, dontSendNotification);
            setKeys[(size_t) k].setButtonText (on ? "SET" : "LEAVE");
            for (auto* kn : { &mildKnobs[(size_t) k], &hardKnobs[(size_t) k] })
            {
                if (pd.isChoice()) kn->setRange (0.0, (double) jmax (1, pd.numChoices - 1), 1.0);
                else               kn->setRange (0.0, 1.0, 0.001);
                kn->textFromValueFunction = [&pd] (double v) { return fx::valueText (pd, (float) v); };
                kn->setEnabled (on);
                kn->setAlpha (on ? 1.0f : 0.35f);
            }
            mildKnobs[(size_t) k].setValue (recipes.getMild (macro, fxSel, k), dontSendNotification);
            hardKnobs[(size_t) k].setValue (recipes.getHard (macro, fxSel, k), dontSendNotification);
            mildKnobs[(size_t) k].updateText();
            hardKnobs[(size_t) k].updateText();
        }
        shownVersion = recipes.getVersion();
        repaint();
    }

    void MacroEditorPanel::refresh()
    {
        if (hearing && ! hearKey.isMouseButtonDown()) hear (false);
        if (recipes.getVersion() != shownVersion && ! isMouseButtonDown (true)) rebuild();
        hearKey.setToggleState (hearing, dontSendNotification);
    }

    void MacroEditorPanel::resized()
    {
        for (int m = 0; m < MacroRecipes::kMacros; ++m) macroKeys[(size_t) m]->setBounds (150 + m * 40, 10, 36, 24);
        nameBox.setBounds (14, 44, 150, 24);
        hearKey.setBounds (170, 44, 104, 24);
        int x = getWidth() - 14;
        for (auto [k, w] : { std::pair<PushKey*, int> { &loadKey, 56 }, { &saveKey, 56 }, { &resetAll, 86 }, { &resetKey, 60 } })
        {
            x -= w;
            k->setBounds (x, 44, w, 24);
            x -= 4;
        }
        useKey.setBounds (getWidth() - 14 - 120, 82, 120, 24);
        for (int k = 0; k < 3; ++k)
        {
            const int y = kRowTop + 4 + k * kRowH;
            setKeys[(size_t) k].setBounds (296, y + 20, 62, 24);
            mildKnobs[(size_t) k].setBounds (366, y, 88, kRowH - 6);
            hardKnobs[(size_t) k].setBounds (462, y, 88, kRowH - 6);
        }
        background = {};
    }

    void MacroEditorPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random r (515);
            const auto all = getLocalBounds().toFloat();
            paint::plate (bg, all, c::steel, 4.0f);
            {
                Graphics::ScopedSaveState ss (bg);
                bg.reduceClipRegion (getLocalBounds());
                paint::ageArea (bg, all, r, 1.2f);
                paint::edgeWear (bg, all, r, 1.4f);
            }
            const auto rr = all.reduced (9.0f);
            for (auto pt : { rr.getTopLeft(), rr.getTopRight(), rr.getBottomLeft(), rr.getBottomRight() })
                paint::rivet (bg, pt, 4.2f, r);
            paint::labelPlate (bg, { 18.0f, 10.0f, 124.0f, 22.0f }, "MACRO EDITOR", {}, paint::PlateStyle::bakelite, r);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        // The effects: lit = in this macro, outlined = being edited.
        for (int f = 0; f < MacroRecipes::kFx; ++f)
        {
            const auto r = fxChip (f);
            const bool used = recipes.usesFx (macro, f);
            paint::plate (g, r, used ? Colour (0xff3a3226) : Colour (0xff1d1e1b), 3.0f);
            paint::lamp (g, { r.getX() + 8.0f, r.getY() + 8.0f }, 3.2f, c::amber, c::amberDim, used ? 1.0f : 0.1f);
            g.setColour (c::plateText.withAlpha (used ? 1.0f : 0.55f));
            g.setFont (industrialFont (12.0f));
            g.drawText (fx::def (f).label, r.withTrimmedTop (8.0f), Justification::centred, false);
            if (f == fxSel)
            {
                g.setColour (c::amber);
                g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.5f);
            }
        }
        g.setFont (monoFont (10.0f));
        g.setColour (c::bakelite.withAlpha (0.65f));
        g.drawText ("CLICK: EDIT   DOUBLE-CLICK: IN / OUT", Rectangle<float> (14.0f, (float) kRowTop + 4.0f * 46.0f, 260.0f, 18.0f), Justification::centredLeft, false);

        // The edited effect.
        const auto& d = fx::def (fxSel);
        g.setFont (industrialFont (12.0f));
        g.setColour (c::plateText);
        g.drawFittedText (d.name, Rectangle<int> (296, 82, getWidth() - 444, 24), Justification::centredLeft, 1, 0.7f);
        g.setFont (monoFont (10.0f));
        g.setColour (c::bakelite.withAlpha (0.7f));
        g.drawText ("MILD", Rectangle<float> (366.0f, (float) kRowTop - 10.0f, 88.0f, 12.0f), Justification::centred, false);
        g.drawText ("HARD", Rectangle<float> (462.0f, (float) kRowTop - 10.0f, 88.0f, 12.0f), Justification::centred, false);
        for (int k = 0; k < d.numParams; ++k)
        {
            const int y = kRowTop + 4 + k * kRowH;
            g.setFont (industrialFont (12.0f));
            g.setColour (c::plateText);
            g.drawText (d.params[k].label, Rectangle<float> (296.0f, (float) y, 66.0f, 18.0f), Justification::centredLeft, false);
        }
        const int amt = roundToInt (processor.getParameters().getRawParameterValue (params::macroAmount)->load());
        g.setFont (monoFont (10.0f));
        g.setColour (c::vfd.withAlpha (0.85f));
        g.drawText ("MACRO AMOUNT " + String (amt) + " %: THE MACRO PLAYS " + String (amt) + " % OF THE WAY FROM MILD TO HARD",
                    Rectangle<float> (14.0f, (float) getHeight() - 30.0f, (float) getWidth() - 28.0f, 18.0f), Justification::centredLeft, true);
    }
}
