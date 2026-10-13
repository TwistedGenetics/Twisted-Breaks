#include "UI/SliceSeqPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr float kStripTop = 40.0f, kCellW = 34.0f, kCellH = 36.0f, kGap = 4.0f;
    }

    SliceSeqPanel::SliceSeqPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        for (Component* comp : std::initializer_list<Component*> { &sliceRead, &pitchRead, &velRead, &lenRead, &ratRead, &chanceRead, &microRead,
                                                                    &revKey, &fillKey, &shuffleKey, &randomKey, &reverseKey, &halfKey,
                                                                    &rotLKey, &rotRKey, &resetKey, &stepRecKey })
            addAndMakeVisible (comp);

        revKey.onClick = [this] { auto s = currentStep (selected); s.rev = ! s.rev; writeStep (selected, s); };
        fillKey.onStateChange = [this] { processor.getBridge().sliceFill[(size_t) pad].store (fillKey.isDown()); };
        stepRecKey.setClickingTogglesState (true);

        using Op = TwistedBreaksProcessor::SliceOp;
        auto op = [this] (Op o) { processor.sliceSeqOp (pad, o, rnd); repaint(); };
        shuffleKey.onClick = [op] { op (Op::shuffle); };
        randomKey.onClick  = [op] { op (Op::random); };
        reverseKey.onClick = [op] { op (Op::reverseOrder); };
        halfKey.onClick    = [op] { op (Op::half); };
        rotLKey.onClick    = [op] { op (Op::rotateLeft); };
        rotRKey.onClick    = [op] { op (Op::rotateRight); };
        resetKey.onClick   = [op] { op (Op::reset); };

        bindStepReadouts();
    }

    int SliceSeqPanel::slot() const   { return processor.sliceSlotOf (pad); }
    int SliceSeqPanel::sliceCount() const
    {
        const auto info = processor.getSamples().getInfo (pad);
        return info.slices.isEmpty() ? 16 : info.slices.count;
    }
    int SliceSeqPanel::length() const
    {
        const auto& data = processor.getBridge().sliceSeq;
        const auto s = data.getSlot (pad, slot());
        if (s.written) return s.length;
        // Unwritten: the default pattern's length.
        const auto info = processor.getSamples().getInfo (pad);
        if (info.buffer == nullptr) return s.length;
        std::array<SliceStep, SliceSeqData::kSteps> pat;
        const double beats = PadVoice::loopBeatsFor ((double) info.buffer->getNumSamples() / info.buffer->sourceSampleRate);
        return SliceSeqData::defaultPattern (info.slices, info.buffer->getNumSamples(), info.buffer->sourceSampleRate, kSliceRateBeats[s.rate], beats, pat);
    }

    SliceStep SliceSeqPanel::currentStep (int k) const
    {
        const auto& data = processor.getBridge().sliceSeq;
        const int sl = slot();
        if (data.getSlot (pad, sl).written) return data.getStep (pad, sl, k);
        const auto info = processor.getSamples().getInfo (pad);
        if (info.buffer == nullptr) return {};
        std::array<SliceStep, SliceSeqData::kSteps> pat;
        const double beats = PadVoice::loopBeatsFor ((double) info.buffer->getNumSamples() / info.buffer->sourceSampleRate);
        SliceSeqData::defaultPattern (info.slices, info.buffer->getNumSamples(), info.buffer->sourceSampleRate,
                                      kSliceRateBeats[data.getSlot (pad, sl).rate], beats, pat);
        return pat[(size_t) jlimit (0, SliceSeqData::kSteps - 1, k)];
    }

    void SliceSeqPanel::writeStep (int k, const SliceStep& s)
    {
        processor.materializeSliceSlot (pad, slot());
        processor.getBridge().sliceSeq.setStep (pad, slot(), jlimit (0, SliceSeqData::kSteps - 1, k), s);
        processor.markStateChanged();
        repaint();
    }

    int SliceSeqPanel::encodeSlice (const SliceStep& s)
    {
        return s.kind == SliceStep::tie ? -2 : (s.kind == SliceStep::rest ? -1 : s.slice);
    }
    void SliceSeqPanel::decodeSlice (SliceStep& s, int v)
    {
        if (v <= -2)      s.kind = SliceStep::tie;
        else if (v == -1) s.kind = SliceStep::rest;
        else              { s.kind = SliceStep::play; s.slice = v; }
    }

    void SliceSeqPanel::bindStepReadouts()
    {
        auto field = [this] (std::function<int (const SliceStep&)> get, std::function<void (SliceStep&, int)> set)
        {
            DragReadout::Model m;
            m.get = [this, get] { return get (currentStep (selected)); };
            m.set = [this, set] (int v) { auto s = currentStep (selected); set (s, v); writeStep (selected, s); };
            return m;
        };

        auto m = field ([] (const SliceStep& s) { return encodeSlice (s); }, [] (SliceStep& s, int v) { decodeSlice (s, v); });
        m.min = -2; m.max = 15; m.def = -2;
        m.text = [] (int v) { return v == -2 ? String ("TIE") : (v == -1 ? String ("REST") : String (v + 1)); };
        sliceRead.bind (m);

        m = field ([] (const SliceStep& s) { return s.pitch; }, [] (SliceStep& s, int v) { s.pitch = v; });
        m.min = -12; m.max = 12; m.def = 0; m.text = [] (int v) { return (v > 0 ? "+" : "") + String (v); };
        pitchRead.bind (m);

        m = field ([] (const SliceStep& s) { return s.velocity; }, [] (SliceStep& s, int v) { s.velocity = v; });
        m.min = 1; m.max = 127; m.def = 127; m.text = {};
        velRead.bind (m);

        m = field ([] (const SliceStep& s) { return s.length; }, [] (SliceStep& s, int v) { s.length = v; });
        m.min = 0; m.max = 3; m.def = 3; m.cycleOnClick = true; m.text = [] (int v) { return String (kSliceLengthNames[jlimit (0, 3, v)]); };
        lenRead.bind (m);

        m = field ([] (const SliceStep& s) { return s.ratchet; }, [] (SliceStep& s, int v) { s.ratchet = v; });
        m.min = 1; m.max = 4; m.def = 1; m.cycleOnClick = true; m.text = [] (int v) { return "x" + String (v); };
        ratRead.bind (m);

        m = field ([] (const SliceStep& s) { return s.chance; }, [] (SliceStep& s, int v) { s.chance = v; });
        m.min = 0; m.max = 100; m.def = 100; m.cycleOnClick = false; m.text = [] (int v) { return String (v) + "%"; };
        chanceRead.bind (m);

        m = field ([] (const SliceStep& s) { return s.micro; }, [] (SliceStep& s, int v) { s.micro = v; });
        m.min = -32; m.max = 31; m.def = 0; m.text = [] (int v) { return (v > 0 ? "+" : "") + String (v); };
        microRead.bind (m);
    }

    void SliceSeqPanel::setPad (int p)
    {
        if (p == pad) return;
        processor.getBridge().sliceFill[(size_t) pad].store (false);
        pad = jlimit (0, kNumPads - 1, p);
        selected = 0;
        stepRecKey.setToggleState (false, dontSendNotification);
        repaint();
    }

    void SliceSeqPanel::selectStep (int k)
    {
        selected = jlimit (0, SliceSeqData::kSteps - 1, k);
        repaint();
    }

    Rectangle<float> SliceSeqPanel::cellBounds (int k) const
    {
        return { 14.0f + (float) (k % 16) * kCellW, kStripTop + (float) (k / 16) * (kCellH + kGap), kCellW - 3.0f, kCellH };
    }

    void SliceSeqPanel::resized()
    {
        const int y = (int) (kStripTop + 2 * (kCellH + kGap)) + 2;
        int x = 14;
        for (auto* r : { &sliceRead, &pitchRead, &velRead, &lenRead, &ratRead, &chanceRead, &microRead })
        {
            r->setBounds (x, y, 50, 36);
            x += 54;
        }
        revKey.setBounds (x, y + 10, 44, 26);

        fillKey.setBounds (14, y + 44, 150, 30);

        const int kx = 300, kw = 64, kh = 24;
        PushKey* keys[] = { &shuffleKey, &randomKey, &reverseKey, &halfKey, &rotLKey, &rotRKey, &resetKey, &stepRecKey };
        for (int i = 0; i < 8; ++i)
            keys[i]->setBounds (kx - 110 + (i % 4) * (kw + 2) + 72, y + 44 + (i / 4) * (kh + 3), kw, kh);
        background = {};
    }

    //==========================================================================
    void SliceSeqPanel::mouseDown (const MouseEvent& e)
    {
        dragStep = -1; dragged = false;
        for (int k = 0; k < SliceSeqData::kSteps; ++k)
            if (cellBounds (k).contains (e.position))
            {
                selectStep (k);
                dragStep = k;
                dragStart = encodeSlice (currentStep (k));
                return;
            }
    }

    void SliceSeqPanel::mouseDrag (const MouseEvent& e)
    {
        if (dragStep < 0) return;
        const int delta = -e.getDistanceFromDragStartY() / 8;
        if (delta == 0 && ! dragged) return;
        dragged = true;
        auto s = currentStep (dragStep);
        decodeSlice (s, jlimit (-2, sliceCount() - 1, dragStart + delta));
        writeStep (dragStep, s);
    }

    void SliceSeqPanel::mouseUp (const MouseEvent&) { dragStep = -1; }

    void SliceSeqPanel::mouseDoubleClick (const MouseEvent& e)
    {
        for (int k = 0; k < SliceSeqData::kSteps; ++k)
            if (cellBounds (k).contains (e.position))
            {
                auto s = currentStep (k);
                s.kind = SliceStep::tie;
                writeStep (k, s);
            }
    }

    void SliceSeqPanel::refresh()
    {
        auto& br = processor.getBridge();

        // STEP REC: a tapped SPREAD pad (SLICE mode, same sample) writes its slice.
        const auto mine = processor.getSamples().getInfo (pad).buffer;
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto n = br.padTriggerCount[(size_t) p].load();
            if (n != lastTriggers[(size_t) p] && stepRecKey.getToggleState() && p != pad && mine != nullptr)
            {
                auto& apvts = processor.getParameters();
                const bool sliceMode = roundToInt (apvts.getRawParameterValue (params::padId (p, params::padMode))->load()) == params::modeSlice;
                if (sliceMode && processor.getSamples().getInfo (p).buffer == mine)
                {
                    auto s = currentStep (selected);
                    s.kind = SliceStep::play;
                    s.slice = jlimit (0, 15, roundToInt (apvts.getRawParameterValue (params::padId (p, params::padSlice))->load()) - 1);
                    writeStep (selected, s);
                    selectStep ((selected + 1) % juce::jmax (1, length()));
                }
            }
            lastTriggers[(size_t) p] = n;
        }

        stepRecKey.setBlinking (stepRecKey.getToggleState());
        stepRecKey.tick();
        revKey.setToggleState (currentStep (selected).rev, dontSendNotification);

        const auto v = br.sliceSeq.getVersion();
        const int step = br.sliceStep[(size_t) pad].load();
        if (v != shownVersion || step != shownStep)
        {
            shownVersion = v; shownStep = step;
            repaint();
        }
    }

    //==========================================================================
    void SliceSeqPanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random r (8808);
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
            paint::labelPlate (bg, { 18.0f, 10.0f, 124.0f, 22.0f }, "SLICE SEQ", {}, paint::PlateStyle::bakelite, r);
            paint::recess (bg, cellBounds (0).getUnion (cellBounds (31)).expanded (4.0f, 3.0f), 4.0f);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        const auto info = processor.getSamples().getInfo (pad);
        const int sl = slot();
        const bool written = processor.getBridge().sliceSeq.getSlot (pad, sl).written;
        g.setColour (c::vfd);
        g.setFont (monoFont (14.0f));
        g.drawText ("PAD " + String (pad + 1).paddedLeft ('0', 2) + "  SLOT " + String (sl + 1)
                      + (written ? String() : String ("  (AS RECORDED)")) + "  " + info.name.toUpperCase(),
                    Rectangle<float> (152.0f, 10.0f, (float) getWidth() - 166.0f, 22.0f), Justification::centredLeft, true);

        const int len  = length();
        const int head = processor.getBridge().sliceStep[(size_t) pad].load();
        for (int k = 0; k < SliceSeqData::kSteps; ++k)
        {
            const auto cell = cellBounds (k);
            const auto s = currentStep (k);
            const bool in = k < len;
            g.setColour ((k / 4) % 2 == 0 ? Colour (0xff17191a) : Colour (0xff222526));
            g.fillRect (cell);
            if (k == head)
            {
                g.setColour (c::phosphor.withAlpha (0.25f));
                g.fillRect (cell);
            }

            String label;
            Colour col = c::amber;
            if (s.kind == SliceStep::play)      label = String (s.slice + 1) + (s.rev ? "<" : "");
            else if (s.kind == SliceStep::rest) { label = "-"; col = c::red; }
            else                                 { label = "~"; col = c::bakelite.withAlpha (0.5f); }
            g.setColour (col.withMultipliedAlpha (in ? 1.0f : 0.3f));
            g.setFont (industrialFont (15.0f));
            g.drawText (label, cell.withTrimmedBottom (8.0f), Justification::centred, false);

            if (s.kind == SliceStep::play && in)
            {
                for (int r = 0; r < s.ratchet; ++r)        // ratchet dots
                    g.fillEllipse (cell.getX() + 4.0f + (float) r * 5.0f, cell.getBottom() - 7.0f, 3.0f, 3.0f);
                if (s.pitch != 0 || s.chance < 100)
                {
                    g.setFont (monoFont (8.5f));
                    g.drawText (s.chance < 100 ? String (s.chance) + "%" : (s.pitch > 0 ? "+" : "") + String (s.pitch),
                                cell.withTrimmedTop (cell.getHeight() - 10.0f).withTrimmedLeft (12.0f), Justification::centredRight, false);
                }
            }
            if (k == selected)
            {
                g.setColour (c::vfd);
                g.drawRect (cell.expanded (1.0f), 1.5f);
            }
        }
    }
}
