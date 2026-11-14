#include "UI/ManglePanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    using namespace tb::mangle;
    namespace c = colours;

    namespace
    {
        const Rectangle<float> kGrid { 16.0f, 44.0f, 540.0f, 86.0f };
        constexpr float kMovesY = 146.0f, kMovesH = 26.0f;     // two rows: 5 + 4 moves
        constexpr float kReadY = 206.0f, kReadH = 36.0f;       // the bar's settings
        constexpr float kTexY = 248.0f, kSpaceY = 290.0f;      // TEXTURE, SPACE rows (0.30)
        constexpr float kBottomY = 340.0f;
    }

    Colour ManglePanel::moveColour (int m)
    {
        switch (m)
        {
            case repeat:  return Colour (0xffffa928);   // amber
            case reverse: return Colour (0xff4fd8ff);   // cyan
            case half:    return Colour (0xff6f8dff);   // blue
            case stop:    return Colour (0xffff4a3a);   // red
            case dbl:     return Colour (0xff7dff5a);   // green
            case scratch: return Colour (0xffff5ad7);   // magenta
            case freeze:  return Colour (0xffe8f4ff);   // ice
            case shuffle: return Colour (0xffe1ff3a);   // lime
            default:      return Colour (0xff3a3f3b);   // THRU
        }
    }

    ManglePanel::ManglePanel (TwistedBreaksProcessor& p) : processor (p)
    {
        auto& apvts = processor.getParameters();
        for (int m = 0; m < kNumMoves; ++m)
        {
            moveKeys[(size_t) m] = std::make_unique<PushKey> (kMoveNames[m], String ("Paint ") + kMoveNames[m] + " onto steps", moveColour (m));
            moveKeys[(size_t) m]->setClickingTogglesState (false);
            moveKeys[(size_t) m]->onClick = [this, m] { setBrush (m); };
            addAndMakeVisible (*moveKeys[(size_t) m]);
        }
        setBrush (repeat);

        onKey.setClickingTogglesState (true);
        if (auto* prm = apvts.getParameter (params::mglOn))
            onAtt = std::make_unique<ButtonParameterAttachment> (*prm, onKey);
        holdKey.addMouseListener (this, false);
        fillKey.onClick = [this]
        {
            setParam (params::mglFillNext, 1.0f);
            fillOffTicks = 3;                       // back to 0 a moment later: the engine sees one rising edge
        };
        backKey.onClick = [this] { if (onBack) onBack(); };
        randKey.onClick = [this] { processor.getMangleOps().randomize(); repaint(); };
        mutKey.onClick  = [this] { processor.getMangleOps().mutate(); repaint(); };
        revertKey.onClick = [this] { processor.getMangleOps().revert(); repaint(); };
        for (auto* k : { &onKey, &holdKey, &fillKey, &backKey, &randKey, &mutKey, &revertKey }) addAndMakeVisible (k);

        stepsRead.attach (apvts.getParameter (params::mglSteps));
        targetRead.attach (apvts.getParameter (params::mglTarget));
        mixRead.attach (apvts.getParameter (params::mglMix));
        amountRead.attach (apvts.getParameter (params::mglAmount));
        everyRead.attach (apvts.getParameter (params::mglFillEvery));
        lenRead.attach (apvts.getParameter (params::mglFillLen));
        styleRead.attach (apvts.getParameter (params::mglStyle));
        for (auto* r : { &stepsRead, &targetRead, &mixRead, &amountRead, &everyRead, &lenRead, &styleRead }) addAndMakeVisible (r);

        // 0.30
        for (int k = 0; k < mangle::kSlots; ++k)
        {
            slotKeys[(size_t) k] = std::make_unique<PushKey> (String::charToString ((juce_wchar) ('A' + k)),
                                                              "Variation " + String::charToString ((juce_wchar) ('A' + k))
                                                                + ": edit it (it plays from the next bar). Right-click: copy here / clear",
                                                              Colour (0xff6f8dff));
            slotKeys[(size_t) k]->setClickingTogglesState (false);
            slotKeys[(size_t) k]->onClick = [this, k] { setParam (params::mglSlot, (float) k); repaint(); };
            slotKeys[(size_t) k]->addMouseListener (this, false);
            addAndMakeVisible (*slotKeys[(size_t) k]);
        }
        for (auto* k : { &chainKey, &texLock, &spaceLock }) k->setClickingTogglesState (true);
        if (auto* prm = apvts.getParameter (params::mglChain))     chainAtt = std::make_unique<ButtonParameterAttachment> (*prm, chainKey);
        if (auto* prm = apvts.getParameter (params::mglLockTex))   texLockAtt = std::make_unique<ButtonParameterAttachment> (*prm, texLock);
        if (auto* prm = apvts.getParameter (params::mglLockSpace)) spaceLockAtt = std::make_unique<ButtonParameterAttachment> (*prm, spaceLock);
        presetKey.onClick = [this] { showPresetMenu(); };
        for (auto* k : { &chainKey, &texLock, &spaceLock, &presetKey }) addAndMakeVisible (k);
        texMode.attach (apvts.getParameter (params::mglTexMode));
        texAmount.attach (apvts.getParameter (params::mglTexAmount));
        texSize.attach (apvts.getParameter (params::mglTexSize));
        texDensity.attach (apvts.getParameter (params::mglTexDensity));
        texScatter.attach (apvts.getParameter (params::mglTexScatter));
        spMode.attach (apvts.getParameter (params::mglSpaceMode));
        spTime.attach (apvts.getParameter (params::mglSpaceTime));
        spFb.attach (apvts.getParameter (params::mglSpaceFb));
        spAmount.attach (apvts.getParameter (params::mglSpaceAmount));
        for (auto* r : { &texMode, &texAmount, &texSize, &texDensity, &texScatter, &spMode, &spTime, &spFb, &spAmount }) addAndMakeVisible (r);

        processor.getMangleOps().onChange = [sp = Component::SafePointer<ManglePanel> (this)] { if (sp != nullptr) sp->repaint(); };
        startTimerHz (30);
    }

    ManglePanel::~ManglePanel()
    {
        processor.getMangleOps().onChange = nullptr;
    }

    void ManglePanel::setBrush (int m)
    {
        brush = jlimit (0, (int) kNumMoves - 1, m);
        for (int k = 0; k < kNumMoves; ++k) moveKeys[(size_t) k]->setToggleState (k == brush, dontSendNotification);
    }

    void ManglePanel::setParam (const char* id, float v)
    {
        if (auto* r = processor.getParameters().getParameter (id))
        {
            r->beginChangeGesture();
            r->setValueNotifyingHost (r->convertTo0to1 (v));
            r->endChangeGesture();
        }
    }

    void ManglePanel::resized()
    {
        background = {};
        onKey.setBounds (124, 8, 48, 26);
        holdKey.setBounds (176, 8, 58, 26);
        fillKey.setBounds (238, 8, 84, 26);
        for (int k = 0; k < mangle::kSlots; ++k) slotKeys[(size_t) k]->setBounds (330 + k * 32, 8, 30, 26);
        chainKey.setBounds (460, 8, 58, 26);
        backKey.setBounds (522, 8, 40, 26);
        const float mw = kGrid.getWidth() / 5.0f;
        for (int m = 0; m < kNumMoves; ++m)
        {
            const int row = m < 5 ? 0 : 1, col = m < 5 ? m : m - 5;
            moveKeys[(size_t) m]->setBounds (Rectangle<float> (kGrid.getX() + col * mw + 2.0f, kMovesY + row * (kMovesH + 4.0f),
                                                               mw - 4.0f, kMovesH).toNearestInt());
        }
        const float rw = kGrid.getWidth() / 6.0f;
        int i = 0;
        for (auto* r : { &stepsRead, &targetRead, &mixRead, &amountRead, &everyRead, &lenRead })
            r->setBounds (Rectangle<float> (kGrid.getX() + (i++) * rw + 4.0f, kReadY, rw - 8.0f, kReadH).toNearestInt());
        // TEXTURE and SPACE rows: the lock key as the row's name, then its settings.
        const float cw = (kGrid.getWidth() - 92.0f) / 5.0f;
        texLock.setBounds (16, (int) kTexY + 8, 86, 28);
        i = 0;
        for (auto* r : { &texMode, &texAmount, &texSize, &texDensity, &texScatter })
            r->setBounds (Rectangle<float> (108.0f + (i++) * cw, kTexY, cw - 6.0f, kReadH).toNearestInt());
        spaceLock.setBounds (16, (int) kSpaceY + 8, 86, 28);
        i = 0;
        for (auto* r : { &spMode, &spTime, &spFb, &spAmount })
            r->setBounds (Rectangle<float> (108.0f + (i++) * cw, kSpaceY, cw - 6.0f, kReadH).toNearestInt());
        presetKey.setBounds ((int) (108.0f + 4 * cw), (int) kSpaceY + 8, (int) cw - 6, 28);

        randKey.setBounds (16, (int) kBottomY + 8, 108, 30);
        mutKey.setBounds (128, (int) kBottomY + 8, 86, 30);
        revertKey.setBounds (218, (int) kBottomY + 8, 86, 30);
        styleRead.setBounds (310, (int) kBottomY, 86, 38);
    }

    int ManglePanel::stepAt (Point<float> pt) const
    {
        if (! kGrid.expanded (0.0f, 4.0f).contains (pt)) return -1;
        const int S = processor.getMangleOps().steps();
        return jlimit (0, S - 1, (int) ((pt.x - kGrid.getX()) / kGrid.getWidth() * S));
    }

    Rectangle<float> ManglePanel::stepRect (int s) const
    {
        const int S = processor.getMangleOps().steps();
        const float w = kGrid.getWidth() / (float) S;
        return { kGrid.getX() + s * w, kGrid.getY(), w, kGrid.getHeight() };
    }

    Rectangle<float> ManglePanel::runRect (int s0) const
    {
        const auto pat = processor.getMangleOps().get();
        const int S = processor.getMangleOps().steps();
        const int n = pat.runLength (s0, S);
        return stepRect (s0).getUnion (stepRect (s0 + n - 1));
    }

    void ManglePanel::paint (Graphics& g)
    {
        if (background.isNull() && getWidth() > 0)
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random r (1515);
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
            paint::labelPlate (bg, { 18.0f, 10.0f, 100.0f, 22.0f }, "MANGLE", {}, paint::PlateStyle::bakelite, r);
            paint::recess (bg, kGrid.expanded (4.0f), 4.0f);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        auto& ops = processor.getMangleOps();
        const auto pat = ops.get();
        const int S = ops.steps();
        const int playing = processor.getBridge().mangleStep.load (std::memory_order_relaxed);
        const bool active = processor.getBridge().mangleActive.load (std::memory_order_relaxed);

        // The bar: one cell per run, in its move's colour, big dark text on the bright cells.
        for (int s = 0; s < S; )
        {
            const int n = pat.runLength (s, S);
            const int mv = pat.move[(size_t) s];
            auto r = runRect (s).reduced (2.0f, 2.0f);
            const bool painting = dragFrom >= 0 && s <= jmax (dragFrom, dragTo) && s + n - 1 >= jmin (dragFrom, dragTo);
            const auto col = moveColour (mv);
            g.setColour (mv == thru ? Colour (0xff222624) : col.withMultipliedBrightness (0.92f));
            g.fillRoundedRectangle (r, 4.0f);
            if (painting) { g.setColour (moveColour (brush).withAlpha (0.55f)); g.fillRoundedRectangle (r, 4.0f); }
            g.setColour (Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (r, 4.0f, 1.5f);

            const bool narrow = r.getWidth() < 44.0f;
            const String name = mv == thru ? String ("-") : String (narrow ? kMoveShort[mv] : kMoveNames[mv]);
            g.setColour (mv == thru ? c::plateText.withAlpha (0.55f) : Colours::black);
            g.setFont (industrialFont (narrow ? 13.0f : 17.0f).boldened());
            g.drawFittedText (name, r.toNearestInt().withTrimmedBottom ((int) (r.getHeight() * 0.35f)), Justification::centred, 1, 0.7f);
            String argText;
            if (mv == repeat)  argText = "x" + String (kRepeatSlices[jlimit (0, 2, (int) pat.arg[(size_t) s])]);
            if (mv == shuffle) argText = "<" + String (jlimit (0, S - 1, (int) pat.arg[(size_t) s]) + 1);
            if (argText.isNotEmpty())
            {
                g.setFont (monoFont (narrow ? 13.0f : 16.0f).boldened());
                g.drawText (argText, r.withTrimmedTop (r.getHeight() * 0.55f), Justification::centredTop, false);
            }
            s += n;
        }
        // Step numbers and the playhead.
        for (int s = 0; s < S; ++s)
        {
            const auto r = stepRect (s);
            g.setColour (c::bakelite.withAlpha (0.75f));
            g.setFont (monoFont (11.0f));
            g.drawText (String (s + 1), Rectangle<float> (r.getX(), r.getBottom() + 1.0f, r.getWidth(), 11.0f), Justification::centred, false);
            if (s == playing)
            {
                g.setColour ((active ? c::red : c::green).withAlpha (0.95f));
                g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 3.0f);
            }
        }

        // Status (and the last RANDOMIZE / preset), bottom right.
        const bool on = processor.getParameters().getRawParameterValue (params::mglOn)->load() > 0.5f;
        const int playingSlot = processor.getBridge().mangleSlot.load (std::memory_order_relaxed);
        const String status = playing < 0 ? String ("LIVE STOPPED") : active ? "MANGLING " + String::charToString ((juce_wchar) ('A' + playingSlot))
                            : on ? String ("WAITING FOR THE FILL") : String ("THRU");
        g.setColour (active ? c::red : c::vfd);
        g.setFont (monoFont (13.5f).boldened());
        g.drawText (status, Rectangle<float> (404.0f, kBottomY + 2.0f, 152.0f, 18.0f), Justification::centredLeft, false);
        g.setColour (c::vfd);
        g.setFont (monoFont (12.0f));
        const String report = ops.isBusy() ? String ("LISTENING...") : ops.getLastReport().toUpperCase();
        g.drawFittedText (report, Rectangle<int> (404, (int) kBottomY + 20, 152, 18), Justification::centredLeft, 1);
    }

    void ManglePanel::timerCallback()
    {
        auto& b = processor.getBridge();
        const int st = b.mangleStep.load (std::memory_order_relaxed);
        const bool ac = b.mangleActive.load (std::memory_order_relaxed);
        const auto v = b.mangle.getVersion();
        if (fillOffTicks > 0 && --fillOffTicks == 0) setParam (params::mglFillNext, 0.0f);
        const bool hold = processor.getParameters().getRawParameterValue (params::mglHold)->load() > 0.5f;
        holdKey.setToggleState (hold, dontSendNotification);
        fillKey.setToggleState (fillOffTicks > 0, dontSendNotification);
        const int edit = processor.getMangleOps().slot();
        for (int k = 0; k < mangle::kSlots; ++k)
        {
            slotKeys[(size_t) k]->setToggleState (k == edit, dontSendNotification);
            slotKeys[(size_t) k]->setBlinking (ac && k == b.mangleSlot.load (std::memory_order_relaxed) && k != edit);
            slotKeys[(size_t) k]->tick();
        }
        if (edit != shownSlot) { shownSlot = edit; repaint(); }
        if (st != shownStep || ac != shownActive || v != shownVersion || processor.getMangleOps().isBusy())
        {
            shownStep = st; shownActive = ac; shownVersion = v;
            repaint();
        }
    }

    void ManglePanel::mouseDown (const MouseEvent& e)
    {
        if (e.eventComponent == &holdKey) { setParam (params::mglHold, 1.0f); return; }
        for (int k = 0; k < mangle::kSlots; ++k)
            if (e.eventComponent == slotKeys[(size_t) k].get() && e.mods.isPopupMenu()) { showSlotMenu (k); return; }
        if (e.eventComponent != this) return;
        const int s = stepAt (e.position);
        if (s < 0) return;
        if (e.mods.isPopupMenu()) { showStepMenu (s); return; }
        dragFrom = dragTo = s;
        dragJoin = ! e.mods.isAltDown();
        repaint();
    }

    void ManglePanel::mouseDrag (const MouseEvent& e)
    {
        if (e.eventComponent != this || dragFrom < 0) return;
        const int s = stepAt (e.position.withY (kGrid.getCentreY()));
        if (s >= 0 && s != dragTo) { dragTo = s; repaint(); }
    }

    void ManglePanel::mouseUp (const MouseEvent& e)
    {
        if (e.eventComponent == &holdKey) { setParam (params::mglHold, 0.0f); return; }
        if (dragFrom < 0) return;
        auto& ops = processor.getMangleOps();
        const int a = jmin (dragFrom, dragTo), b = jmax (dragFrom, dragTo);
        const auto pat = ops.get();
        // Clicking a single step that already has this move: back to THRU (a toggle).
        if (a == b && pat.move[(size_t) pat.runStart (a)] == brush && pat.runLength (pat.runStart (a), ops.steps()) == 1)
            ops.clearStep (a);
        else
            ops.paint (a, b, brush, dragJoin && b > a);
        dragFrom = dragTo = -1;
        repaint();
    }

    void ManglePanel::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
    {
        const int s = stepAt (e.position);
        if (s < 0) return;
        processor.getMangleOps().nudgeArg (s, w.deltaY > 0.0f ? 1 : -1);
        repaint();
    }

    void ManglePanel::showStepMenu (int s)
    {
        auto& ops = processor.getMangleOps();
        const auto pat = ops.get();
        const int s0 = pat.runStart (s);
        const int mv = pat.move[(size_t) s0];
        PopupMenu m;
        m.addSectionHeader ("STEP " + String (s + 1) + ": " + kMoveNames[mv]);
        m.addItem ("THRU (clear)", [&ops, s] { ops.clearStep (s); });
        m.addItem ("Split this run", pat.runLength (s0, ops.steps()) > 1, false, [&ops, s] { ops.split (s); });
        if (mv == repeat)
        {
            PopupMenu sl;
            for (int k = 0; k < 3; ++k) sl.addItem (String (kRepeatSlices[k]) + " slices", true, pat.arg[(size_t) s0] == k, [&ops, s0, k] { ops.setArg (s0, k); });
            m.addSubMenu ("REPEAT slices", sl);
        }
        if (mv == shuffle)
        {
            PopupMenu src;
            for (int k = 0; k < ops.steps(); ++k) src.addItem ("Step " + String (k + 1) + " of the last bar", true, pat.arg[(size_t) s0] == k, [&ops, s0, k] { ops.setArg (s0, k); });
            m.addSubMenu ("SHUFFLE plays", src);
        }
        m.addSeparator();
        m.addItem ("Clear the whole bar", [&ops] { ops.set ({}, true); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (runRect (s0).toNearestInt())));
    }

    void ManglePanel::showSlotMenu (int k)
    {
        auto& ops = processor.getMangleOps();
        const int cur = ops.slot();
        const String name = String::charToString ((juce_wchar) ('A' + k));
        PopupMenu m;
        m.addSectionHeader ("VARIATION " + name);
        m.addItem ("Copy " + String::charToString ((juce_wchar) ('A' + cur)) + " here", k != cur, false, [&ops, cur, k] { ops.copySlot (cur, k); });
        m.addItem ("Clear " + name, [&ops, k] { ops.setSlot (k, {}, true); });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (slotKeys[(size_t) k].get()));
    }

    void ManglePanel::showPresetMenu()
    {
        auto& ops = processor.getMangleOps();
        PopupMenu m;
        StringArray groups;
        for (auto& p : MangleOps::factoryPresets()) groups.addIfNotAlreadyThere (p.group);
        for (const auto& grp : groups)
        {
            PopupMenu sub;
            const auto& list = MangleOps::factoryPresets();
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].group == grp) sub.addItem (list[i].name, [&ops, i] { ops.applyPreset (MangleOps::factoryPresets()[i]); });
            m.addSubMenu (grp.toUpperCase(), sub);
        }
        m.addSeparator();
        m.addItem ("Save as .tbmangle...", [this]
        {
            const auto dir = MangleOps::userPresetFolder();
            dir.createDirectory();
            chooser = std::make_unique<FileChooser> ("Save MANGLE preset", dir.getChildFile ("My Mangle.tbmangle"), "*.tbmangle");
            chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f == File()) return;
                const auto file = f.withFileExtension ("tbmangle");
                if (auto xml = MangleOps::presetToTree (processor.getMangleOps().capturePreset (file.getFileNameWithoutExtension())).createXml())
                    xml->writeTo (file);
            });
        });
        m.addItem ("Load .tbmangle...", [this]
        {
            chooser = std::make_unique<FileChooser> ("Load MANGLE preset", MangleOps::userPresetFolder(), "*.tbmangle");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
            {
                if (auto xml = parseXML (fc.getResult()))
                    processor.getMangleOps().applyPreset (MangleOps::presetFromTree (ValueTree::fromXml (*xml)));
            });
        });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&presetKey));
    }
}
