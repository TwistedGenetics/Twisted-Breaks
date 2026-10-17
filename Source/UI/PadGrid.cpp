#include "UI/PadGrid.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    PadGrid::PadGrid (TwistedBreaksProcessor& p) : processor (p)
    {
        shownNotes.fill (-2);
        shownVersions.fill (0xffffffff);

        for (int i = 0; i < kPadsPerScene; ++i)
        {
            auto pad = std::make_unique<PadComponent> (i);

            pad->onPress       = [this] (int idx, float vel) { processor.triggerPadFromUi (idx, vel, true); };
            pad->onRelease     = [this] (int idx)            { processor.triggerPadFromUi (idx, 0.0f, false); };
            pad->onSelect      = [this] (int idx)            { setSelectedPad (idx); };
            pad->onFileDropped = [this] (int idx, const juce::File& f) { processor.getSamples().loadFile (idx, f); };
            pad->onMenu        = [this] (int idx)            { showPadMenu (idx); };
            pad->onDragOnto    = [this] (int idx, juce::Point<int> at, bool swap)
            {
                for (auto& t : pads)
                    if (t->getPadIndex() != idx && t->getScreenBounds().contains (at))
                    {
                        if (swap) processor.getPadOps().swap (idx, t->getPadIndex());
                        else      processor.getPadOps().copyPad (idx, t->getPadIndex());
                        setSelectedPad (t->getPadIndex());
                        return;
                    }
            };

            addAndMakeVisible (*pad);
            pads[(size_t) i] = std::move (pad);
        }

        setSelectedPad (0);
        refresh();
    }

    void PadGrid::showPadMenu (int pad)
    {
        auto& ops = processor.getPadOps();
        juce::PopupMenu m;
        const auto name = "PAD " + padLabel (pad);
        m.addSectionHeader (name);
        m.addItem ("Copy", [&ops, pad] { ops.copy (pad); });
        const bool clip = ops.hasClip();
        const auto from = clip ? " (from " + padLabel (ops.clipSource()) + ")" : juce::String();
        m.addItem ("Paste" + from, clip, false, [&ops, pad] { ops.paste (pad); });
        juce::PopupMenu special;
        special.addItem ("Sound only (sample, slices, settings)", clip, false, [&ops, pad] { ops.paste (pad, PadOps::sound); });
        special.addItem ("Steps only (sequencer lane, slice sequence)", clip, false, [&ops, pad] { ops.paste (pad, PadOps::steps); });
        special.addItem ("Settings only", clip, false, [&ops, pad] { ops.paste (pad, PadOps::settings); });
        m.addSubMenu ("Paste special", special, clip);

        auto padList = [pad] (std::function<void (int)> fn)
        {
            juce::PopupMenu sub;
            for (int sc = 0; sc < kNumScenes; ++sc)
            {
                if (sc > 0) sub.addSeparator();
                for (int i = 0; i < kPadsPerScene; ++i)
                {
                    const int t = sc * kPadsPerScene + i;
                    sub.addItem ("Pad " + padLabel (t), t != pad, false, [fn, t] { fn (t); });
                }
            }
            return sub;
        };
        m.addSubMenu ("Copy to", padList ([&ops, pad] (int t) { ops.copyPad (pad, t); }));
        m.addSubMenu ("Swap with", padList ([&ops, pad] (int t) { ops.swap (pad, t); }));
        m.addSeparator();
        m.addItem ("Clear pad (sample and steps)", [&ops, pad] { ops.clear (pad); });
        m.addItem ("TWIST", [&ops, pad] { ops.twist (pad); });

        // A11: fit to tempo, layers.
        auto& apvts = processor.getParameters();
        auto toggle = [this] (const juce::String& id, float v)
        {
            if (auto* r = processor.getParameters().getParameter (id)) { r->beginChangeGesture(); r->setValueNotifyingHost (r->convertTo0to1 (v)); r->endChangeGesture(); }
        };
        const auto info = processor.getSamples().getInfo (pad);
        const double bpm = info.buffer != nullptr ? AudioEngine::tempoOf (*info.buffer) : 0.0;
        // A12: STRETCH (0.21's FIT is Repitch now).
        {
            const auto rawOf = [&apvts, pad] (const char* n) { return apvts.getRawParameterValue (params::padId (pad, n))->load(); };
            int mode = juce::roundToInt (rawOf (params::padStretch));
            if (mode == params::stretchOff && rawOf (params::padFit) > 0.5f) mode = params::stretchRepitch;
            const float obpm = rawOf (params::padOrigBpm);
            const double detected = info.buffer != nullptr ? AudioEngine::detectTempo (*info.buffer, info.slices) : 0.0;
            const double own = obpm >= 1.0f ? (double) obpm : detected;
            juce::PopupMenu st;
            const char* tips[] = { "plays at its own speed", "speed follows Live, pitch moves with it", "each slice on Live's grid, pitch kept",
                                   "grains: pitch kept, smooth", "the 90s S950 / S1000 cyclic stretch, metallic", "cyclic, in-phase splices: cleaner" };
            for (int k = 0; k < 6; ++k)
                st.addItem (juce::String (params::kStretchNames[k]) + "  -  " + tips[k], true, mode == k, [toggle, pad, k]
                {
                    toggle (params::padId (pad, params::padStretch), (float) k);
                    toggle (params::padId (pad, params::padFit), 0.0f);
                });
            st.addSeparator();
            st.addSectionHeader ("THE BREAK'S OWN TEMPO: " + (own > 0.0 ? juce::String (own, 2) + " BPM" : juce::String ("-"))
                                 + (obpm >= 1.0f ? " (SET)" : " (DETECTED)"));
            st.addItem ("Double it (x2)", own > 0.0, false, [toggle, pad, own] { toggle (params::padId (pad, params::padOrigBpm), (float) juce::jmin (300.0, own * 2.0)); });
            st.addItem ("Halve it (/2)", own > 0.0, false, [toggle, pad, own] { toggle (params::padId (pad, params::padOrigBpm), (float) juce::jmax (40.0, own / 2.0)); });
            st.addItem ("Type it in...", [this, pad, own]
            {
                auto* w = new juce::AlertWindow ("Original tempo", "The tempo this break was played at (BPM)", juce::MessageBoxIconType::NoIcon, this);
                w->addTextEditor ("bpm", own > 0.0 ? juce::String (own, 2) : juce::String());
                w->addButton ("SET", 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, pad] (int r)
                {
                    const double v = w->getTextEditorContents ("bpm").getDoubleValue();
                    if (r != 0 && v >= 40.0 && v <= 300.0)
                        if (auto* p = processor.getParameters().getParameter (params::padId (pad, params::padOrigBpm)))
                        { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 ((float) v)); p->endChangeGesture(); }
                }), true);
            });
            st.addItem ("Detect it (" + juce::String (detected, 2) + " BPM)", obpm >= 1.0f, false, [toggle, pad] { toggle (params::padId (pad, params::padOrigBpm), 0.0f); });
            st.addSeparator();
            juce::PopupMenu gaps, grains;
            const int gap = juce::roundToInt (rawOf (params::padGap));
            for (int k = 0; k < 3; ++k) gaps.addItem (params::kGapNames[k], true, gap == k, [toggle, pad, k] { toggle (params::padId (pad, params::padGap), (float) k); });
            st.addSubMenu ("BEATS: fill the gaps", gaps);
            const float gr = rawOf (params::padGrain);
            for (float ms : { 8.0f, 12.0f, 18.0f, 25.0f, 30.0f, 40.0f, 60.0f, 90.0f, 140.0f })
                grains.addItem (juce::String (ms, 0) + " ms", true, std::abs (gr - ms) < 0.5f, [toggle, pad, ms] { toggle (params::padId (pad, params::padGrain), ms); });
            st.addSubMenu ("TEXTURE grain / AKAI cycle (" + juce::String (gr, 0) + " ms)", grains);
            m.addSubMenu ("Stretch: " + juce::String (params::kStretchNames[juce::jlimit (0, 5, mode)])
                          + (own > 0.0 ? "  (" + juce::String (own, 1) + " BPM)" : juce::String()), st);
        }
        m.addItem ("Render Akai... (the cyclic stretch, into a new pad)", info.buffer != nullptr, false, [this, pad] { showAkaiDialog (pad); });
        juce::PopupMenu layers;
        const int lm = juce::roundToInt (apvts.getRawParameterValue (params::padId (pad, params::padLayers))->load());
        for (int k = 0; k < 4; ++k)
            layers.addItem (juce::String ("Play: ") + params::kLayerModeNames[k], true, lm == k,
                            [toggle, pad, k] { toggle (params::padId (pad, params::padLayers), (float) k); });
        layers.addSeparator();
        for (int l = 0; l < SampleManager::PadInfo::kLayers; ++l)
        {
            const auto& L = info.layers[(size_t) l];
            if (L.status == SampleManager::Status::empty) continue;
            juce::PopupMenu one;
            one.addItem ("Replace...", [this, pad, l] { chooseLayerFile (pad, l); });
            one.addItem ("Remove", [this, pad, l] { processor.getSamples().clearLayer (pad, l); });
            layers.addSubMenu ("Layer " + juce::String (l + 1) + ": " + L.name.toUpperCase()
                               + (L.status == SampleManager::Status::missing ? " (MISSING)" : L.status == SampleManager::Status::loading ? " (LOADING)" : ""), one);
        }
        layers.addItem ("Add a layer...", info.layerCount() < SampleManager::PadInfo::kLayers && info.buffer != nullptr, false,
                        [this, pad] { chooseLayerFile (pad, -1); });
        m.addSubMenu ("Layers" + (info.layerCount() > 0 ? " (" + juce::String (info.layerCount()) + ")" : juce::String()), layers);
        m.addSeparator();
        m.addItem ("Undo " + ops.undoName(), ops.canUndo(), false, [&ops] { ops.undo(); });
        m.addItem ("Redo " + ops.redoName(), ops.canRedo(), false, [&ops] { ops.redo(); });
        m.addSeparator();
        m.addItem ("Tip: Alt-drag a pad to copy it, Shift-drag to swap", false, false, [] {});

        juce::Component* target = this;
        for (auto& t : pads) if (t->getPadIndex() == pad) target = t.get();
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
    }

    void PadGrid::showAkaiDialog (int pad)
    {
        auto* w = new juce::AlertWindow ("Render Akai: pad " + padLabel (pad),
                                         "The Akai S-series cyclic stretch on this pad's sample. The result goes onto the next empty pad.",
                                         juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("time", "100", "Time factor % (25 - 2000)");
        w->addTextEditor ("pitch", "0", "Pitch, semitones (-36 .. +36)");
        w->addTextEditor ("cycle", "25", "Cycle, ms (5 - 150)");
        w->addComboBox ("mode", { "Classic", "Revised" }, "Mode");
        w->addButton ("RENDER", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, pad] (int r)
        {
            if (r == 0) return;
            const double tf = juce::jlimit (25.0, 2000.0, w->getTextEditorContents ("time").getDoubleValue()) / 100.0;
            const float st  = juce::jlimit (-36.0f, 36.0f, w->getTextEditorContents ("pitch").getFloatValue());
            const float cy  = juce::jlimit (5.0f, 150.0f, w->getTextEditorContents ("cycle").getFloatValue());
            const bool rev  = w->getComboBoxComponent ("mode")->getSelectedItemIndex() == 1;
            juce::File out;
            const auto res = processor.getExporter().renderAkai (pad, tf, st, cy, rev, out);
            if (res.failed()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Render Akai", res.getErrorMessage()); return; }
            int target = -1;
            for (int k = 1; k <= kNumPads && target < 0; ++k)
            {
                const int p = (pad + k) % kNumPads;
                if (processor.getSamples().getInfo (p).status == SampleManager::Status::empty) target = p;
            }
            if (target < 0) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Render Akai", "No empty pad: the file is in " + out.getFullPathName()); return; }
            processor.getSamples().loadFile (target, out);
            setSelectedPad (target);
        }), true);
    }

    void PadGrid::chooseLayerFile (int pad, int layer)
    {
        chooser = std::make_unique<juce::FileChooser> ("A sound for pad " + padLabel (pad) + (layer < 0 ? " (new layer)" : " (layer " + juce::String (layer + 1) + ")"),
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, pad, layer] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            auto& sm = processor.getSamples();
            if (layer < 0) sm.addLayer (pad, f); else sm.loadLayer (pad, layer, f);
            // Adding the first layer: switch the pad to round robin if it was off.
            if (auto* r = processor.getParameters().getParameter (params::padId (pad, params::padLayers)))
                if (juce::roundToInt (r->convertFrom0to1 (r->getValue())) == params::layersOff)
                { r->beginChangeGesture(); r->setValueNotifyingHost (r->convertTo0to1 ((float) params::layersRoundRobin)); r->endChangeGesture(); }
        });
    }

    void PadGrid::resized()
    {
        const int gap  = 10, gapV = 4;        // tighter rows since the scene strip (0.13)
        const int cell = (getWidth()  - gap * 3) / 4;
        const int rowH = (getHeight() - gapV * 3) / 4;

        for (int i = 0; i < kPadsPerScene; ++i)
            // 01 bottom left .. 16 top right, like an MPC / Maschine / a Drum Rack (0.16.1).
            pads[(size_t) i]->setBounds ((i % 4) * (cell + gap), (3 - i / 4) * (rowH + gapV), cell, rowH);
    }

    void PadGrid::setSelectedPad (int pad)
    {
        selectedPad = juce::jlimit (0, kNumPads - 1, pad);
        if (sceneOf (selectedPad) != scene) setScene (sceneOf (selectedPad));
        for (int i = 0; i < kPadsPerScene; ++i)
            pads[(size_t) i]->setSelected (pads[(size_t) i]->getPadIndex() == selectedPad);
        if (onSelectionChanged) onSelectionChanged (selectedPad);
    }

    void PadGrid::setScene (int s)
    {
        s = juce::jlimit (0, kNumScenes - 1, s);
        if (s == scene) return;
        scene = s;
        for (int i = 0; i < kPadsPerScene; ++i)
        {
            pads[(size_t) i]->setPadIndex (scene * kPadsPerScene + i);
            pads[(size_t) i]->setSelected (scene * kPadsPerScene + i == selectedPad);
        }
        shownNotes.fill (-2);
        shownVersions.fill (0xffffffff);
        shownTags.fill ({});
        refresh();
    }

    void PadGrid::refresh()
    {
        auto& bridge  = processor.getBridge();
        auto& engine  = processor.getEngine();
        auto& samples = processor.getSamples();

        bool anySolo = false;
        for (int i = 0; i < kNumPads; ++i)
            anySolo = anySolo || processor.getParameters().getRawParameterValue (params::padId (i, params::padSolo))->load() > 0.5f;

        for (int t = 0; t < kPadsPerScene; ++t)
        {
            auto& pad = *pads[(size_t) t];
            const int i = pad.getPadIndex();      // engine pad
            pad.setActivity (bridge.padActivity[(size_t) i].load (std::memory_order_relaxed));

            auto raw = [&] (const char* name) { return processor.getParameters().getRawParameterValue (params::padId (i, name))->load(); };
            const bool muted  = raw (params::padMute) > 0.5f;
            const bool soloed = raw (params::padSolo) > 0.5f;
            pad.setMuteSolo (muted, soloed, muted || (anySolo && ! soloed));
            pad.setQueued (bridge.padQueued[(size_t) i].load (std::memory_order_relaxed));
            pad.setGroup (juce::roundToInt (raw (params::padGroup)));

            // Footer tag: playback mode, choke and SWAP group, e.g. "LOOP C2 SW1".
            const int mode  = juce::roundToInt (raw (params::padMode));
            const int choke = juce::roundToInt (raw (params::padChoke));
            const int swap  = juce::roundToInt (raw (params::padSwap));
            const juce::String modeTag = mode == params::modeSeqOne  ? juce::String ("SEQ1")
                                       : mode == params::modeSeqLoop ? juce::String ("SEQL")
                                       : mode == params::modeLoop  ? juce::String ("LOOP")
                                       : mode == params::modeSlice ? "SL " + juce::String (juce::roundToInt (raw (params::padSlice)))
                                                                   : juce::String ("ONE");
            const juce::String tag = modeTag + (choke > 0 ? " C" + juce::String (choke) : juce::String())
                                             + (swap > 0 ? " SW" + juce::String (swap) : juce::String())
                                             + (raw (params::padFollowBank) > 0.5f ? juce::String (" BK") : juce::String())
                                             + juce::String (std::array<const char*, 5> { "", " SP12", " MPC", " S950", " LOFI" }[(size_t) juce::jlimit (0, 4, juce::roundToInt (raw (params::padCharacter)))])
                                             + [&] { int sm = juce::roundToInt (raw (params::padStretch));
                                                     if (sm == params::stretchOff && raw (params::padFit) > 0.5f) sm = params::stretchRepitch;
                                                     const char* tags[] = { "", " RP", " BT", " TX", " AK", " AKR" };
                                                     return juce::String (tags[juce::jlimit (0, 5, sm)]); }()
                                             + [&] { const int n = samples.getInfo (i).layerCount();
                                                     const int lm = juce::roundToInt (raw (params::padLayers));
                                                     return n > 0 && lm != params::layersOff ? (lm == params::layersRoundRobin ? " RR" : lm == params::layersVelocity ? " VL" : " RN") + juce::String (n + 1)
                                                                                             : juce::String(); }();

            const int  note    = engine.getPadNote (i);
            const auto version = samples.getVersion (i);
            if (note == shownNotes[(size_t) t] && version == shownVersions[(size_t) t] && tag == shownTags[(size_t) t])
                continue;

            shownNotes[(size_t) t]    = note;
            shownVersions[(size_t) t] = version;
            shownTags[(size_t) t]     = tag;

            const auto info = samples.getInfo (i);
            using S = SampleManager::Status;

            juce::String upper, name;
            bool warn = false;
            switch (info.status)
            {
                case S::empty:   upper = "EMPTY";   name = "-";  break;
                case S::loading: upper = "LOADING"; name = info.name; break;
                case S::ready:
                    upper = juce::String (info.buffer->getLengthSeconds(), 2) + " s  "
                          + (info.buffer->getNumChannels() > 1 ? "ST" : "MONO");
                    name  = info.name;
                    break;
                case S::missing:
                case S::unreadable:
                case S::tooLong:
                    upper = SampleManager::statusText (info.status);
                    name  = info.name;
                    warn  = true;
                    break;
            }

            pad.setLabels (upper, name.toUpperCase(), note, tag, warn, info.buffer != nullptr);
        }
    }
}
