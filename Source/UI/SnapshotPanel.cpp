#include "UI/SnapshotPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr float kSnapTop = 40.0f, kBankTop = 132.0f, kTileH = 48.0f;
        String bankName (const BreakBank::Slot& s)
        {
            switch (s.status)
            {
                case BreakBank::Status::empty:   return "EMPTY";
                case BreakBank::Status::loading: return "LOADING";
                case BreakBank::Status::missing: return "MISSING";
                case BreakBank::Status::ready:   return s.name.toUpperCase();
            }
            return {};
        }
    }

    SnapshotPanel::SnapshotPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        for (int i = 0; i < 3; ++i)
        {
            addAndMakeVisible (snapAt[(size_t) i]);
            addAndMakeVisible (bankAt[(size_t) i]);
            snapAt[(size_t) i].onClick = [this, i] { setChoice (params::snapAt, i); };
            bankAt[(size_t) i].onClick = [this, i] { setChoice (params::bankAt, i); };
        }
        storeKey.setClickingTogglesState (true);
        fromPadKey.setClickingTogglesState (true);
        followKey.onClick = [this]
        {
            auto& apvts = processor.getParameters();
            for (int pad = 0; pad < kNumPads; ++pad)
            {
                const bool slice = roundToInt (apvts.getRawParameterValue (params::padId (pad, params::padMode))->load()) == params::modeSlice
                                || apvts.getRawParameterValue (params::padId (pad, params::padSliceSeq))->load() > 0.5f;
                if (auto* prm = apvts.getParameter (params::padId (pad, params::padFollowBank)))
                    if (slice && prm->getValue() < 0.5f)
                    {
                        prm->beginChangeGesture(); prm->setValueNotifyingHost (1.0f); prm->endChangeGesture();
                    }
            }
        };
        findKey.onClick = [this]
        {
            chooser = std::make_unique<FileChooser> ("Find the missing samples in...", File::getSpecialLocation (File::userMusicDirectory));
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories, [this] (const FileChooser& fc)
            {
                if (fc.getResult().isDirectory()) processor.getSamples().relocateMissing (fc.getResult());
            });
        };
        songKey.onClick = [this]
        {
            if (auto* r = processor.getParameters().getParameter (params::songOn))
            { r->beginChangeGesture(); r->setValueNotifyingHost (r->getValue() > 0.5f ? 0.0f : 1.0f); r->endChangeGesture(); }
        };
        songClear.onClick = [this] { processor.getSong().clear(); };
        for (auto* k : { &storeKey, &fromPadKey, &followKey, &findKey, &songKey, &songClear })
            addAndMakeVisible (k);
        setSelectedPad (0);
    }

    void SnapshotPanel::setSelectedPad (int pad)
    {
        selectedPad = jlimit (0, kNumPads - 1, pad);
        fromPadKey.setButtonText ("FROM PAD " + padLabel (selectedPad));
    }

    void SnapshotPanel::setChoice (const char* id, int index)
    {
        if (auto* prm = processor.getParameters().getParameter (id))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) index));
            prm->endChangeGesture();
        }
        refresh();
    }

    Rectangle<float> SnapshotPanel::snapBounds (int i) const { return { 14.0f + (float) i * 69.0f, kSnapTop, 66.0f, kTileH }; }
    Rectangle<float> SnapshotPanel::bankBounds (int i) const { return { 14.0f + (float) i * 69.0f, kBankTop, 66.0f, kTileH - 6.0f }; }
    Rectangle<float> SnapshotPanel::songBounds (int i) const
    {
        constexpr float top = 222.0f;
        return { 14.0f + (float) (i % 8) * 69.0f, top + (float) (i / 8) * 40.0f, 66.0f, 36.0f };
    }

    void SnapshotPanel::songMenu (int entry)
    {
        auto& song = processor.getSong();
        const auto& es = song.getEntries();
        if (! isPositiveAndBelow (entry, (int) es.size())) return;
        const auto e = es[(size_t) entry];
        PopupMenu m;
        m.addSectionHeader ("STEP " + String (entry + 1) + " OF THE SONG");
        for (int s = 0; s < SnapshotManager::kSlots; ++s)
            m.addItem ("Snapshot " + String (s + 1) + (processor.getSnapshots().getState (s) == SnapshotManager::SlotState::empty ? " (empty)" : ""),
                       true, e.slot == s, [this, entry, e, s] { processor.getSong().set (entry, { s, e.bars }); });
        m.addSeparator();
        for (int b : { 1, 2, 4, 8, 16 })
            m.addItem ("For " + String (b) + (b == 1 ? " bar" : " bars"), true, e.bars == b, [this, entry, e, b] { processor.getSong().set (entry, { e.slot, b }); });
        m.addSeparator();
        m.addItem ("Move earlier", entry > 0, false, [this, entry] { processor.getSong().move (entry, -1); });
        m.addItem ("Move later", entry < (int) es.size() - 1, false, [this, entry] { processor.getSong().move (entry, 1); });
        m.addItem ("Duplicate", (int) es.size() < SongMode::kMaxEntries, false, [this, e] { processor.getSong().add (e); });
        m.addItem ("Remove", [this, entry] { processor.getSong().remove (entry); });
        m.showMenuAsync ({});
    }

    void SnapshotPanel::resized()
    {
        for (int i = 0; i < 3; ++i)
        {
            snapAt[(size_t) i].setBounds (226 + i * 52, 10, 50, 24);
            bankAt[(size_t) i].setBounds (226 + i * 52, 100, 50, 24);
        }
        storeKey.setBounds (getWidth() - 96, 10, 82, 24);
        fromPadKey.setBounds (386, 100, 100, 24);
        followKey.setBounds (getWidth() - 14 - 176, getHeight() - 36, 176, 26);
        songKey.setBounds (128, 190, 80, 24);
        songClear.setBounds (getWidth() - 14 - 96, 190, 96, 24);
        findKey.setBounds (14, getHeight() - 36, 150, 26);
        // Room check: the FROM PAD key sits left of the right edge.
        fromPadKey.setBounds (getWidth() - 14 - 100, 100, 100, 24);
        background = {};
    }

    void SnapshotPanel::mouseDown (const MouseEvent& e)
    {
        // SONG chain.
        {
            auto& song = processor.getSong();
            const int n = (int) song.getEntries().size();
            for (int i = 0; i <= n && i < 24; ++i)
                if (songBounds (i).contains (e.position))
                {
                    if (i == n)
                    {
                        const int act = processor.getSnapshots().getActive();
                        song.add ({ act >= 0 ? act : 0, 4 });
                    }
                    else songMenu (i);
                    repaint();
                    return;
                }
        }
        auto& snaps = processor.getSnapshots();
        auto& bank  = processor.getBank();
        for (int i = 0; i < SnapshotManager::kSlots; ++i)
        {
            if (snapBounds (i).contains (e.position))
            {
                if (e.mods.isPopupMenu())
                {
                    PopupMenu m;
                    m.addItem ("Store into " + String (i + 1), [this, i] { processor.getSnapshots().store (i); });
                    m.addItem ("Clear " + String (i + 1), snaps.getState (i) != SnapshotManager::SlotState::empty,
                               false, [this, i] { processor.getSnapshots().clear (i); });
                    m.showMenuAsync ({});
                }
                else if (storeKey.getToggleState())
                {
                    snaps.store (i);
                    storeKey.setToggleState (false, dontSendNotification);
                }
                else
                {
                    snaps.recall (i);
                }
                repaint();
                return;
            }
            if (bankBounds (i).contains (e.position))
            {
                if (e.mods.isPopupMenu())
                {
                    PopupMenu m;
                    m.addItem ("Load a break...", [this, i] { chooseBankFile (i); });
                    m.addItem ("Copy from pad " + String (selectedPad + 1), [this, i] { processor.getBank().copyFromPad (i, selectedPad); });
                    m.addItem ("Clear", bank.get (i).status != BreakBank::Status::empty, false, [this, i] { processor.getBank().clear (i); });
                    m.showMenuAsync ({});
                }
                else if (fromPadKey.getToggleState())
                {
                    bank.copyFromPad (i, selectedPad);
                    fromPadKey.setToggleState (false, dontSendNotification);
                }
                else if (auto* prm = processor.getParameters().getParameter (params::bankActive))
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (i + 1)));
                    prm->endChangeGesture();
                }
                repaint();
                return;
            }
        }
    }

    void SnapshotPanel::chooseBankFile (int slot)
    {
        chooser = std::make_unique<FileChooser> ("Load a break into bank slot " + String (slot + 1),
                                                 File::getSpecialLocation (File::userMusicDirectory), "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this, slot] (const FileChooser& fc)
        {
            if (fc.getResult().existsAsFile()) processor.getBank().loadFile (slot, fc.getResult());
        });
    }

    bool SnapshotPanel::isInterestedInFileDrag (const StringArray& files)
    {
        for (const auto& f : files) if (SampleManager::isSupportedFile (File (f))) return true;
        return false;
    }

    void SnapshotPanel::filesDropped (const StringArray& files, int x, int y)
    {
        for (int i = 0; i < BreakBank::kSlots; ++i)
            if (bankBounds (i).contains ((float) x, (float) y))
                for (const auto& f : files)
                    if (SampleManager::isSupportedFile (File (f))) { processor.getBank().loadFile (i, File (f)); return; }
    }

    void SnapshotPanel::refresh()
    {
        auto& apvts = processor.getParameters();
        const int sa = roundToInt (apvts.getRawParameterValue (params::snapAt)->load());
        const int ba = roundToInt (apvts.getRawParameterValue (params::bankAt)->load());
        for (int i = 0; i < 3; ++i)
        {
            snapAt[(size_t) i].setToggleState (i == sa, dontSendNotification);
            bankAt[(size_t) i].setToggleState (i == ba, dontSendNotification);
        }
        storeKey.setBlinking (storeKey.getToggleState()); storeKey.tick();
        fromPadKey.setBlinking (fromPadKey.getToggleState()); fromPadKey.tick();

        songKey.setToggleState (processor.getParameters().getRawParameterValue (params::songOn)->load() > 0.5f, dontSendNotification);
        songClear.setEnabled (! processor.getSong().getEntries().empty());
        if (processor.getSong().getVersion() != shownSong) { shownSong = processor.getSong().getVersion(); repaint(); }
        const int missing = processor.getSamples().countMissing();
        findKey.setEnabled (missing > 0);
        findKey.setButtonText (missing > 0 ? "FIND " + String (missing) + " MISSING" : String ("NONE MISSING"));

        const bool b = (Time::getMillisecondCounter() / 200) % 2 == 0;
        const auto sv = processor.getSnapshots().getVersion(), bv = processor.getBank().getVersion();
        const int activeBank = processor.getEngine().getActiveBank();
        if (sv != shownSnap || bv != shownBank || activeBank != shownActiveBank || missing != shownMissing || b != blink)
        {
            shownSnap = sv; shownBank = bv; shownActiveBank = activeBank; shownMissing = missing; blink = b;
            repaint();
        }
    }

    void SnapshotPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random r (9909);
            const auto all = getLocalBounds().toFloat();
            paint::plate (bg, all, c::steel, 4.0f);
            {
                Graphics::ScopedSaveState ss (bg);
                bg.reduceClipRegion (getLocalBounds());
                paint::ageArea (bg, all, r, 1.3f);
                paint::edgeWear (bg, all, r, 1.5f);
            }
            const auto rr = all.reduced (9.0f);
            for (auto pt : { rr.getTopLeft(), rr.getTopRight(), rr.getBottomLeft(), rr.getBottomRight() })
                paint::rivet (bg, pt, 4.2f, r);
            paint::labelPlate (bg, { 18.0f, 10.0f, 124.0f, 22.0f }, "SNAPSHOTS", {}, paint::PlateStyle::bakelite, r);
            paint::labelPlate (bg, { 18.0f, 100.0f, 124.0f, 22.0f }, "BREAK BANK", {}, paint::PlateStyle::bakelite, r);
            paint::labelPlate (bg, { 18.0f, 190.0f, 100.0f, 22.0f }, "SONG", {}, paint::PlateStyle::bakelite, r);
            bg.setColour (c::bakelite.withAlpha (0.8f));
            bg.setFont (industrialFont (11.0f));
            bg.drawText ("SWITCH", Rectangle<float> (166.0f, 10.0f, 56.0f, 24.0f), Justification::centredRight, false);
            bg.drawText ("SWITCH", Rectangle<float> (166.0f, 100.0f, 56.0f, 24.0f), Justification::centredRight, false);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        auto& snaps = processor.getSnapshots();
        for (int i = 0; i < SnapshotManager::kSlots; ++i)
        {
            const auto r = snapBounds (i);
            const auto st = snaps.getState (i);
            using S = SnapshotManager::SlotState;
            paint::plate (g, r, st == S::active ? Colour (0xff3a3226) : Colour (0xff1d1e1b), 3.0f);
            const float lamp = st == S::active ? 1.0f : ((st == S::loading || st == S::waiting) ? (blink ? 1.0f : 0.2f)
                                                         : (st == S::stored ? 0.25f : 0.0f));
            paint::lamp (g, { r.getX() + 10.0f, r.getY() + 10.0f }, 4.0f, c::amber, c::amberDim, lamp);
            g.setColour (c::plateText.withAlpha (st == S::empty ? 0.4f : 1.0f));
            g.setFont (industrialFont (20.0f));
            g.drawText (String (i + 1), r.withTrimmedTop (2.0f).withTrimmedBottom (14.0f), Justification::centred, false);
            g.setFont (monoFont (9.0f));
            g.setColour (c::vfd.withAlpha (st == S::empty ? 0.4f : 0.9f));
            const char* label = st == S::empty ? "EMPTY" : st == S::loading ? "LOADING" : st == S::waiting ? "WAITING"
                              : st == S::active ? "ACTIVE" : "STORED";
            g.drawText (label, r.withTrimmedTop (r.getHeight() - 14.0f), Justification::centred, false);
        }

        auto& bank = processor.getBank();
        const int activeBank = processor.getEngine().getActiveBank();
        for (int i = 0; i < BreakBank::kSlots; ++i)
        {
            const auto r = bankBounds (i);
            const auto s = bank.get (i);
            const bool on = i == activeBank;
            paint::plate (g, r, on ? Colour (0xff3a3226) : Colour (0xff1d1e1b), 3.0f);
            paint::lamp (g, { r.getX() + 9.0f, r.getY() + 9.0f }, 3.5f, c::amber, c::amberDim, on ? 1.0f : 0.15f);
            g.setColour (c::plateText);
            g.setFont (industrialFont (12.0f));
            g.drawText (String (i + 1), r.withTrimmedLeft (16.0f).withHeight (18.0f), Justification::centredLeft, false);
            g.setColour (s.status == BreakBank::Status::missing ? c::red : c::vfd.withAlpha (s.status == BreakBank::Status::empty ? 0.4f : 0.9f));
            g.setFont (monoFont (9.0f));
            g.drawFittedText (bankName (s) + (s.slices.isEmpty() ? String() : "\n" + String (s.slices.count) + " SL"),
                              r.reduced (3.0f, 2.0f).withTrimmedTop (14.0f).toNearestInt(), Justification::centred, 2, 0.7f);
        }

        // SONG chain.
        {
            auto& song = processor.getSong();
            const auto& es = song.getEntries();
            const bool on = processor.getParameters().getRawParameterValue (params::songOn)->load() > 0.5f;
            const bool playing = processor.getBridge().transport.isPlaying.load();
            for (int i = 0; i < (int) es.size() && i < 24; ++i)
            {
                const auto r = songBounds (i);
                const bool cur = on && playing && song.getCurrent() == i;
                const bool empty = snaps.getState (es[(size_t) i].slot) == SnapshotManager::SlotState::empty;
                paint::plate (g, r, cur ? Colour (0xff2c3a26) : Colour (0xff1d1e1b), 3.0f);
                paint::lamp (g, { r.getX() + 8.0f, r.getY() + 8.0f }, 3.2f, c::green, c::greenDim, cur ? 1.0f : 0.12f);
                g.setColour (empty ? c::red.withAlpha (0.8f) : c::plateText);
                g.setFont (industrialFont (15.0f));
                g.drawText (String (es[(size_t) i].slot + 1), r.withTrimmedLeft (14.0f).withWidth (26.0f), Justification::centred, false);
                g.setFont (monoFont (10.0f));
                g.setColour (c::vfd.withAlpha (0.9f));
                g.drawText (String::fromUTF8 ("\xc3\x97") + String (es[(size_t) i].bars), r.withTrimmedLeft (36.0f), Justification::centredLeft, false);
            }
            const int n = (int) es.size();
            if (n < 24)
            {
                const auto r = songBounds (n);
                g.setColour (c::steelEdge.withAlpha (0.6f));
                g.drawRoundedRectangle (r.reduced (1.0f), 3.0f, 1.0f);
                g.setFont (industrialFont (18.0f));
                g.drawText ("+", r, Justification::centred, false);
            }
            g.setFont (monoFont (10.0f));
            g.setColour (c::bakelite.withAlpha (0.7f));
            const int total = song.totalBars();
            const String info = n == 0 ? String ("CLICK + TO ADD A SNAPSHOT FOR SOME BARS")
                              : String (n) + " STEPS, " + String (total) + " BARS" + (on && playing ? "   BAR " + String (song.getBarInSong() + 1) + " / " + String (total) : String());
            g.drawText (info, Rectangle<float> (216.0f, 190.0f, (float) getWidth() - 340.0f, 24.0f), Justification::centredLeft, false);
        }

        g.setColour (c::bakelite.withAlpha (0.65f));
        g.setFont (monoFont (10.0f));
        g.drawText ("MIDI PROGRAM 1-8 = SNAPSHOT 1-8", Rectangle<float> (176.0f, (float) getHeight() - 36.0f, 200.0f, 26.0f),
                    Justification::centredLeft, false);
    }
}
