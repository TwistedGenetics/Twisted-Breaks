#include "UI/PadEditorPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"
#include "UI/StretchMenu.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        struct KnobDef { const char* caption; const char* param; double resetTo; const char* tip; };
        const KnobDef kKnobs[] = {
            { "GAIN",  params::padGain,  0.0, "Pad level (double-click: 0 dB)" },
            { "PAN",   params::padPan,   0.0, "Pad pan (double-click: centre)" },
            { "PITCH", params::padPitch, 0.0, "Pitch in semitones, +/-12 (double-click: 0). A loop on STRETCH PITCH follows the tempo instead." },
            { "START", params::padStart, 0.0, "Where playback starts (also the S marker on the screen)" },
            { "END",   params::padEnd,   1.0, "Where playback ends (also the E marker on the screen)" },
        };

        constexpr int kHeaderH = 28;
        constexpr int kKnobW   = 76;
        constexpr int kCtlTop  = 52;     // top of controls (captions sit above)

        // PLAY page columns: x, width, caption
        struct Col { int x, w; const char* caption; };
        const Col kPlayCols[] = {
            {   0, 84, "MODE"  },
            {  86, 100, "STRETCH" },   // A13: in SYNC's old place (the tempo chip sits at its right)
            { 188, 72, "RATE"  },
            { 262, 72, "CHOKE" },
            { 336, 58, "VEL"   },
            { 396, 36, ""      },     // FIXED velocity key
            { 434, 36, ""      },     // mute / solo keys
            { 472, 68, "NOTE"  },
        };

        File& lastFolder()
        {
            static File f = File::getSpecialLocation (File::userMusicDirectory);
            return f;
        }

        void styleKnob (Slider& k, int boxWidth)
        {
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::TextBoxBelow, true, boxWidth, 16);
            k.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        }
    }

    PadEditorPanel::PadEditorPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        for (int i = 0; i < kNumKnobs; ++i)
        {
            styleKnob (knobs[(size_t) i], kKnobW - 8);
            knobs[(size_t) i].setTooltip (kKnobs[i].tip);
            addChildComponent (knobs[(size_t) i]);
        }
        addChildComponent (reverseSwitch);

        for (int g = 0; g < kNumGroups; ++g)
        {
            addChildComponent (groupKeys[(size_t) g]);
            groupKeys[(size_t) g].onClick = [this, g]
            {
                if (groupAttachment != nullptr) groupAttachment->setValueAsCompleteGesture ((float) g);
            };
        }

        styleKnob (velKnob, 62);
        velKnob.setTooltip ("Velocity sensitivity: 0% = every hit at full level, 100% = level follows how hard you play");
        modeSel.setTooltip ("ONE: plays once.  LOOP: repeats while the pad / note is held (latched in a SWAP group).  SLICE: plays one slice (SLICE knob).  "
                            "SEQ1 / SEQL: a hit plays the pad's sequencer lane once / looped (hit a looping one again to stop it).");
        launchSel.setTooltip ("When a hit starts this pad's sequence: NOW, or the next 1/16, beat or bar");
        rateSel.setTooltip ("Loop speed: 1/4, 1/2, 1x or 2x");
        chokeSel.setTooltip ("Choke group: hitting a pad stops the other pads in the same group");
        // A13: STRETCH (click / drag / wheel; double-click: OFF; right-click or the tempo: the options).
        stretchSel.setGeometry (13.0f, 20.0f);
        stretchSel.onChange      = [this] (int k) { stretch::setMode (processor, pad, k); };
        stretchSel.onDoubleClick = [this] { stretch::setMode (processor, pad, params::stretchOff); };
        stretchSel.onRightClick  = [this] { stretch::showTempoMenu (processor, pad, tempoChip); };
        tempoChip.onClick        = [this] { stretch::showTempoMenu (processor, pad, tempoChip); };
        muteKey.setClickingTogglesState (true);
        fixVelKey.setClickingTogglesState (true);
        soloKey.setClickingTogglesState (true);

        for (Component* comp : std::initializer_list<Component*> { &modeSel, &stretchSel, &tempoChip, &rateSel, &chokeSel,
                                                                    &velKnob, &fixVelKey, &muteKey, &soloKey, &noteSel, &launchSel, &seqLenRead })
            addChildComponent (comp);

        noteSel.onNoteChanged = [this] (int n)
        {
            processor.getEngine().setPadNote (pad, n);
            processor.markStateChanged();
        };
        noteSel.onLearn = [this]
        {
            auto& e = processor.getEngine();
            e.startNoteLearn (e.getNoteLearnPad() == pad ? -1 : pad);   // press again to cancel
        };

        // ---- SLICE page
        sensKnob.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        sensKnob.setTextBoxStyle (Slider::TextBoxBelow, true, 62, 16);
        sensKnob.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        sensKnob.setRange (0.0, 100.0, 1.0);
        sensKnob.setTextValueSuffix (" %");
        sensKnob.setDoubleClickReturnValue (true, 50.0);
        sensKnob.setTooltip ("AUTO SLICE sensitivity: higher finds quieter hits (ghost notes); lower keeps only the big ones");
        sensKnob.onDragEnd = [this] { processor.getSamples().setSensitivity (pad, (float) sensKnob.getValue() / 100.0f); };
        sensKnob.onValueChange = [this] { if (! sensKnob.isMouseButtonDown()) processor.getSamples().setSensitivity (pad, (float) sensKnob.getValue() / 100.0f); };

        countSel.setTooltip ("How many slices: AUTO keeps the strongest this many hits; EQUAL cuts this many");
        countSel.onChange = [this] (int i) { processor.getSamples().setSliceCount (pad, i == 0 ? 4 : (i == 1 ? 8 : 16)); };

        styleKnob (sliceKnob, 62);
        sliceKnob.setTooltip ("Which slice this pad plays in SLICE mode (also: click a slice on the screen)");

        autoKey.onClick   = [this] { processor.getSamples().autoSlice (pad); };
        equalKey.onClick  = [this] { processor.getSamples().equalSlice (pad); };
        resetKey.onClick  = [this] { processor.getSamples().resetSlices (pad); };
        unsliceKey.onClick = [this] { processor.getSamples().clearSlices (pad); };
        spreadKey.onClick = [this] { processor.spreadSlices (pad); };
        fitKey.onClick    = [this] { if (onFitView) onFitView(); };

        for (Component* comp : sliceControls())
            addChildComponent (comp);

        // ---- SWAP page
        swapSel.setTooltip ("SWAP group: pads in the same group take turns. One plays (latched); hit another and it takes over at the next slice point, from the same place in its own break. Hit the playing pad again to stop.");
        swapAtSel.setTooltip ("When a swap onto this pad lands: the next SLICE marker of the playing break, or the next 1/16, beat or bar of Live's grid");
        addChildComponent (swapSel);
        addChildComponent (swapAtSel);
        followBankKey.setClickingTogglesState (true);
        addChildComponent (followBankKey);
        swapFromKey.setClickingTogglesState (true);
        addChildComponent (swapFromKey);

        // ---- SEQ page
        sseqKey.setClickingTogglesState (true);
        for (auto* comp : seqControls()) addChildComponent (comp);
        {
            auto slotModel = [this] (std::function<int (const SliceSlot&)> get, std::function<void (SliceSlot&, int)> set)
            {
                DragReadout::Model m;
                m.get = [this, get]
                {
                    const auto s = processor.getBridge().sliceSeq.getSlot (pad, processor.sliceSlotOf (pad));
                    return get (s);
                };
                m.set = [this, set] (int v)
                {
                    const int sl = processor.sliceSlotOf (pad);
                    processor.materializeSliceSlot (pad, sl);
                    auto s = processor.getBridge().sliceSeq.getSlot (pad, sl);
                    set (s, v);
                    processor.getBridge().sliceSeq.setSlot (pad, sl, s);
                    processor.markStateChanged();
                };
                return m;
            };
            auto m = slotModel ([] (const SliceSlot& s) { return s.length; }, [] (SliceSlot& s, int v) { s.length = v; });
            m.min = 1; m.max = 32; m.def = 16;
            sLenRead.bind (m);
            m = slotModel ([] (const SliceSlot& s) { return s.rate; }, [] (SliceSlot& s, int v) { s.rate = v; });
            m.min = 0; m.max = 3; m.def = 1; m.cycleOnClick = true; m.text = [] (int v) { return String (kSliceRateNames[jlimit (0, 3, v)]); };
            sRateRead.bind (m);
            m = slotModel ([] (const SliceSlot& s) { return s.swing; }, [] (SliceSlot& s, int v) { s.swing = v; });
            m.min = 0; m.max = 100; m.def = 0; m.cycleOnClick = false; m.text = [] (int v) { return String (v) + "%"; };
            swingRead.bind (m);
        }

        for (auto* k : { &soundTab, &playTab, &sliceTab, &swapTab, &seqTab, &loadKey, &clearKey })
            addAndMakeVisible (k);

        soundTab.onClick = [this] { setPage (Page::sound); };
        playTab.onClick  = [this] { setPage (Page::play); };
        sliceTab.onClick = [this] { setPage (Page::slice); };
        swapTab.onClick  = [this] { setPage (Page::swap); };
        seqTab.onClick   = [this] { setPage (Page::seq); };
        loadKey.onClick  = [this] { chooseFile(); };
        clearKey.onClick = [this] { processor.getSamples().clearPad (pad); };

        bindControls();
        setPage (Page::sound);
    }

    PadEditorPanel::~PadEditorPanel()
    {
        // Attachments must go before the controls they point at.
        for (auto& a : knobAttachments) a.reset();
        velAttachment.reset(); sliceAttachment.reset();
        reverseAttachment.reset(); muteAttachment.reset(); soloAttachment.reset(); fixVelAttachment.reset();
        modeSel.attach (nullptr); rateSel.attach (nullptr); chokeSel.attach (nullptr);
        launchSel.attach (nullptr); seqLenRead.attach (nullptr); swapFromAttachment.reset();
        swapSel.attach (nullptr); swapAtSel.attach (nullptr);
        groupAttachment.reset();
        sseqAttachment.reset();
        followBankAttachment.reset();
        slotRead.attach (nullptr);
    }

    void PadEditorPanel::showGroup (int g)
    {
        for (int k = 0; k < kNumGroups; ++k)
            groupKeys[(size_t) k].setToggleState (k == g, dontSendNotification);
    }

    std::vector<Component*> PadEditorPanel::seqControls()
    {
        return { &sseqKey, &slotRead, &sLenRead, &sRateRead, &swingRead };
    }

    std::vector<Component*> PadEditorPanel::sliceControls()
    {
        return { &autoKey, &equalKey, &sensKnob, &countSel, &sliceKnob, &resetKey, &unsliceKey, &spreadKey, &fitKey };
    }

    void PadEditorPanel::setPad (int newPad)
    {
        newPad = jlimit (0, kNumPads - 1, newPad);
        if (newPad == pad) return;
        pad = newPad;
        bindControls();
        shownVersion = 0xffffffff;
        refresh();
        repaint();
    }

    bool PadEditorPanel::isSeqModeNow() const
    {
        return params::isSeqMode (roundToInt (processor.getParameters().getRawParameterValue (params::padId (pad, params::padMode))->load()));
    }

    void PadEditorPanel::setPage (Page p)
    {
        page = p;
        const bool sound = page == Page::sound, play = page == Page::play, slice = page == Page::slice,
                   swap = page == Page::swap, seq = page == Page::seq;
        seqTab.setToggleState (seq, dontSendNotification);
        for (auto* comp : seqControls()) comp->setVisible (seq);
        soundTab.setToggleState (sound, dontSendNotification);
        playTab.setToggleState (play, dontSendNotification);
        sliceTab.setToggleState (slice, dontSendNotification);
        swapTab.setToggleState (swap, dontSendNotification);
        swapSel.setVisible (swap);
        swapAtSel.setVisible (swap);
        followBankKey.setVisible (swap);
        swapFromKey.setVisible (swap);

        for (auto& k : knobs) k.setVisible (sound);
        reverseSwitch.setVisible (sound);
        for (auto& k : groupKeys) k.setVisible (sound);
        for (Component* comp : std::initializer_list<Component*> { &modeSel, &chokeSel,
                                                                    &velKnob, &fixVelKey, &muteKey, &soloKey, &noteSel })
            comp->setVisible (play);
        shownSeqMode = isSeqModeNow();
        stretchSel.setVisible (play && ! shownSeqMode);
        tempoChip.setVisible (play && ! shownSeqMode);
        rateSel.setVisible (play && ! shownSeqMode);
        launchSel.setVisible (play && shownSeqMode);
        seqLenRead.setVisible (play && shownSeqMode);
        for (Component* comp : sliceControls())
            comp->setVisible (slice);

        if (onPageChanged) onPageChanged (page);
        repaint();
    }

    void PadEditorPanel::bindControls()
    {
        auto& apvts = processor.getParameters();
        auto id = [this] (const char* name) { return params::padId (pad, name); };

        for (int i = 0; i < kNumKnobs; ++i)
        {
            knobAttachments[(size_t) i].reset();
            knobAttachments[(size_t) i] = std::make_unique<SliderAttachment> (apvts, id (kKnobs[i].param), knobs[(size_t) i]);
            knobs[(size_t) i].setDoubleClickReturnValue (true, kKnobs[i].resetTo);
        }

        velAttachment.reset();
        velAttachment = std::make_unique<SliderAttachment> (apvts, id (params::padVelSens), velKnob);
        velKnob.setDoubleClickReturnValue (true, 1.0);

        sliceAttachment.reset();
        sliceAttachment = std::make_unique<SliderAttachment> (apvts, id (params::padSlice), sliceKnob);
        sliceKnob.setDoubleClickReturnValue (true, 1.0);

        reverseAttachment.reset(); muteAttachment.reset(); soloAttachment.reset(); fixVelAttachment.reset();
        fixVelAttachment  = std::make_unique<ButtonAttachment> (apvts, id (params::padFixVel),  fixVelKey);
        reverseAttachment = std::make_unique<ButtonAttachment> (apvts, id (params::padReverse), reverseSwitch);
        muteAttachment    = std::make_unique<ButtonAttachment> (apvts, id (params::padMute),    muteKey);
        soloAttachment    = std::make_unique<ButtonAttachment> (apvts, id (params::padSolo),    soloKey);

        modeSel.attach  (apvts.getParameter (id (params::padMode)));
        rateSel.attach  (apvts.getParameter (id (params::padRate)));
        chokeSel.attach (apvts.getParameter (id (params::padChoke)));
        swapSel.attach  (apvts.getParameter (id (params::padSwap)));
        swapAtSel.attach (apvts.getParameter (id (params::padSwapAt)));
        launchSel.attach (apvts.getParameter (id (params::padLaunch)));
        seqLenRead.attach (apvts.getParameter (id (params::padSeqLen)));
        swapFromAttachment.reset();
        swapFromAttachment = std::make_unique<ButtonAttachment> (apvts, id (params::padSwapFrom), swapFromKey);
        sseqAttachment.reset();
        sseqAttachment = std::make_unique<ButtonAttachment> (apvts, id (params::padSliceSeq), sseqKey);
        followBankAttachment.reset();
        followBankAttachment = std::make_unique<ButtonAttachment> (apvts, id (params::padFollowBank), followBankKey);
        slotRead.attach (apvts.getParameter (id (params::padSliceSlot)));

        noteSel.setNote (processor.getEngine().getPadNote (pad));

        groupAttachment.reset();
        if (auto* prm = apvts.getParameter (id (params::padGroup)))
        {
            groupAttachment = std::make_unique<ParameterAttachment> (*prm, [this] (float v) { showGroup (roundToInt (v)); });
            groupAttachment->sendInitialUpdate();
        }
    }

    void PadEditorPanel::chooseFile()
    {
        const int target = pad;
        chooser = std::make_unique<FileChooser> ("Load a sample into pad " + String (target + 1),
                                                 lastFolder(), "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac");

        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this, target] (const FileChooser& fc)
                              {
                                  const auto file = fc.getResult();
                                  if (file.existsAsFile())
                                  {
                                      lastFolder() = file.getParentDirectory();
                                      processor.getSamples().loadFile (target, file);
                                  }
                              });
    }

    void PadEditorPanel::refresh()
    {
        if (page == Page::seq)
        {
            const auto sv = processor.getBridge().sliceSeq.getVersion();
            if (sv != shownSliceVersion)
            {
                shownSliceVersion = sv;
                for (auto* r : { &sLenRead, &sRateRead, &swingRead, &slotRead }) r->repaint();
            }
        }

        const auto v = processor.getSamples().getVersion (pad);
        if (v != shownVersion)
        {
            shownVersion = v;
            clearKey.setEnabled (processor.getSamples().getInfo (pad).status != SampleManager::Status::empty);
            repaint();
        }

        // Slicer controls follow this pad's slicer settings.
        if (page == Page::slice)
        {
            const auto info = processor.getSamples().getInfo (pad);
            if (! sensKnob.isMouseButtonDown())
                sensKnob.setValue (std::round (info.sensitivity * 100.0f), dontSendNotification);
            countSel.setIndex (info.sliceCount <= 4 ? 0 : (info.sliceCount <= 8 ? 1 : 2));
            const bool hasSample = info.buffer != nullptr;
            autoKey.setEnabled (hasSample && ! info.slicing);
            autoKey.setBlinking (info.slicing);
            autoKey.tick();
            equalKey.setEnabled (hasSample);
            resetKey.setEnabled (! info.detected.isEmpty() && info.detected != info.slices);
            unsliceKey.setEnabled (! info.slices.isEmpty());
            spreadKey.setEnabled (hasSample && ! info.slices.isEmpty() && pad % kPadsPerScene < kPadsPerScene - 1);
            sliceKnob.setAlpha (info.slices.isEmpty() ? 0.45f : 1.0f);
        }

        auto& engine = processor.getEngine();
        noteSel.setNote (engine.getPadNote (pad));
        noteSel.setLearning (engine.getNoteLearnPad() == pad);
        noteSel.tick();

        // VEL does nothing while FIXED is on.
        velKnob.setAlpha (fixVelKey.getToggleState() ? 0.4f : 1.0f);

        // SEQ modes swap SYNC / RATE for LAUNCH / LEN.
        if (isSeqModeNow() != shownSeqMode) setPage (page);

        // Rate only matters for loops; dim it otherwise.
        const bool loop = modeSel.getIndex() == params::modeLoop;
        rateSel.setAlpha (loop ? 1.0f : 0.45f);

        // A13: STRETCH and the break's own tempo.
        const int em = stretch::effectiveMode (processor, pad);
        stretchSel.setIndex (em);
        {
            const auto si = processor.getSamples().getInfo (pad);
            auto rawP = [this] (const char* n) { return processor.getParameters().getRawParameterValue (params::padId (pad, n))->load(); };
            const String key = String::toHexString ((pointer_sized_int) si.buffer.get()) + ":" + si.slices.toString()
                             + ":" + String (rawP (params::padOrigBpm)) + ":" + String (rawP (params::padStart)) + ":" + String (rawP (params::padEnd))
                             + ":" + String (modeSel.getIndex()) + ":" + String (pad);
            if (key != shownTempoKey)
            {
                shownTempoKey = key;
                bool set = false;
                const double own = stretch::ownTempo (processor, pad, set);
                tempoChip.text = own > 0.0 ? String (own, 1) : String ("--");
                tempoChip.set = set;
                tempoChip.setTooltip (own > 0.0 ? "The break's own tempo: " + String (own, 2) + " BPM, " + (set ? "typed in" : "worked out")
                                                  + ". Click for x2, /2, type it in, BEATS gaps and the grain / cycle."
                                                : String ("No sample"));
                tempoChip.repaint();
            }
        }
        stretchSel.setTooltip ("STRETCH - " + stretch::describe (em)
                               + ".  OFF / PITCH / BEATS / TEXTURE / AKAI / AKAI REVISED, for one-shots, slices and loops (a loop stays on Live's bar grid unless OFF). "
                                 "Double-click: OFF.  Right-click or the tempo above: options.");
    }

    //==========================================================================
    Rectangle<int> PadEditorPanel::knobCell (int index) const
    {
        return { index * kKnobW, kCtlTop, kKnobW, getHeight() - kCtlTop };
    }

    void PadEditorPanel::resized()
    {
        const int w = getWidth();
        soundTab.setBounds (63,  0, 60, kHeaderH);
        playTab.setBounds  (124, 0, 47, kHeaderH);
        sliceTab.setBounds (172, 0, 51, kHeaderH);
        swapTab.setBounds  (224, 0, 51, kHeaderH);
        seqTab.setBounds   (276, 0, 42, kHeaderH);

        sseqKey.setBounds   (0,   kCtlTop - 6, 110, 34);
        slotRead.setBounds  (124, kCtlTop - 10, 60, 40);
        sLenRead.setBounds  (196, kCtlTop - 10, 60, 40);
        sRateRead.setBounds (268, kCtlTop - 10, 60, 40);
        swingRead.setBounds (340, kCtlTop - 10, 60, 40);

        swapSel.setBounds   (0,   kCtlTop - 6, 104, getHeight() - kCtlTop + 6);
        swapAtSel.setBounds (108, kCtlTop - 6, 104, getHeight() - kCtlTop + 6);
        followBankKey.setBounds (222, kCtlTop - 20, 112, 34);
        swapFromKey.setBounds (222, kCtlTop + 22, 112, 34);
        loadKey.setBounds  (w - 136, 0, 66, kHeaderH);
        clearKey.setBounds (w - 68,  0, 68, kHeaderH);

        {
            const int h = getHeight() - kCtlTop;
            const int top = kCtlTop - 14, half = (h + 14) / 2;
            autoKey.setBounds   (0,   top,        104, half - 2);
            equalKey.setBounds  (0,   top + half, 104, half - 2);
            sensKnob.setBounds  (108, kCtlTop, 72, h);
            countSel.setBounds  (182, kCtlTop - 6, 90, h + 6);
            sliceKnob.setBounds (274, kCtlTop, 72, h);
            resetKey.setBounds  (352, top,        92, half - 2);
            unsliceKey.setBounds (352, top + half, 92, half - 2);
            spreadKey.setBounds (448, top,        92, half - 2);
            fitKey.setBounds    (448, top + half, 92, half - 2);
        }

        for (int i = 0; i < kNumKnobs; ++i)
            knobs[(size_t) i].setBounds (knobCell (i).reduced (4, 0));
        reverseSwitch.setBounds (kNumKnobs * kKnobW + 8, kCtlTop - 10, 66, getHeight() - kCtlTop + 10);
        {
            // GROUP: A B / C D keys in the last column.
            const int x0 = kNumKnobs * kKnobW + 82, kw = (w - x0) / 2;
            const int y0 = kCtlTop - 4, kh = (getHeight() - y0) / 2;
            for (int k = 0; k < kNumGroups; ++k)
                groupKeys[(size_t) k].setBounds (x0 + (k % 2) * kw, y0 + (k / 2) * kh, kw - 3, kh - 3);
        }

        const int h = getHeight() - kCtlTop;
        auto col = [&] (int i) { return Rectangle<int> (kPlayCols[i].x, kCtlTop, kPlayCols[i].w, h); };
        modeSel.setBounds   (col (0).withTrimmedTop (-6));
        stretchSel.setBounds (col (1).withTrimmedTop (-6));
        tempoChip.setBounds (kPlayCols[1].x + kPlayCols[1].w - 46, kCtlTop - 21, 46, 17);
        rateSel.setBounds   (col (2).withTrimmedTop (-6));
        launchSel.setBounds (col (2).withTrimmedTop (-6));
        seqLenRead.setBounds (col (1).withTrimmedTop (-18).withHeight (44));
        chokeSel.setBounds  (col (3).withTrimmedTop (-6));
        velKnob.setBounds   (col (4));
        fixVelKey.setBounds (col (5).withTrimmedTop (-14).reduced (0, 2));
        auto ms = col (6).withTrimmedTop (-14);
        muteKey.setBounds (ms.removeFromTop (ms.getHeight() / 2).reduced (0, 2));
        soloKey.setBounds (ms.reduced (0, 2));
        noteSel.setBounds (col (7).withTrimmedTop (2).withTrimmedBottom (6));
    }

    void PadEditorPanel::paint (Graphics& g)
    {
        Random rnd (pad + 77);
        paint::labelPlate (g, { 0.0f, 0.0f, 60.0f, (float) kHeaderH },
                           "PAD " + padLabel (pad), {}, paint::PlateStyle::black, rnd);

        // Sample readout in a recess between the tabs and LOAD.
        auto r = Rectangle<float> (320.0f, 0.0f, (float) (getWidth() - 320 - 140), (float) kHeaderH);
        paint::recess (g, r, 3.0f);
        r.reduce (8.0f, 0.0f);

        const auto info = processor.getSamples().getInfo (pad);
        String text;
        Colour col = c::vfd;
        switch (info.status)
        {
            case SampleManager::Status::empty:   text = "EMPTY - LOAD OR DROP"; col = c::vfd.withAlpha (0.55f); break;
            case SampleManager::Status::loading: text = "LOADING " + info.name.toUpperCase(); break;
            case SampleManager::Status::ready:
                text = info.name.toUpperCase();
                if (! info.slices.isEmpty()) text << "  " << info.slices.count << " SL";
                break;
            case SampleManager::Status::missing:
            case SampleManager::Status::unreadable:
            case SampleManager::Status::tooLong:
                text = SampleManager::statusText (info.status) + ": " + info.file.getFileName();
                col  = c::red;
                break;
        }
        g.setColour (col);
        g.setFont (monoFont (13.0f));
        g.drawText (text, r, Justification::centredLeft, true);

        // Captions for the current page.
        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (13.0f));
        if (page == Page::sound)
        {
            for (int i = 0; i < kNumKnobs; ++i)
                g.drawText (kKnobs[i].caption, knobCell (i).withY (kCtlTop - 16).withHeight (14).toFloat(),
                            Justification::centred, false);
            const float gx = (float) (kNumKnobs * kKnobW + 82);
            g.drawText ("GROUP", Rectangle<float> (gx, (float) kCtlTop - 20.0f, (float) getWidth() - gx, 14.0f),
                        Justification::centred, false);
        }
        else if (page == Page::slice)
        {
            g.drawText ("SENS",  Rectangle<float> (108.0f, (float) kCtlTop - 16.0f, 72.0f, 14.0f), Justification::centred, false);
            g.drawText ("COUNT", Rectangle<float> (182.0f, (float) kCtlTop - 18.0f, 90.0f, 14.0f), Justification::centred, false);
            g.drawText ("SLICE", Rectangle<float> (274.0f, (float) kCtlTop - 16.0f, 72.0f, 14.0f), Justification::centred, false);
        }
        else if (page == Page::seq)
        {
            g.setColour (c::bakelite.withAlpha (0.75f));
            g.setFont (monoFont (10.5f));
            g.drawFittedText ("STEPS ON THE SCREEN.\nHIT THE PAD: START / STOP.", Rectangle<int> (410, kCtlTop - 10, getWidth() - 410, 44),
                              Justification::centredLeft, 2, 1.0f);
        }
        else if (page == Page::swap)
        {
            g.drawText ("GROUP", Rectangle<float> (0.0f,   (float) kCtlTop - 18.0f, 104.0f, 14.0f), Justification::centred, false);
            g.drawText ("AT",    Rectangle<float> (108.0f, (float) kCtlTop - 18.0f, 104.0f, 14.0f), Justification::centred, false);

            // How it works, on a recessed instruction plate.
            auto help = Rectangle<float> (342.0f, (float) kCtlTop - 20.0f, (float) getWidth() - 342.0f, (float) (getHeight() - kCtlTop + 20));
            paint::recess (g, help, 3.0f);
            help.reduce (8.0f, 4.0f);
            g.setColour (c::vfd.withAlpha (0.85f));
            g.setFont (monoFont (11.0f));
            g.drawFittedText ("ONE GROUP: PADS TAKE TURNS.\n"
                              "HIT ANOTHER: TAKES OVER AT\n"
                              "THE NEXT SLICE (SEQ PADS:\n"
                              "STEP). HIT AGAIN TO STOP.",
                              help.toNearestInt(), Justification::centredLeft, 4, 1.0f);
        }
        else
        {
            for (const auto& cdef : kPlayCols)
            {
                const String cap (cdef.caption);
                if (cap == "STRETCH")
                {
                    if (! shownSeqMode)       // (LEN draws its own caption)
                        g.drawText (cap, Rectangle<float> ((float) cdef.x, (float) kCtlTop - 18.0f, (float) cdef.w - 48.0f, 14.0f),
                                    Justification::centred, false);
                }
                else if (cap.isNotEmpty())
                    g.drawText (shownSeqMode && cap == "RATE" ? String ("LAUNCH") : cap,
                                Rectangle<float> ((float) cdef.x, (float) kCtlTop - 18.0f, (float) cdef.w, 14.0f),
                                Justification::centred, false);
            }
        }
    }
}

namespace tb::ui
{
    void PadEditorPanel::TempoChip::paint (Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        paint::recess (g, b, 3.0f);
        g.setColour (set ? c::amber : c::vfd);
        g.setFont (monoFont (11.5f));
        g.drawText (text, b, Justification::centred, false);
    }
}
