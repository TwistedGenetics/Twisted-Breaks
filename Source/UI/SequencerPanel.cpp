#include "UI/SequencerPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    //==========================================================================
    //  DragReadout
    //==========================================================================
    DragReadout::DragReadout (const String& cap, const String& tip) : caption (cap)
    {
        setTooltip (tip);
    }

    void DragReadout::attach (RangedAudioParameter* p)
    {
        att.reset();
        param = p;
        if (param != nullptr)
        {
            att = std::make_unique<ParameterAttachment> (*param, [this] (float) { repaint(); });
            att->sendInitialUpdate();
        }
    }

    void DragReadout::stepBy (int delta)
    {
        if (param == nullptr && model.get)
        {
            model.set (jlimit (model.min, model.max, model.get() + delta));
            repaint();
            return;
        }
        if (param == nullptr || att == nullptr) return;
        const auto& range = param->getNormalisableRange();
        const float v = jlimit (range.start, range.end, param->convertFrom0to1 (param->getValue()) + (float) delta);
        att->setValueAsCompleteGesture (v);
    }

    void DragReadout::mouseDown (const MouseEvent&)
    {
        dragged = false;
        if (param != nullptr) { startValue = param->convertFrom0to1 (param->getValue()); att->beginGesture(); }
        else if (model.get) modelStart = model.get();
    }

    void DragReadout::mouseDrag (const MouseEvent& e)
    {
        const int steps = -e.getDistanceFromDragStartY() / 8;
        if (param == nullptr && model.get)
        {
            if (steps != 0) { dragged = true; model.set (jlimit (model.min, model.max, modelStart + steps)); repaint(); }
            return;
        }
        if (param == nullptr) return;
        if (steps != 0) dragged = true;
        const auto& range = param->getNormalisableRange();
        att->setValueAsPartOfGesture (jlimit (range.start, range.end, startValue + (float) steps));
    }

    void DragReadout::mouseUp (const MouseEvent& e)
    {
        if (param == nullptr && model.get)
        {
            if (! dragged && e.mouseWasClicked() && model.cycleOnClick)
                model.set (model.get() + 1 > model.max ? model.min : model.get() + 1);
            repaint();
            return;
        }
        if (param == nullptr) return;
        att->endGesture();
        // A click (no drag) on a choice cycles it.
        if (! dragged && e.mouseWasClicked() && dynamic_cast<AudioParameterChoice*> (param) != nullptr)
        {
            const auto& range = param->getNormalisableRange();
            const float v = param->convertFrom0to1 (param->getValue()) + 1.0f;
            att->setValueAsCompleteGesture (v > range.end ? range.start : v);
        }
    }

    void DragReadout::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
    {
        stepBy (w.deltaY > 0.0f ? 1 : (w.deltaY < 0.0f ? -1 : 0));
    }

    void DragReadout::mouseDoubleClick (const MouseEvent&)
    {
        if (param != nullptr) att->setValueAsCompleteGesture (param->convertFrom0to1 (param->getDefaultValue()));
        else if (model.set) { model.set (model.def); repaint(); }
    }

    void DragReadout::paint (Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (c::bakelite.withAlpha (0.8f));
        g.setFont (industrialFont (10.5f));
        g.drawText (caption, b.removeFromTop (12.0f), Justification::centred, false);
        paint::recess (g, b, 3.0f);
        g.setColour (c::vfd);
        g.setFont (monoFont (14.0f));
        const String text = param != nullptr ? param->getCurrentValueAsText()
                          : (model.get ? (model.text ? model.text (model.get()) : String (model.get())) : String ("--"));
        g.drawText (text, b, Justification::centred, false);
    }

    //==========================================================================
    //  SequencerPanel
    //==========================================================================
    namespace
    {
        constexpr float kHeaderH    = 44.0f;
        constexpr float kLabelW     = 30.0f;
        constexpr float kInspectorH = 96.0f;
    }

    SequencerPanel::SequencerPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        auto& apvts = processor.getParameters();
        lenRead.attach (apvts.getParameter (params::seqLength));
        stepRead.attach (apvts.getParameter (params::seqRes));
        quantRead.attach (apvts.getParameter (params::seqQuant));

        armKey.onClick = [this]
        {
            auto& seq = processor.getBridge().seq;
            seq.setArmed (selectedPad, ! seq.isArmed (selectedPad));
            refresh();
        };
        clearKey.onClick = [this]
        {
            processor.getBridge().seq.clearLane (selectedPad);
            processor.markStateChanged();
        };

        revKey.onClick = [this] { auto st = currentStep(); st.rev = ! st.rev; writeStep (st); };
        hitKey.onClick = [this] { auto st = currentStep(); st.hit = ! (st.on && st.hit); writeStep (st); };

        // FILL (HOLD): the parameter, so it can be MIDI-mapped / automated in Live too.
        fillKey.onStateChange = [this]
        {
            auto* prm = processor.getParameters().getParameter (params::seqFill);
            const bool down = fillKey.isDown();
            if (prm == nullptr || (prm->getValue() > 0.5f) == down) return;
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (down ? 1.0f : 0.0f);
            prm->endChangeGesture();
        };

        for (Component* comp : std::initializer_list<Component*> { &lenRead, &stepRead, &quantRead, &armKey, &clearKey, &wholeKey,
                                                                    &velRead, &chanceRead, &ratRead, &pitchRead, &sliceRead, &microRead,
                                                                    &fxRead, &condRead, &revKey, &hitKey, &fillKey })
            addAndMakeVisible (comp);
        bindStepReadouts();
        bindMutateControls();
        showPage (Page::step);
        setSelectedPad (0);
    }

    void SequencerPanel::bindMutateControls()
    {
        auto& apvts = processor.getParameters();
        for (int t = 0; t < params::kNumMutTargets; ++t)
        {
            targetKeys[(size_t) t] = std::make_unique<PushKey> (params::mutTargetKeyText[t],
                                                                 String ("MUTATE may change: ") + params::mutTargetNames[t], Colour (0xffffa928));
            targetKeys[(size_t) t]->setClickingTogglesState (true);
            targetAtts[(size_t) t] = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::mutTargetId (t), *targetKeys[(size_t) t]);
            addChildComponent (*targetKeys[(size_t) t]);
        }
        driftKey.setClickingTogglesState (true);
        driftAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::mutDrift, driftKey);

        // Readouts drive their parameters (short VFD texts instead of the host's names).
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
        amountRead.bind (paramModel (params::mutAmount, 0, 20, 7, 5, [] (int v) { return String (v * 5) + "%"; }));
        auto m = paramModel (params::mutScope, 0, 1, 1, 1, [] (int v) { return v == 0 ? String ("PAD") : String ("ALL"); });
        m.cycleOnClick = true; scopeRead.bind (m);
        m = paramModel (params::mutAuto, 0, 4, 0, 1, [] (int v) { return v == 0 ? String ("OFF") : String (params::mutAutoBars[jlimit (0, 4, v)]) + " BAR"; });
        m.cycleOnClick = true; autoRead.bind (m);
        m = paramModel (params::mutAt, 0, 2, 2, 1, [] (int v) { return v == 0 ? String ("NOW") : (v == 1 ? String ("BEAT") : String ("BAR")); });
        m.cycleOnClick = true; atRead.bind (m);

        goKey.onClick    = [this] { processor.getMutator().mutate(); refresh(); };
        undoKey.onClick  = [this] { processor.getMutator().undo();   refresh(); };
        resetKey.onClick = [this] { processor.getMutator().reset();  refresh(); };

        stepTab.setRadioGroupId (7101);
        mutateTab.setRadioGroupId (7101);
        stepTab.onClick   = [this] { showPage (Page::step); };
        mutateTab.onClick = [this] { showPage (Page::mutate); };

        for (Component* comp : std::initializer_list<Component*> { &stepTab, &mutateTab })
            addAndMakeVisible (comp);
        for (Component* comp : std::initializer_list<Component*> { &amountRead, &scopeRead, &autoRead, &atRead, &driftKey, &goKey, &undoKey, &resetKey })
            addChildComponent (comp);
    }

    void SequencerPanel::showPage (Page p)
    {
        page = p;
        const bool step = p == Page::step;
        stepTab.setToggleState (step, dontSendNotification);
        mutateTab.setToggleState (! step, dontSendNotification);
        for (Component* comp : std::initializer_list<Component*> { &velRead, &chanceRead, &ratRead, &pitchRead, &sliceRead, &microRead,
                                                                    &fxRead, &condRead, &revKey, &hitKey, &fillKey })
            comp->setVisible (step);
        for (Component* comp : std::initializer_list<Component*> { &amountRead, &scopeRead, &autoRead, &atRead, &driftKey, &goKey, &undoKey, &resetKey })
            comp->setVisible (! step);
        for (auto& k : targetKeys) k->setVisible (! step);
        repaint (inspectorArea().toNearestInt().expanded (2));
    }

    SequencerPanel::~SequencerPanel()
    {
        wholeAtt.reset();
        driftAtt.reset();
        for (auto& a : targetAtts) a.reset();
        lenRead.attach (nullptr); stepRead.attach (nullptr); quantRead.attach (nullptr);
    }

    void SequencerPanel::setSelectedPad (int pad)
    {
        selectedPad = jlimit (0, SequencerData::kPads - 1, pad);
        armKey.setButtonText ("ARM PAD " + padLabel (selectedPad));
        // A10: WHOLE ONLY for the selected pad's lane.
        wholeAtt.reset();
        wholeKey.setClickingTogglesState (true);
        wholeAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (processor.getParameters(),
                                                                                      params::padId (selectedPad, params::padStepsWhole), wholeKey);
        refresh();
        repaint();
    }

    void SequencerPanel::selectStep (int pad, int step)
    {
        selectedStep = jlimit (0, SequencerData::kSteps - 1, step);
        if (pad != selectedPad)
        {
            setSelectedPad (pad);
            if (onSelectPad) onSelectPad (selectedPad);
        }
        shownVersion = 0xffffffff;
        refresh();
        repaint();
    }

    DragReadout& SequencerPanel::readout (const String& caption)
    {
        for (auto* r : { &velRead, &chanceRead, &ratRead, &pitchRead, &sliceRead, &microRead, &fxRead, &condRead })
            if (r->getName() == caption) return *r;
        jassertfalse;
        return velRead;
    }

    SeqStep SequencerPanel::currentStep() const
    {
        return processor.getBridge().seq.get (selectedPad, selectedStep);
    }

    void SequencerPanel::writeStep (SeqStep st)
    {
        st.on = true;                         // editing an empty step switches it on
        processor.getBridge().seq.set (selectedPad, selectedStep, st);
        processor.markStateChanged();
        refresh();
        repaint();
    }

    void SequencerPanel::bindStepReadouts()
    {
        auto field = [this] (std::function<int (const SeqStep&)> get, std::function<void (SeqStep&, int)> set)
        {
            DragReadout::Model m;
            m.get = [this, get] { return get (currentStep()); };
            m.set = [this, set] (int v) { auto st = currentStep(); if (! st.on) st = SeqStep {}; set (st, v); writeStep (st); };
            return m;
        };
        auto sign = [] (int v) { return (v > 0 ? "+" : "") + String (v); };

        auto m = field ([] (const SeqStep& s) { return s.velocity; }, [] (SeqStep& s, int v) { s.velocity = v; });
        m.min = 1; m.max = 127; m.def = 100;
        velRead.bind (m);

        m = field ([] (const SeqStep& s) { return roundToInt (s.probability / 5.0f); }, [] (SeqStep& s, int v) { s.probability = v * 5; });
        m.min = 0; m.max = 20; m.def = 20; m.text = [] (int v) { return String (v * 5) + "%"; };
        chanceRead.bind (m);

        m = field ([] (const SeqStep& s) { return s.ratchet; }, [] (SeqStep& s, int v) { s.ratchet = v; });
        m.min = 1; m.max = 4; m.def = 1; m.cycleOnClick = true; m.text = [] (int v) { return "x" + String (v); };
        ratRead.bind (m);

        m = field ([] (const SeqStep& s) { return s.pitch; }, [] (SeqStep& s, int v) { s.pitch = v; });
        m.min = -24; m.max = 24; m.def = 0; m.cycleOnClick = false; m.text = sign;
        pitchRead.bind (m);

        m = field ([] (const SeqStep& s) { return s.slice; }, [] (SeqStep& s, int v) { s.slice = v; });
        m.min = 0; m.max = 16; m.def = 0; m.text = [] (int v) { return v == 0 ? String ("PAD") : String (v); };
        sliceRead.bind (m);

        m = field ([] (const SeqStep& s) { return s.micro; }, [] (SeqStep& s, int v) { s.micro = v; });
        m.min = -64; m.max = 63; m.def = 0; m.text = sign;
        microRead.bind (m);

        m = field ([] (const SeqStep& s) { return s.fx; }, [] (SeqStep& s, int v) { s.fx = v; });
        m.min = 0; m.max = fx::kNumFx; m.def = 0; m.text = [] (int v) { return v == 0 ? String ("-") : String (fx::def (v - 1).label); };
        fxRead.bind (m);

        m = field ([] (const SeqStep& s) { return s.cond; }, [] (SeqStep& s, int v) { s.cond = v; });
        m.min = 0; m.max = kNumSeqConds - 1; m.def = 0; m.text = [] (int v) { return String (kSeqCondNames[jlimit (0, kNumSeqConds - 1, v)]); };
        condRead.bind (m);

        for (auto* r : { &velRead, &chanceRead, &ratRead, &pitchRead, &sliceRead, &microRead, &fxRead, &condRead })
            r->setName (r == &velRead ? "VEL" : r == &chanceRead ? "CHANCE" : r == &ratRead ? "RAT" : r == &pitchRead ? "PITCH"
                      : r == &sliceRead ? "SLICE" : r == &microRead ? "MICRO" : r == &fxRead ? "FX" : "COND");
    }

    int SequencerPanel::lengthNow() const
    {
        return jlimit (1, 32, roundToInt (processor.getParameters().getRawParameterValue (params::seqLength)->load()));
    }

    Rectangle<float> SequencerPanel::gridArea() const
    {
        return getLocalBounds().toFloat().reduced (14.0f, 0.0f).withTrimmedTop (kHeaderH + 4.0f).withTrimmedBottom (kInspectorH + 22.0f);
    }

    Rectangle<float> SequencerPanel::inspectorArea() const
    {
        return getLocalBounds().toFloat().reduced (14.0f, 0.0f).withTrimmedBottom (12.0f).removeFromBottom (kInspectorH);
    }

    Rectangle<float> SequencerPanel::laneLabelBounds (int pad) const
    {
        const auto a = gridArea();
        const float rowH = a.getHeight() / (float) kPadsPerScene;
        return { a.getX(), a.getY() + (float) (pad % kPadsPerScene) * rowH, kLabelW - 4.0f, rowH };
    }

    Rectangle<float> SequencerPanel::cellBounds (int pad, int step) const
    {
        const auto a = gridArea().withTrimmedLeft (kLabelW);
        const float rowH = a.getHeight() / (float) kPadsPerScene;
        const float colW = a.getWidth() / (float) SequencerData::kSteps;
        return Rectangle<float> (a.getX() + (float) step * colW, a.getY() + (float) (pad % kPadsPerScene) * rowH, colW, rowH).reduced (1.0f);
    }

    bool SequencerPanel::cellAt (Point<float> pos, int& pad, int& step) const
    {
        const auto a = gridArea().withTrimmedLeft (kLabelW);
        if (! a.contains (pos)) return false;
        pad  = sceneOf (selectedPad) * kPadsPerScene
             + jlimit (0, kPadsPerScene - 1, (int) ((pos.y - a.getY()) / (a.getHeight() / (float) kPadsPerScene)));
        step = jlimit (0, SequencerData::kSteps - 1, (int) ((pos.x - a.getX()) / (a.getWidth()  / (float) SequencerData::kSteps)));
        return true;
    }

    void SequencerPanel::resized()
    {
        int x = 150;
        const int y = 10, h = 30;
        lenRead.setBounds  (x, y - 6, 52, h + 6); x += 56;
        stepRead.setBounds (x, y - 6, 52, h + 6); x += 56;
        quantRead.setBounds (x, y - 6, 56, h + 6); x += 62;
        armKey.setBounds   (x, y, 94, h);      x += 98;
        clearKey.setBounds (x, y, 64, h);  x += 68;
        wholeKey.setBounds (x, y, jmin (72, getWidth() - x - 14), h);

        // Bottom area: [STEP / MUTATE tabs] then the page's two rows.
        const auto ins = inspectorArea().toNearestInt().reduced (8, 6);
        auto tabs = ins.withWidth (80);
        stepTab.setBounds (tabs.removeFromTop (40).withTrimmedTop (8));
        mutateTab.setBounds (tabs.withTrimmedTop (14).withHeight (32));
        const auto body = ins.withTrimmedLeft (86);
        auto row1 = body.withHeight (40);
        auto row2 = body.withTrimmedTop (46).withHeight (40);
        auto key = [] (Rectangle<int>& row, int w) { return row.removeFromLeft (w).withTrimmedTop (10); };

        // STEP page: VEL CHANCE RAT PITCH SLICE MICRO / FX COND [REV] [HIT] [FILL] position
        {
            auto r1 = row1, r2 = row2;
            for (auto* r : { &velRead, &chanceRead, &ratRead, &pitchRead, &sliceRead, &microRead })
            {
                r->setBounds (r1.removeFromLeft (60));
                r1.removeFromLeft (8);
            }
            fxRead.setBounds   (r2.removeFromLeft (80));  r2.removeFromLeft (6);
            condRead.setBounds (r2.removeFromLeft (60));  r2.removeFromLeft (8);
            revKey.setBounds   (key (r2, 46));            r2.removeFromLeft (4);
            hitKey.setBounds   (key (r2, 46));            r2.removeFromLeft (8);
            fillKey.setBounds  (key (r2, 92));
        }
        // MUTATE page: AMOUNT + targets / SCOPE AUTO AT [DRIFT] [MUTATE] [UNDO] [RESET]
        {
            auto r1 = row1, r2 = row2;
            amountRead.setBounds (r1.removeFromLeft (52)); r1.removeFromLeft (6);
            constexpr int widths[] = { 61, 61, 52, 44, 44, 36, 53 };     // fit SLICE PITCH PATT RAT REV FX CHNC
            for (size_t t = 0; t < targetKeys.size(); ++t) { targetKeys[t]->setBounds (key (r1, widths[t])); r1.removeFromLeft (4); }
            scopeRead.setBounds (r2.removeFromLeft (50)); r2.removeFromLeft (6);
            autoRead.setBounds  (r2.removeFromLeft (50)); r2.removeFromLeft (6);
            atRead.setBounds    (r2.removeFromLeft (54)); r2.removeFromLeft (8);
            driftKey.setBounds  (key (r2, 52));           r2.removeFromLeft (6);
            goKey.setBounds     (key (r2, 84));           r2.removeFromLeft (6);
            undoKey.setBounds   (key (r2, 52));           r2.removeFromLeft (6);
            resetKey.setBounds  (key (r2, 54));
        }
        background = {};
    }

    //==========================================================================
    void SequencerPanel::mouseDown (const MouseEvent& e)
    {
        for (int p = sceneOf (selectedPad) * kPadsPerScene; p < (sceneOf (selectedPad) + 1) * kPadsPerScene; ++p)
            if (laneLabelBounds (p).contains (e.position))
            {
                setSelectedPad (p);
                if (onSelectPad) onSelectPad (p);
                return;
            }

        int pad, step;
        if (! cellAt (e.position, pad, step)) return;
        selectStep (pad, step);

        // Right-click / shift-click: just select it for the STEP row.
        editSelectOnly = e.mods.isPopupMenu() || e.mods.isShiftDown();
        if (editSelectOnly) { editPad = -1; return; }

        auto& seq = processor.getBridge().seq;
        auto st = seq.get (pad, step);
        editPad = pad; editStep = step; editDragged = false;
        editWasOn = st.on;
        if (! st.on)
        {
            st = {};
            st.on = true;
            seq.set (pad, step, st);
        }
        editStartVel = st.velocity;
        repaint();
    }

    void SequencerPanel::mouseDrag (const MouseEvent& e)
    {
        if (editPad < 0) return;
        const int dy = -e.getDistanceFromDragStartY();
        if (std::abs (dy) < 3 && ! editDragged) return;
        editDragged = true;
        auto& seq = processor.getBridge().seq;
        auto st = seq.get (editPad, editStep);
        st.on = true;
        st.velocity = jlimit (1, 127, editStartVel + dy);
        seq.set (editPad, editStep, st);
        repaint();
    }

    void SequencerPanel::mouseUp (const MouseEvent&)
    {
        if (editPad < 0) return;
        if (editWasOn && ! editDragged)                      // a plain click on a lit step: off
            processor.getBridge().seq.set (editPad, editStep, {});
        editPad = editStep = -1;
        processor.markStateChanged();
        repaint();
    }

    void SequencerPanel::refresh()
    {
        auto& seq = processor.getBridge().seq;
        armKey.setToggleState (seq.isArmed (selectedPad), dontSendNotification);
        armKey.setBlinking (seq.isArmed (selectedPad) && processor.getBridge().seqStep.load() >= 0);
        armKey.tick();

        const auto st = currentStep();
        revKey.setToggleState (st.on && st.rev, dontSendNotification);
        hitKey.setToggleState (st.on && st.hit, dontSendNotification);
        const bool fill = processor.getParameters().getRawParameterValue (params::seqFill)->load() > 0.5f;
        fillKey.setToggleState (fill, dontSendNotification);

        const auto& tr = processor.getBridge().transport;
        const int bar = tr.bar.load (std::memory_order_relaxed), beat = tr.beat.load (std::memory_order_relaxed);

        // Tabs + MUTATE page state.
        const String tabText = "STEP " + padLabel (selectedPad) + "." + String (selectedStep + 1).paddedLeft ('0', 2);
        if (stepTab.getButtonText() != tabText) stepTab.setButtonText (tabText);
        auto& mut = processor.getMutator();
        goKey.setBlinking (mut.isWaiting());
        goKey.tick();
        undoKey.setEnabled (mut.canUndo());
        resetKey.setEnabled (mut.hasOriginal());
        for (auto* r : { &amountRead, &scopeRead, &autoRead, &atRead }) r->repaint();

        const auto v = seq.getVersion();
        const int step = processor.getBridge().seqStep.load();
        const int len = lengthNow();
        uint32_t laneHash = 0;
        for (int p = 0; p < kNumPads; ++p) laneHash = laneHash * 31u + (uint32_t) (processor.getBridge().laneStep[(size_t) p].load() + 2);
        if (laneHash != shownLaneHash) { shownLaneHash = laneHash; shownStep = -3; }
        if (v != shownVersion || step != shownStep || len != shownLen)
        {
            if (v != shownVersion)
                for (auto* r : { &velRead, &chanceRead, &ratRead, &pitchRead, &sliceRead, &microRead, &fxRead, &condRead })
                    r->repaint();
            shownVersion = v; shownStep = step; shownLen = len;
            repaint (gridArea().toNearestInt().expanded (2));
            repaint (inspectorArea().toNearestInt().withWidth (130));
        }
        if (bar != shownBar || beat != shownBeat || fill != shownFill)
        {
            shownBar = bar; shownBeat = beat; shownFill = fill;
            repaint (inspectorArea().toNearestInt().withTrimmedLeft (inspectorArea().toNearestInt().getWidth() - 130));
        }
    }

    //==========================================================================
    void SequencerPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random rnd (7707);
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
            paint::labelPlate (bg, { 18.0f, 12.0f, 124.0f, 22.0f }, "SEQUENCER", {}, paint::PlateStyle::bakelite, rnd);
            paint::recess (bg, gridArea().expanded (4.0f, 3.0f), 4.0f);
            paint::recess (bg, inspectorArea(), 4.0f);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        auto& seq = processor.getBridge().seq;
        const int len  = lengthNow();
        const int head = processor.getBridge().seqStep.load();

        // Playhead column.
        if (head >= 0)
        {
            const auto top = cellBounds (0, head), bottom = cellBounds (kPadsPerScene - 1, head);
            g.setColour (c::phosphor.withAlpha (0.16f));
            g.fillRect (top.getUnion (bottom).expanded (1.0f));
        }

        for (int p = sceneOf (selectedPad) * kPadsPerScene; p < (sceneOf (selectedPad) + 1) * kPadsPerScene; ++p)
        {
            const auto lb = laneLabelBounds (p);
            if (p == selectedPad)
            {
                g.setColour (c::vfd.withAlpha (0.14f));
                g.fillRect (lb.getUnion (cellBounds (p, SequencerData::kSteps - 1)));
            }
            g.setColour (p == selectedPad ? c::vfd : c::bakelite.withAlpha (0.7f));
            g.setFont (monoFont (jmin (11.0f, lb.getHeight())));
            g.drawText (padLabel (p), lb.withTrimmedLeft (9.0f), Justification::centredLeft, false);
            if (seq.isArmed (p))
                paint::lamp (g, { lb.getX() + 4.0f, lb.getCentreY() }, 2.8f, c::red, c::redDim, 1.0f);

            // SEQ pads (A6) play only when launched: their own playhead, dimmed while idle.
            const bool seqPad = params::isSeqMode (roundToInt (processor.getParameters().getRawParameterValue (params::padId (p, params::padMode))->load()));
            const int laneStep = processor.getBridge().laneStep[(size_t) p].load (std::memory_order_relaxed);
            if (seqPad && laneStep < 0)
                paint::lamp (g, { lb.getRight() - 2.0f, lb.getCentreY() }, 2.4f, c::amber, c::amberDim, 0.15f);
            else if (seqPad)
                paint::lamp (g, { lb.getRight() - 2.0f, lb.getCentreY() }, 2.4f, c::amber, c::amberDim, 1.0f);

            for (int s = 0; s < SequencerData::kSteps; ++s)
            {
                const auto cell = cellBounds (p, s);
                const bool inLen = s < len;
                const auto st = seq.get (p, s);
                g.setColour ((s / 4) % 2 == 0 ? Colour (0xff17191a) : Colour (0xff222526));
                g.fillRect (cell);
                if (st.on)
                {
                    const float a = inLen ? 1.0f : 0.3f;
                    const float vel = (float) st.velocity / 127.0f;
                    if (st.hit)
                    {
                        g.setColour (c::amber.withAlpha (0.35f * a));
                        g.fillRect (cell);
                        g.setColour (c::amber.withAlpha (a));
                        g.fillRect (cell.withTop (cell.getBottom() - cell.getHeight() * (0.25f + 0.75f * vel)));
                    }
                    else
                    {
                        g.setColour (c::amber.withAlpha (0.8f * a));        // FX only: no hit, just the outline
                        g.drawRect (cell.reduced (0.5f), 1.0f);
                    }
                    if (st.micro != 0)       // played off the grid: a tick shows where
                    {
                        g.setColour (Colours::black.withAlpha (0.7f));
                        const float x = cell.getCentreX() + cell.getWidth() * (float) st.micro / 128.0f;
                        g.drawVerticalLine (roundToInt (x), cell.getY(), cell.getBottom());
                    }
                    if (st.ratchet > 1)      // ratchet: dividers along the top
                    {
                        g.setColour (Colours::black.withAlpha (0.85f));
                        for (int j = 1; j < st.ratchet; ++j)
                        {
                            const float x = cell.getX() + cell.getWidth() * (float) j / (float) st.ratchet;
                            g.fillRect (x - 0.5f, cell.getY(), 1.2f, cell.getHeight() * 0.45f);
                        }
                    }
                    if (st.probability < 100)    // chance: a hollow dot top-right
                    {
                        g.setColour (Colours::black.withAlpha (0.85f));
                        g.fillEllipse (cell.getRight() - 5.0f, cell.getY() + 1.0f, 4.0f, 4.0f);
                        g.setColour (c::vfd.withAlpha (a));
                        g.fillEllipse (cell.getRight() - 4.0f, cell.getY() + 2.0f, 2.0f * (float) st.probability / 100.0f + 0.4f, 2.0f);
                    }
                    if (st.hasSoundTweak())     // pitch / reverse / slice: green corner
                    {
                        Path tri;
                        tri.addTriangle (cell.getRight(), cell.getBottom(), cell.getRight() - 5.0f, cell.getBottom(), cell.getRight(), cell.getBottom() - 5.0f);
                        g.setColour (c::phosphor.withAlpha (a));
                        g.fillPath (tri);
                    }
                    if (st.fx > 0)              // FX: red corner
                    {
                        Path tri;
                        tri.addTriangle (cell.getX(), cell.getY(), cell.getX() + 5.0f, cell.getY(), cell.getX(), cell.getY() + 5.0f);
                        g.setColour (c::red.withAlpha (a));
                        g.fillPath (tri);
                    }
                    if (st.cond != condAlways)  // condition: cyan underline
                    {
                        g.setColour (Colour (0xff4fd8ff).withAlpha (a));
                        g.fillRect (cell.getX() + 1.0f, cell.getBottom() - 1.6f, cell.getWidth() - 2.0f, 1.6f);
                    }
                }
                if (! inLen)
                {
                    g.setColour (Colours::black.withAlpha (0.45f));
                    g.fillRect (cell);
                }
                if (seqPad)
                {
                    const int ls = processor.getBridge().laneStep[(size_t) p].load (std::memory_order_relaxed);
                    if (ls == s)
                    {
                        g.setColour (c::phosphor.withAlpha (0.45f));
                        g.drawRect (cell, 1.5f);
                    }
                    else if (ls < 0)
                    {
                        g.setColour (Colours::black.withAlpha (0.3f));
                        g.fillRect (cell);
                    }
                }
                if (p == selectedPad && s == selectedStep)
                {
                    g.setColour (c::vfd);
                    g.drawRect (cell.expanded (1.0f), 1.5f);
                }
            }
        }

        // STEP page: Live's position (the transport box is covered here).
        if (page == Page::step)
        {
            const auto& tr = processor.getBridge().transport;
            auto pos = inspectorArea().reduced (8.0f, 6.0f).withTrimmedTop (46.0f).withHeight (40.0f);
            pos = pos.removeFromRight (78.0f);
            g.setColour (c::bakelite.withAlpha (0.8f));
            g.setFont (industrialFont (10.5f));
            g.drawText (String (tr.bpm.load (std::memory_order_relaxed), 1) + " BPM", pos.removeFromTop (12.0f), Justification::centred, false);
            paint::recess (g, pos, 3.0f);
            g.setColour (c::vfd);
            g.setFont (monoFont (13.0f));
            g.drawText (String (tr.bar.load (std::memory_order_relaxed) + 1).paddedLeft ('0', 3) + "." + String (tr.beat.load (std::memory_order_relaxed) + 1),
                        pos, Justification::centred, false);
        }
    }
}
