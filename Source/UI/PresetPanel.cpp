#include "UI/PresetPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;
    using Type = PresetManager::Type;
    using Filter = PresetManager::Filter;

    namespace
    {
        constexpr int kRowH = 21;
        const char* const kTypeChip[] = { "KIT", "PAD", "SEQ" };
    }

    PresetPanel::PresetPanel (TwistedBreaksProcessor& p) : processor (p), pm (p.getPresets())
    {
        const Filter filters[] = { Filter::kits, Filter::pads, Filter::sequences, Filter::all, Filter::favourites };
        for (int i = 0; i < 5; ++i)
        {
            addAndMakeVisible (filterKeys[(size_t) i]);
            const auto f = filters[i];
            filterKeys[(size_t) i].onClick = [this, f] { pm.setFilter (f); pm.setCursor (0); scroll = 0; refresh(); };
        }
        sortKey.onClick = [this] { pm.setSort (pm.getSort() == PresetManager::Sort::name ? PresetManager::Sort::date : PresetManager::Sort::name); refresh(); };
        loadKey.onClick = [this] { loadRow (pm.getCursor()); };
        saveKey.onClick = [this]
        {
            const auto f = pm.getCurrentKitFile();
            if (f.existsAsFile() && ! f.isAChildOf (pm.factoryFolder()) && pm.getKitName() != "INIT")
            {
                PresetManager::Meta m;
                PresetManager::readMeta (f, m);
                m.name = pm.getKitName();
                const auto r = pm.saveKit (f, m);
                message (r.wasOk() ? "SAVED " + m.name.toUpperCase() : r.getErrorMessage());
            }
            else showSaveDialog (Type::kit);
        };
        saveAsKey.onClick = [this] { showSaveDialog (Type::kit); };
        renameKey.onClick = [this] { renameRow (pm.getCursor()); };
        deleteKey.onClick = [this]
        {
            const auto& list = pm.getList();
            const int row = pm.getCursor();
            if (! isPositiveAndBelow (row, list.size()) || list[row].factory) return;
            const auto f = list[row].file;
            const auto name = list[row].name;
            AlertWindow::showOkCancelBox (MessageBoxIconType::QuestionIcon, "Delete preset", "Move \"" + name + "\" to the Trash?", "DELETE", "CANCEL", this,
                                          ModalCallbackFunction::create ([this, f] (int ok)
            {
                if (ok == 0) return;
                const auto r = pm.removePreset (f);
                message (r.wasOk() ? "MOVED TO THE TRASH" : r.getErrorMessage());
            }));
        };
        finderKey.onClick = [this]
        {
            const auto& list = pm.getList();
            if (isPositiveAndBelow (pm.getCursor(), list.size())) list[pm.getCursor()].file.revealToUser();
            else { pm.presetsFolder (Type::kit).createDirectory(); pm.presetsFolder (Type::kit).revealToUser(); }
        };
        importKey.onClick = [this]
        {
            chooser = std::make_unique<FileChooser> ("Import a Twisted Breaks preset", File::getSpecialLocation (File::userHomeDirectory), "*.tbkit;*.tbpad;*.tbseq");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
            {
                if (! fc.getResult().existsAsFile()) return;
                File imported;
                const auto r = pm.importPreset (fc.getResult(), &imported);
                message (r.wasOk() ? "IMPORTED " + imported.getFileNameWithoutExtension().toUpperCase() : r.getErrorMessage());
            });
        };
        collectKey.onClick = [this]
        {
            const int n = pm.collectSamples();
            message (n > 0 ? "COLLECTED " + String (n) + " SAMPLES INTO THE LIBRARY" : String ("NOTHING TO COLLECT: EVERY SAMPLE IS IN THE LIBRARY"));
        };
        listenKey.setClickingTogglesState (true);
        listenKey.setToggleState (true, dontSendNotification);
        listenKey.onClick = [this] { if (! listenKey.getToggleState()) pm.stopPreview(); };

        for (auto* k : { &sortKey, &loadKey, &saveKey, &saveAsKey, &renameKey, &deleteKey, &finderKey, &importKey, &collectKey, &listenKey })
            addAndMakeVisible (k);

        searchBox.setTextToShowWhenEmpty ("SEARCH NAME, TAG, NOTE", c::vfd.withAlpha (0.35f));
        searchBox.setFont (monoFont (13.0f));
        searchBox.setColour (TextEditor::backgroundColourId, c::plateBlack);
        searchBox.setColour (TextEditor::textColourId, c::vfd);
        searchBox.setColour (TextEditor::outlineColourId, c::steelEdge.withAlpha (0.5f));
        searchBox.setColour (TextEditor::focusedOutlineColourId, c::amber.withAlpha (0.7f));
        searchBox.onTextChange = [this] { pm.setSearch (searchBox.getText()); pm.setCursor (0); scroll = 0; refresh(); };
        searchBox.onEscapeKey = [this] { searchBox.clear(); pm.setSearch ({}); refresh(); };
        addAndMakeVisible (searchBox);
        setWantsKeyboardFocus (true);
    }

    void PresetPanel::setSelectedPad (int pad)
    {
        selectedPad = jlimit (0, kNumPads - 1, pad);
        pm.setTargetPad (selectedPad);
    }

    void PresetPanel::message (const String& s)
    {
        note = s.toUpperCase();
        noteUntil = Time::getMillisecondCounterHiRes() + 6000.0;
        repaint();
    }

    Rectangle<int> PresetPanel::listArea() const
    {
        return { 14, 74, getWidth() - 28, getHeight() - 74 - 76 };
    }

    void PresetPanel::resized()
    {
        int x = 128;
        for (auto& k : filterKeys) { k.setBounds (x, 10, 50, 24); x += 52; }
        sortKey.setBounds (getWidth() - 14 - 54, 10, 54, 24);
        searchBox.setBounds (14, 42, 250, 24);

        const int y1 = getHeight() - 68, y2 = getHeight() - 36;
        const int w = (getWidth() - 28 - 5 * 4) / 6;
        int bx = 14;
        for (auto* k : { &loadKey, &saveKey, &saveAsKey, &renameKey, &deleteKey, &finderKey }) { k->setBounds (bx, y1, w, 26); bx += w + 4; }
        importKey.setBounds (14, y2, w, 26);
        collectKey.setBounds (14 + w + 4, y2, w * 2 + 4, 26);
        listenKey.setBounds (14 + (w + 4) * 3, y2, w, 26);
        background = {};
    }

    int PresetPanel::rowAt (Point<float> pt) const
    {
        const auto a = listArea().withTrimmedTop (kRowH);
        if (! a.toFloat().contains (pt)) return -1;
        return scroll + (int) ((pt.y - (float) a.getY()) / (float) kRowH);
    }

    void PresetPanel::ensureVisible()
    {
        const int visible = jmax (1, (listArea().getHeight() - kRowH) / kRowH);
        const int cur = pm.getCursor();
        if (cur < scroll) scroll = cur;
        if (cur >= scroll + visible) scroll = cur - visible + 1;
        scroll = jlimit (0, jmax (0, pm.getList().size() - visible), scroll);
    }

    void PresetPanel::loadRow (int row)
    {
        const auto& list = pm.getList();
        if (! isPositiveAndBelow (row, list.size())) return;
        pm.stopPreview();
        const auto m = list[row];
        const auto r = pm.load (m.file, selectedPad);
        if (r.failed()) message (r.getErrorMessage());
        else if (m.type == Type::pad) message ("LOADED " + m.name + " ONTO PAD " + padLabel (selectedPad));
    }

    void PresetPanel::renameRow (int row)
    {
        const auto& list = pm.getList();
        if (! isPositiveAndBelow (row, list.size()) || list[row].factory) return;
        const auto f = list[row].file;
        auto* w = new AlertWindow ("Rename preset", {}, MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", list[row].name, "Name");
        w->addButton ("RENAME", 1, KeyPress (KeyPress::returnKey));
        w->addButton ("CANCEL", 0, KeyPress (KeyPress::escapeKey));
        w->enterModalState (true, ModalCallbackFunction::create ([this, w, f] (int r)
        {
            if (r == 0) return;
            const auto name = w->getTextEditorContents ("name").trim();
            if (name.isEmpty()) return;
            const bool wasCurrent = f == pm.getCurrentKitFile();
            const auto res = pm.renamePreset (f, name);
            if (res.wasOk() && wasCurrent) pm.setKitName (name);
            message (res.wasOk() ? "RENAMED TO " + name : res.getErrorMessage());
        }), true);
    }

    void PresetPanel::showSaveDialog (Type initial)
    {
        auto* w = new AlertWindow ("Save preset", "Samples are saved inside the preset, so it opens on any Mac.", MessageBoxIconType::NoIcon, this);
        w->addComboBox ("type", { "Kit: everything", "Pad " + padLabel (selectedPad) + ": its sound, settings and steps", "Sequences: the patterns only" }, "Save");
        if (auto* cb = w->getComboBoxComponent ("type")) cb->setSelectedItemIndex ((int) initial);
        const auto suggested = initial == Type::kit && pm.getKitName() != "INIT" ? pm.getKitName() : String();
        w->addTextEditor ("name", suggested, "Name");
        w->addTextEditor ("tags", {}, "Tags (comma separated: Jungle, Amen ...)");
        w->addTextEditor ("note", {}, "Note");
        w->addButton ("SAVE", 1, KeyPress (KeyPress::returnKey));
        w->addButton ("CANCEL", 0, KeyPress (KeyPress::escapeKey));
        w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
        {
            if (r == 0) return;
            PresetManager::Meta m;
            m.name = w->getTextEditorContents ("name").trim();
            if (m.name.isEmpty()) m.name = "Untitled";
            m.tags = StringArray::fromTokens (w->getTextEditorContents ("tags"), ",", "");
            m.tags.trim(); m.tags.removeEmptyStrings();
            m.note = w->getTextEditorContents ("note").trim();
            const auto type = (Type) jlimit (0, 2, w->getComboBoxComponent ("type")->getSelectedItemIndex());
            const auto file = pm.presetsFolder (type).getChildFile (File::createLegalFileName (m.name) + PresetManager::extensionFor (type));
            auto doSave = [this, type, file, m]
            {
                const auto res = type == Type::kit ? pm.saveKit (file, m) : type == Type::pad ? pm.savePad (selectedPad, file, m) : pm.saveSequence (file, m);
                message (res.wasOk() ? "SAVED " + m.name : res.getErrorMessage());
            };
            if (file.existsAsFile())
                AlertWindow::showOkCancelBox (MessageBoxIconType::QuestionIcon, "Replace preset", "\"" + m.name + "\" already exists. Replace it?",
                                              "REPLACE", "CANCEL", this, ModalCallbackFunction::create ([doSave] (int ok) { if (ok != 0) doSave(); }));
            else doSave();
        }), true);
    }

    void PresetPanel::showRowMenu (int row)
    {
        const auto& list = pm.getList();
        if (! isPositiveAndBelow (row, list.size())) return;
        const auto m = list[row];
        PopupMenu menu;
        menu.addSectionHeader (m.name);
        menu.addItem (m.type == Type::pad ? "Load onto pad " + padLabel (selectedPad) : String ("Load"), [this, row] { loadRow (row); });
        if (m.type == Type::pad)
        {
            PopupMenu onto;
            for (int p = 0; p < kNumPads; ++p)
                onto.addItem ("Pad " + padLabel (p), [this, f = m.file, p] { const auto r = pm.load (f, p); if (r.failed()) message (r.getErrorMessage()); });
            menu.addSubMenu ("Load onto", onto);
        }
        menu.addItem ("Listen", [this, f = m.file] { pm.preview (f); });
        menu.addItem (m.favourite ? "Remove from favourites" : "Add to favourites", [this, f = m.file, fav = m.favourite] { pm.setFavourite (f, ! fav); });
        menu.addSeparator();
        menu.addItem ("Rename...", ! m.factory, false, [this, row] { renameRow (row); });
        menu.addItem ("Delete (to the Trash)", ! m.factory, false, [this, row] { pm.setCursor (row); deleteKey.triggerClick(); });
        menu.addItem ("Show in Finder", [f = m.file] { f.revealToUser(); });
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void PresetPanel::mouseDown (const MouseEvent& e)
    {
        grabKeyboardFocus();
        const int row = rowAt (e.position);
        const auto& list = pm.getList();
        if (! isPositiveAndBelow (row, list.size())) return;
        pm.setCursor (row);
        if (e.mods.isPopupMenu()) { showRowMenu (row); return; }
        const auto a = listArea();
        if (e.position.x < (float) a.getX() + 18.0f) { pm.setFavourite (list[row].file, ! list[row].favourite); return; }
        if (listenKey.getToggleState()) pm.preview (list[row].file);
        repaint();
    }

    void PresetPanel::mouseDoubleClick (const MouseEvent& e)
    {
        const int row = rowAt (e.position);
        if (row >= 0 && e.position.x >= (float) listArea().getX() + 18.0f) loadRow (row);
    }

    void PresetPanel::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
    {
        const int visible = jmax (1, (listArea().getHeight() - kRowH) / kRowH);
        scroll = jlimit (0, jmax (0, pm.getList().size() - visible), scroll - (int) std::round (w.deltaY * 8.0f));
        repaint();
    }

    bool PresetPanel::keyPressed (const KeyPress& k)
    {
        if (k == KeyPress::upKey)     { pm.moveCursor (-1); ensureVisible(); repaint(); return true; }
        if (k == KeyPress::downKey)   { pm.moveCursor (1);  ensureVisible(); repaint(); return true; }
        if (k == KeyPress::returnKey) { loadRow (pm.getCursor()); return true; }
        if (k == KeyPress::spaceKey)
        {
            const auto& list = pm.getList();
            if (isPositiveAndBelow (pm.getCursor(), list.size())) pm.preview (list[pm.getCursor()].file);
            return true;
        }
        return false;
    }

    bool PresetPanel::isInterestedInFileDrag (const StringArray& files)
    {
        for (const auto& f : files) { Type t; if (PresetManager::typeFromFile (File (f), t)) return true; }
        return false;
    }

    void PresetPanel::filesDropped (const StringArray& files, int, int)
    {
        for (const auto& path : files)
        {
            Type t;
            if (! PresetManager::typeFromFile (File (path), t)) continue;
            File imported;
            auto r = pm.importPreset (File (path), &imported);
            if (r.wasOk()) r = pm.load (imported, selectedPad);
            message (r.wasOk() ? "IMPORTED AND LOADED " + imported.getFileNameWithoutExtension() : r.getErrorMessage());
            return;
        }
    }

    void PresetPanel::refresh()
    {
        const auto f = pm.getFilter();
        const Filter filters[] = { Filter::kits, Filter::pads, Filter::sequences, Filter::all, Filter::favourites };
        for (int i = 0; i < 5; ++i) filterKeys[(size_t) i].setToggleState (f == filters[i], dontSendNotification);
        sortKey.setButtonText (pm.getSort() == PresetManager::Sort::name ? "A-Z" : "NEW");
        if (searchBox.getText() != pm.getSearch() && ! searchBox.hasKeyboardFocus (true)) searchBox.setText (pm.getSearch(), false);

        const auto& list = pm.getList();
        const bool has = isPositiveAndBelow (pm.getCursor(), list.size());
        const bool user = has && ! list[pm.getCursor()].factory;
        loadKey.setEnabled (has);
        renameKey.setEnabled (user);
        deleteKey.setEnabled (user);
        listenKey.tick();

        const auto v = pm.getVersion();
        const bool mod = pm.isModified(), ld = pm.isLoading();
        if (v != shownVersion || pm.getKitName() != shownKit || mod != shownModified || ld != shownLoading
             || (noteUntil > 0.0 && Time::getMillisecondCounterHiRes() > noteUntil))
        {
            if (noteUntil > 0.0 && Time::getMillisecondCounterHiRes() > noteUntil) { note = {}; noteUntil = 0.0; }
            shownVersion = v; shownKit = pm.getKitName(); shownModified = mod; shownLoading = ld;
            ensureVisible();
            repaint();
        }
    }

    void PresetPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random r (4242);
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
            paint::labelPlate (bg, { 18.0f, 10.0f, 100.0f, 22.0f }, "PRESETS", {}, paint::PlateStyle::bakelite, r);
            paint::recess (bg, listArea().toFloat().expanded (2.0f), 3.0f);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        // Current kit, top right of the search row.
        {
            const auto kitArea = Rectangle<float> (276.0f, 42.0f, (float) getWidth() - 290.0f, 24.0f);
            g.setColour (c::plateBlack);
            g.fillRoundedRectangle (kitArea, 3.0f);
            g.setFont (monoFont (12.0f));
            g.setColour (c::bakelite.withAlpha (0.6f));
            g.drawText ("KIT", kitArea.reduced (8.0f, 0.0f), Justification::centredLeft, false);
            g.setColour (c::vfd);
            const auto name = (pm.isLoading() ? "LOADING " : "") + pm.getKitName().toUpperCase() + (pm.isModified() ? " *" : "");
            g.drawText (name, kitArea.withTrimmedLeft (40.0f).reduced (4.0f, 0.0f), Justification::centredLeft, true);
        }

        // The list.
        const auto a = listArea();
        g.setColour (c::crtGlass.darker (0.3f));
        g.fillRect (a);
        const int w = a.getWidth();
        const int xStar = a.getX() + 4, xType = a.getX() + 22, xName = a.getX() + 60, xBpm = a.getRight() - 196, xPads = a.getRight() - 152, xTags = a.getRight() - 112;
        g.setFont (monoFont (10.0f));
        g.setColour (c::bakelite.withAlpha (0.55f));
        const int hy = a.getY();
        g.drawText ("TYPE", xType, hy, 36, kRowH, Justification::centredLeft, false);
        g.drawText ("NAME", xName, hy, xBpm - xName, kRowH, Justification::centredLeft, false);
        g.drawText ("BPM", xBpm, hy, 40, kRowH, Justification::centredLeft, false);
        g.drawText ("PADS", xPads, hy, 36, kRowH, Justification::centredLeft, false);
        g.drawText ("TAGS", xTags, hy, 108, kRowH, Justification::centredLeft, false);
        g.setColour (c::steelEdge.withAlpha (0.4f));
        g.drawHorizontalLine (hy + kRowH - 1, (float) a.getX(), (float) a.getRight());

        const auto& list = pm.getList();
        const int visible = jmax (1, (a.getHeight() - kRowH) / kRowH);
        if (list.isEmpty())
        {
            g.setColour (c::vfd.withAlpha (0.5f));
            g.setFont (monoFont (12.0f));
            g.drawFittedText (pm.getSearch().isNotEmpty() ? "NOTHING MATCHES" : "NO PRESETS HERE YET\nSAVE AS... TO MAKE ONE",
                              a.withTrimmedTop (kRowH), Justification::centred, 2);
        }
        for (int i = 0; i < visible; ++i)
        {
            const int row = scroll + i;
            if (row >= list.size()) break;
            const auto& m = list[row];
            const int y = a.getY() + kRowH * (i + 1);
            const bool cur = row == pm.getCursor();
            const bool current = m.file == pm.getCurrentKitFile();
            if (cur)
            {
                g.setColour (c::amber.withAlpha (0.16f));
                g.fillRect (a.getX(), y, w, kRowH);
            }
            g.setFont (monoFont (13.0f));
            g.setColour (m.favourite ? c::amber : c::steelEdge.withAlpha (0.6f));
            g.drawText (m.favourite ? String::fromUTF8 ("\xe2\x98\x85") : String::fromUTF8 ("\xe2\x98\x86"), xStar, y, 16, kRowH, Justification::centred, false);
            g.setFont (monoFont (10.0f));
            g.setColour (m.type == Type::kit ? c::amber : m.type == Type::pad ? c::phosphor : Colour (0xff6cc7ff));
            g.drawText (kTypeChip[(int) m.type], xType, y, 36, kRowH, Justification::centredLeft, false);
            g.setFont (monoFont (12.0f));
            g.setColour (cur ? c::vfd : c::plateText.withAlpha (m.factory ? 0.7f : 0.95f));
            g.drawText ((current ? "> " : "") + m.name + (m.factory ? "  (FACTORY)" : ""), xName, y, xBpm - xName - 6, kRowH, Justification::centredLeft, true);
            g.setFont (monoFont (11.0f));
            g.setColour (c::plateText.withAlpha (0.7f));
            g.drawText (m.bpm > 0.0 ? String (roundToInt (m.bpm)) : String ("-"), xBpm, y, 40, kRowH, Justification::centredLeft, false);
            g.drawText (m.type == Type::sequence ? String ("-") : String (m.pads), xPads, y, 36, kRowH, Justification::centredLeft, false);
            g.drawText (m.tags.joinIntoString (", "), xTags, y, a.getRight() - xTags - 4, kRowH, Justification::centredLeft, true);
        }
        if (list.size() > visible)
        {
            const float frac = (float) visible / (float) list.size();
            const float th = jmax (16.0f, frac * (float) (a.getHeight() - kRowH));
            const float ty = (float) (a.getY() + kRowH) + ((float) scroll / (float) jmax (1, list.size() - visible)) * ((float) (a.getHeight() - kRowH) - th);
            g.setColour (c::steelEdge.withAlpha (0.7f));
            g.fillRoundedRectangle ((float) a.getRight() - 5.0f, ty, 3.0f, th, 1.5f);
        }

        // Note under the list: the highlighted preset's note, or a message.
        String line = note.isNotEmpty() ? note : pm.getStatus();
        if (line.isEmpty() && isPositiveAndBelow (pm.getCursor(), list.size())) line = list[pm.getCursor()].note;
        g.setFont (monoFont (11.0f));
        g.setColour (c::vfd.withAlpha (0.85f));
        g.drawText (line, Rectangle<int> (14 + ((getWidth() - 48) / 6 + 4) * 4, getHeight() - 36, getWidth() - 28 - ((getWidth() - 48) / 6 + 4) * 4, 26),
                    Justification::centredLeft, true);
    }

    //==========================================================================
    KitBar::KitBar (TwistedBreaksProcessor& p) : processor (p)
    {
        addAndMakeVisible (prevKey);
        addAndMakeVisible (nextKey);
        prevKey.onClick = [this] { processor.getPresets().stepKit (-1); };
        nextKey.onClick = [this] { processor.getPresets().stepKit (1); };
        setTooltip ("The current kit: click for PRESETS");
    }

    void KitBar::resized()
    {
        prevKey.setBounds (0, 0, 28, getHeight());
        nextKey.setBounds (getWidth() - 28, 0, 28, getHeight());
    }

    void KitBar::refresh()
    {
        auto& pm = processor.getPresets();
        const auto n = pm.getKitName();
        const bool mod = pm.isModified(), ld = pm.isLoading();
        if (n != shown || mod != shownMod || ld != shownLoading) { shown = n; shownMod = mod; shownLoading = ld; repaint(); }
    }

    void KitBar::mouseDown (const MouseEvent&) { if (onOpen) onOpen(); }

    void KitBar::paint (Graphics& g)
    {
        const auto r = getLocalBounds().toFloat().reduced (31.0f, 0.0f);
        g.setColour (c::plateBlack);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (c::steelEdge.withAlpha (0.5f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        g.setFont (monoFont (9.0f));
        g.setColour (c::bakelite.withAlpha (0.55f));
        g.drawText ("KIT", r.reduced (6.0f, 3.0f).removeFromTop (10.0f), Justification::centredLeft, false);
        g.setFont (monoFont (13.0f));
        g.setColour (c::vfd);
        g.drawText ((shownLoading ? "LOADING " : "") + shown.toUpperCase() + (shownMod ? " *" : ""), r.reduced (6.0f, 2.0f).withTrimmedTop (10.0f),
                    Justification::centredLeft, true);
    }
}
