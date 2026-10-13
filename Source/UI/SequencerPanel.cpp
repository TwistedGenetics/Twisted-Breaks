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
        if (param == nullptr || att == nullptr) return;
        const auto& range = param->getNormalisableRange();
        const float v = jlimit (range.start, range.end, param->convertFrom0to1 (param->getValue()) + (float) delta);
        att->setValueAsCompleteGesture (v);
    }

    void DragReadout::mouseDown (const MouseEvent&)
    {
        dragged = false;
        if (param != nullptr) { startValue = param->convertFrom0to1 (param->getValue()); att->beginGesture(); }
    }

    void DragReadout::mouseDrag (const MouseEvent& e)
    {
        if (param == nullptr) return;
        const int steps = -e.getDistanceFromDragStartY() / 8;
        if (steps != 0) dragged = true;
        const auto& range = param->getNormalisableRange();
        att->setValueAsPartOfGesture (jlimit (range.start, range.end, startValue + (float) steps));
    }

    void DragReadout::mouseUp (const MouseEvent& e)
    {
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
        g.drawText (param != nullptr ? param->getCurrentValueAsText() : String ("--"), b, Justification::centred, false);
    }

    //==========================================================================
    //  SequencerPanel
    //==========================================================================
    namespace
    {
        constexpr float kHeaderH = 44.0f;
        constexpr float kLabelW  = 30.0f;
    }

    SequencerPanel::SequencerPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        auto& apvts = processor.getParameters();
        runKey.setClickingTogglesState (true);
        runAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::seqRun, runKey);
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

        for (Component* comp : std::initializer_list<Component*> { &runKey, &lenRead, &stepRead, &quantRead, &armKey, &clearKey })
            addAndMakeVisible (comp);
        setSelectedPad (0);
    }

    SequencerPanel::~SequencerPanel()
    {
        runAtt.reset();
        lenRead.attach (nullptr); stepRead.attach (nullptr); quantRead.attach (nullptr);
    }

    void SequencerPanel::setSelectedPad (int pad)
    {
        selectedPad = jlimit (0, SequencerData::kPads - 1, pad);
        armKey.setButtonText ("ARM PAD " + String (selectedPad + 1).paddedLeft ('0', 2));
        refresh();
        repaint();
    }

    int SequencerPanel::lengthNow() const
    {
        return jlimit (1, 32, roundToInt (processor.getParameters().getRawParameterValue (params::seqLength)->load()));
    }

    Rectangle<float> SequencerPanel::gridArea() const
    {
        return getLocalBounds().toFloat().reduced (14.0f, 0.0f).withTrimmedTop (kHeaderH + 4.0f).withTrimmedBottom (12.0f);
    }

    Rectangle<float> SequencerPanel::laneLabelBounds (int pad) const
    {
        const auto a = gridArea();
        const float rowH = a.getHeight() / (float) SequencerData::kPads;
        return { a.getX(), a.getY() + (float) pad * rowH, kLabelW - 4.0f, rowH };
    }

    Rectangle<float> SequencerPanel::cellBounds (int pad, int step) const
    {
        const auto a = gridArea().withTrimmedLeft (kLabelW);
        const float rowH = a.getHeight() / (float) SequencerData::kPads;
        const float colW = a.getWidth() / (float) SequencerData::kSteps;
        return Rectangle<float> (a.getX() + (float) step * colW, a.getY() + (float) pad * rowH, colW, rowH).reduced (1.0f);
    }

    bool SequencerPanel::cellAt (Point<float> pos, int& pad, int& step) const
    {
        const auto a = gridArea().withTrimmedLeft (kLabelW);
        if (! a.contains (pos)) return false;
        pad  = jlimit (0, SequencerData::kPads - 1,  (int) ((pos.y - a.getY()) / (a.getHeight() / (float) SequencerData::kPads)));
        step = jlimit (0, SequencerData::kSteps - 1, (int) ((pos.x - a.getX()) / (a.getWidth()  / (float) SequencerData::kSteps)));
        return true;
    }

    void SequencerPanel::resized()
    {
        int x = 150;
        const int y = 10, h = 30;
        runKey.setBounds   (x, y, 52, h);      x += 58;
        lenRead.setBounds  (x, y - 6, 52, h + 6); x += 56;
        stepRead.setBounds (x, y - 6, 52, h + 6); x += 56;
        quantRead.setBounds (x, y - 6, 56, h + 6); x += 62;
        armKey.setBounds   (x, y, 94, h);      x += 98;
        clearKey.setBounds (x, y, jmin (68, getWidth() - x - 14), h);
        background = {};
    }

    //==========================================================================
    void SequencerPanel::mouseDown (const MouseEvent& e)
    {
        for (int p = 0; p < SequencerData::kPads; ++p)
            if (laneLabelBounds (p).contains (e.position))
            {
                setSelectedPad (p);
                if (onSelectPad) onSelectPad (p);
                return;
            }

        int pad, step;
        if (! cellAt (e.position, pad, step)) return;
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

        const auto v = seq.getVersion();
        const int step = processor.getBridge().seqStep.load();
        const int len = lengthNow();
        if (v != shownVersion || step != shownStep || len != shownLen)
        {
            shownVersion = v; shownStep = step; shownLen = len;
            repaint (gridArea().toNearestInt().expanded (2));
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
        }
        g.drawImage (background, getLocalBounds().toFloat());

        auto& seq = processor.getBridge().seq;
        const int len  = lengthNow();
        const int head = processor.getBridge().seqStep.load();

        // Playhead column.
        if (head >= 0)
        {
            const auto top = cellBounds (0, head), bottom = cellBounds (SequencerData::kPads - 1, head);
            g.setColour (c::phosphor.withAlpha (0.16f));
            g.fillRect (top.getUnion (bottom).expanded (1.0f));
        }

        for (int p = 0; p < SequencerData::kPads; ++p)
        {
            const auto lb = laneLabelBounds (p);
            if (p == selectedPad)
            {
                g.setColour (c::vfd.withAlpha (0.14f));
                g.fillRect (lb.getUnion (cellBounds (p, SequencerData::kSteps - 1)));
            }
            g.setColour (p == selectedPad ? c::vfd : c::bakelite.withAlpha (0.7f));
            g.setFont (monoFont (jmin (11.0f, lb.getHeight())));
            g.drawText (String (p + 1).paddedLeft ('0', 2), lb.withTrimmedLeft (9.0f), Justification::centredLeft, false);
            if (seq.isArmed (p))
                paint::lamp (g, { lb.getX() + 4.0f, lb.getCentreY() }, 2.8f, c::red, c::redDim, 1.0f);

            for (int s = 0; s < SequencerData::kSteps; ++s)
            {
                const auto cell = cellBounds (p, s);
                const bool inLen = s < len;
                const auto st = seq.get (p, s);
                g.setColour ((s / 4) % 2 == 0 ? Colour (0xff17191a) : Colour (0xff222526));
                g.fillRect (cell);
                if (st.on)
                {
                    const float vel = (float) st.velocity / 127.0f;
                    g.setColour (c::amber.withAlpha (inLen ? 0.35f : 0.12f));
                    g.fillRect (cell);
                    g.setColour (c::amber.withAlpha (inLen ? 1.0f : 0.3f));
                    g.fillRect (cell.withTop (cell.getBottom() - cell.getHeight() * (0.25f + 0.75f * vel)));
                    if (st.micro != 0)       // played off the grid: a tick shows where
                    {
                        g.setColour (Colours::black.withAlpha (0.7f));
                        const float x = cell.getCentreX() + cell.getWidth() * (float) st.micro / 128.0f;
                        g.drawVerticalLine (roundToInt (x), cell.getY(), cell.getBottom());
                    }
                }
                if (! inLen)
                {
                    g.setColour (Colours::black.withAlpha (0.45f));
                    g.fillRect (cell);
                }
            }
        }
    }
}
