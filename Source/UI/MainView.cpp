#include "UI/MainView.h"
#include "UI/Controls.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    //==========================================================================
    //  Layout (logical 1200 x 800). Kept together so the console reads as one
    //  drawing; later phases swap the contents of these regions.
    //==========================================================================
    namespace layout
    {
        const Rectangle<int> header      {   0,   0, 1200,  72 };
        const Rectangle<int> navRail     {  16,  80, 1168,  42 };

        const Rectangle<int> padPanel    {  16, 132,  580, 652 };
        const Rectangle<int> pads        {  36, 172,  540, 418 };
        const Rectangle<int> padEditor   {  36, 606,  540, 128 };
        const Rectangle<int> statusStrip {  36, 740,  540,  38 };

        const Rectangle<int> transPanel  { 612, 132,  572, 150 };
        const Rectangle<int> transport   { 626, 166,  544, 108 };

        const Rectangle<int> scope       { 612, 292,  572, 232 };

        const Rectangle<int> fxPanel     { 612, 534,  268, 250 };
        const Rectangle<int> fxGrid      { 626, 572,  240, 198 };

        const Rectangle<int> masterPanel { 892, 534,  292, 250 };
        const Rectangle<int> meter       { 906, 570,  264, 120 };
        const Rectangle<int> masterKnob  { 910, 694,   76,  72 };
        const Rectangle<int> limiter     { 1006, 698,  58,  80 };
        const Rectangle<int> panic       { 1084, 692,  88,  88 };
    }

    //==========================================================================
    //  PanicButton
    //==========================================================================
    PanicButton::PanicButton() : Button ("Panic")
    {
        setTooltip ("PANIC: stop every voice with a short fade (also MIDI CC 120)");
    }

    void PanicButton::paintButton (Graphics& g, bool highlighted, bool down)
    {
        const auto b   = getLocalBounds().toFloat().reduced (2.0f);
        const auto ctr = b.getCentre().translated (0.0f, -6.0f);
        const float R  = jmin (b.getWidth(), b.getHeight() - 14.0f) * 0.5f;

        // Hazard collar.
        Path collar;
        collar.addEllipse (Rectangle<float> (R * 2.0f, R * 2.0f).withCentre (ctr));
        collar.addEllipse (Rectangle<float> (R * 1.36f, R * 1.36f).withCentre (ctr));
        collar.setUsingNonZeroWinding (false);
        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillPath (collar, AffineTransform::translation (1.5f, 2.5f));
        paint::hazardStripes (g, collar, 6.0f);
        g.setColour (Colours::black.withAlpha (0.25f));   // grime on the stripes
        g.fillPath (collar);

        // Mushroom cap. Sinks and brightens when pressed.
        const float press = down ? 1.5f : 0.0f;
        const float capR  = R * 0.62f;
        auto cap = Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (ctr.translated (0.0f, press));
        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillEllipse (cap.translated (1.5f, 3.0f - press));

        const bool lit = flashFrames > 0 || down;
        const Colour redCap = lit ? Colour (0xffff3018) : Colour (0xffb4180c);
        ColourGradient capG (redCap.brighter (highlighted ? 0.45f : 0.3f), cap.getX() + capR * 0.5f, cap.getY() + capR * 0.4f,
                             redCap.darker (0.7f), cap.getRight(), cap.getBottom(), true);
        g.setGradientFill (capG);
        g.fillEllipse (cap);
        if (lit)
        {
            ColourGradient glow (Colour (0x66ff3018), ctr.x, ctr.y, Colour (0x00ff3018), ctr.x + R * 1.3f, ctr.y, true);
            g.setGradientFill (glow);
            g.fillEllipse (Rectangle<float> (R * 2.6f, R * 2.6f).withCentre (ctr));
        }

        g.setColour (Colours::white.withAlpha (0.25f));
        g.fillEllipse (cap.getX() + capR * 0.45f, cap.getY() + capR * 0.25f, capR * 0.7f, capR * 0.4f);

        g.setColour (Colour (0xff2a0602).withAlpha (0.85f));
        g.setFont (industrialFont (capR * 0.62f));
        g.drawText (String ("STOP"), cap, Justification::centred, false);

        g.setColour (c::bakelite.withAlpha (0.9f));
        g.setFont (industrialFont (11.5f));
        g.drawText ("PANIC", getLocalBounds().toFloat().removeFromBottom (14.0f), Justification::centred, false);
    }

    //==========================================================================
    //  SelectorKey
    //==========================================================================
    SelectorKey::SelectorKey (const String& latin, int phaseNumber)
        : Button (latin), phase (phaseNumber), latinName (latin)
    {
        setEnabled (phase <= kCurrentPhase);
        setTooltip (phase <= kCurrentPhase ? latin + " view"
                                           : latin + " - coming online in Phase " + String (phase));
    }

    void SelectorKey::paintButton (Graphics& g, bool highlighted, bool)
    {
        const auto b = getLocalBounds().toFloat().reduced (2.0f);
        const bool active = isEnabled();

        paint::plate (g, b, active ? Colour (0xff23251f) : Colour (0xff1a1b18), 3.0f);
        paint::lamp (g, { b.getX() + 16.0f, b.getCentreY() }, 6.5f, c::amber, c::amberDim,
                     getToggleState() ? 1.0f : (active ? 0.18f : 0.0f));

        auto txt = b.withTrimmedLeft (30.0f).reduced (2.0f, 3.0f);
        g.setColour (c::plateText.withAlpha (active ? (highlighted ? 1.0f : 0.92f) : 0.32f));
        g.setFont (industrialFont (15.0f));
        g.drawText (latinName, txt.removeFromTop (txt.getHeight() * 0.56f), Justification::centredLeft, false);
        g.setFont (industrialFont (11.0f, false));
        g.drawText (getToggleState() ? String ("SELECTED") : (active ? String ("ONLINE") : "OFFLINE - PHASE " + String (phase)),
                    txt, Justification::centredLeft, false);
    }

    //==========================================================================
    //  StatusStrip
    //==========================================================================
    StatusStrip::StatusStrip (TwistedBreaksProcessor& p) : processor (p)
    {
        setInterceptsMouseClicks (false, false);
    }

    void StatusStrip::refresh (int selectedPad)
    {
        auto& br = processor.getBridge();
        const auto count = br.midiEventCount.load (std::memory_order_relaxed);
        const int  note  = br.lastMidiNote.load (std::memory_order_relaxed);
        const int  vel   = br.lastMidiVelocity.load (std::memory_order_relaxed);
        const int  sNote = processor.getEngine().getPadNote (selectedPad);

        bool changed = false;
        if (count != lastCount) { lastCount = count; midiLamp = 1.0f; changed = true; }
        else if (midiLamp > 0.0f) { midiLamp = jmax (0.0f, midiLamp - 0.12f); changed = true; }

        if (note != lastNote || vel != lastVel || selectedPad != selPad || sNote != selNote)
        {
            lastNote = note; lastVel = vel; selPad = selectedPad; selNote = sNote;
            changed = true;
        }
        if (changed) repaint();
    }

    void StatusStrip::paint (Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        paint::recess (g, b, 4.0f);
        b.reduce (12.0f, 2.0f);

        // MIDI activity lamp.
        paint::lamp (g, { b.getX() + 12.0f, b.getCentreY() }, 9.0f, c::green, c::greenDim, midiLamp);
        b.removeFromLeft (34.0f);

        auto readout = [&] (Rectangle<float> r, const String& caption, const String& value)
        {
            g.setColour (c::bakelite.withAlpha (0.7f));
            g.setFont (industrialFont (11.5f));
            g.drawText (caption, r.removeFromTop (14.0f), Justification::centredLeft, false);
            g.setColour (c::vfd);
            g.setFont (monoFont (16.0f));
            g.drawText (value, r, Justification::centredLeft, false);
        };

        auto noteName = [] (int n) { return n < 0 ? String ("---") : MidiMessage::getMidiNoteName (n, true, true, 3) + " (" + String (n) + ")"; };

        readout (b.removeFromLeft (b.getWidth() * 0.5f), "LAST MIDI NOTE IN",
                 lastNote < 0 ? String ("--- waiting ---") : noteName (lastNote) + "  v" + String (lastVel));
        readout (b, "SELECTED PAD",
                 "PAD " + String (selPad + 1).paddedLeft ('0', 2) + "  <-  " + noteName (selNote));
    }

    //==========================================================================
    //  MainView
    //==========================================================================
    MainView::MainView (TwistedBreaksProcessor& p)
        : processor (p),
          padGrid (p),
          transportDisplay (p.getBridge().transport),
          waveform (p),
          padEditor (p),
          meter (p.getBridge()),
          status (p)
    {
        setOpaque (true);

        addAndMakeVisible (padGrid);
        addAndMakeVisible (transportDisplay);
        addAndMakeVisible (waveform);
        addAndMakeVisible (padEditor);
        addAndMakeVisible (meter);
        addAndMakeVisible (fxGrid);
        addAndMakeVisible (status);

        // Master level knob <-> "master_gain" parameter (host-automatable).
        masterKnob.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        masterKnob.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        masterKnob.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        masterKnob.setPopupDisplayEnabled (true, true, this);
        masterKnob.setTooltip ("Master level (double-click: 0 dB)");
        addAndMakeVisible (masterKnob);
        masterAttachment = std::make_unique<SliderAttachment> (p.getParameters(), params::masterGain, masterKnob);
        masterKnob.setDoubleClickReturnValue (true, 0.0);

        addAndMakeVisible (limiterSwitch);
        limiterAttachment = std::make_unique<ButtonAttachment> (p.getParameters(), params::masterLimiter, limiterSwitch);

        addAndMakeVisible (panicButton);
        panicButton.onClick = [this] { processor.requestPanic(); panicButton.flash(); };

        struct NavDef { const char* latin; int phase; };
        const NavDef nav[] = {
            { "PADS", 1 }, { "SLICER", 4 }, { "SEQUENCER", 7 },
            { "FX",   6 }, { "MIXER",  5 }, { "SNAPSHOTS", 8 },
        };
        for (const auto& n : nav)
            addAndMakeVisible (navKeys.add (new SelectorKey (n.latin, n.phase)));

        // PADS and SLICER pick the pad editor's SOUND and SLICE pages; the
        // screen shows slice markers while the SLICE page is open.
        navKeys[0]->onClick = [this] { padEditor.setPage (PadEditorPanel::Page::sound); };
        navKeys[1]->onClick = [this] { padEditor.setPage (PadEditorPanel::Page::slice); };
        padEditor.onPageChanged = [this] (PadEditorPanel::Page page)
        {
            waveform.setSliceView (page == PadEditorPanel::Page::slice || page == PadEditorPanel::Page::swap);
            navKeys[0]->setToggleState (page != PadEditorPanel::Page::slice, dontSendNotification);
            navKeys[1]->setToggleState (page == PadEditorPanel::Page::slice, dontSendNotification);
        };
        padEditor.onFitView = [this] { waveform.fitView(); };
        navKeys[0]->setToggleState (true, dontSendNotification);

        padGrid.onSelectionChanged = [this] (int pad)
        {
            waveform.setPad (pad);
            padEditor.setPad (pad);
            status.refresh (pad);
        };

        shownNoteMapVersion = p.getEngine().getNoteMapVersion();

        // Size last: resized() lays out the children created above.
        setSize (kWidth, kHeight);
        startTimerHz (30);
    }

    MainView::~MainView()
    {
        stopTimer();
    }

    void MainView::resized()
    {
        padGrid.setBounds (layout::pads);
        status.setBounds (layout::statusStrip);
        transportDisplay.setBounds (layout::transport);
        waveform.setBounds (layout::scope);
        padEditor.setBounds (layout::padEditor);
        fxGrid.setBounds (layout::fxGrid);
        meter.setBounds (layout::meter);
        masterKnob.setBounds (layout::masterKnob);
        limiterSwitch.setBounds (layout::limiter);
        panicButton.setBounds (layout::panic);

        auto rail = layout::navRail;
        const int keyW = rail.getWidth() / juce::jmax (1, navKeys.size());
        for (auto* k : navKeys)
            k->setBounds (rail.removeFromLeft (keyW).reduced (3, 0));

        rebuildBackground();
    }

    void MainView::timerCallback()
    {
        padGrid.refresh();
        transportDisplay.refresh();
        waveform.refresh();
        padEditor.refresh();

        // MIDI-learn / note edits change saved state: tell the host.
        const auto nmv = processor.getEngine().getNoteMapVersion();
        if (nmv != shownNoteMapVersion)
        {
            shownNoteMapVersion = nmv;
            processor.markStateChanged();
        }
        meter.refresh();
        status.refresh (padGrid.getSelectedPad());
        panicButton.tick();
    }

    void MainView::paint (Graphics& g)
    {
        g.drawImage (background, getLocalBounds().toFloat());
    }

    //==========================================================================
    namespace
    {
        /** Radiation trefoil sticker, faded and peeling. */
        void trefoilSticker (Graphics& g, Point<float> ctr, float r, Random& rnd)
        {
            Path tri;
            tri.addTriangle (ctr.translated (0, -r), ctr.translated (r * 0.866f, r * 0.5f), ctr.translated (-r * 0.866f, r * 0.5f));
            tri = tri.createPathWithRoundedCorners (r * 0.12f);

            g.setColour (Colours::black.withAlpha (0.4f));
            g.fillPath (tri, AffineTransform::translation (1.5f, 2.0f));
            g.setColour (c::hazardYellow.withMultipliedSaturation (0.75f).withMultipliedBrightness (0.9f));
            g.fillPath (tri);
            g.setColour (Colour (0xff141210));
            g.strokePath (tri, PathStrokeType (r * 0.07f));

            const auto k = ctr.translated (0.0f, r * 0.12f);
            const float inner = r * 0.08f, outer = r * 0.42f;
            for (int i = 0; i < 3; ++i)
            {
                const float a0 = MathConstants<float>::twoPi * (float) i / 3.0f - 0.52f;
                Path blade;
                blade.addPieSegment (Rectangle<float> (outer * 2.0f, outer * 2.0f).withCentre (k), a0, a0 + 1.047f,
                                     (inner + r * 0.06f) / outer);
                g.fillPath (blade);
            }
            g.fillEllipse (Rectangle<float> (inner * 2.0f, inner * 2.0f).withCentre (k));

            // Wear on the sticker.
            for (int i = 0; i < 40; ++i)
            {
                g.setColour (c::steel.withAlpha (0.5f + rnd.nextFloat() * 0.4f));
                const float s = 0.8f + rnd.nextFloat() * 2.5f;
                g.fillEllipse (ctr.x - r + rnd.nextFloat() * r * 2.0f, ctr.y - r + rnd.nextFloat() * r * 1.6f, s, s);
            }
        }
    }

    void MainView::rebuildBackground()
    {
        const float s = 2.0f;
        background = Image (Image::RGB, kWidth * (int) s, kHeight * (int) s, true);
        Graphics g (background);
        g.addTransform (AffineTransform::scale (s));
        Random rnd (19790426);

        const auto all = Rectangle<float> (0, 0, (float) kWidth, (float) kHeight);

        // 1. Frame steel: the cabinet the modules are bolted into.
        ColourGradient cab (c::steelDark.brighter (0.05f), 0, 0, c::grime.brighter (0.05f), 0, (float) kHeight, false);
        g.setGradientFill (cab);
        g.fillRect (all);
        paint::ageArea (g, all, rnd, 1.0f);

        // 2. Module plates.
        const Rectangle<int> modules[] = { layout::padPanel, layout::transPanel, layout::fxPanel, layout::masterPanel };
        for (auto m : modules)
        {
            const auto r = m.toFloat();
            paint::plate (g, r, c::steel, 4.0f);
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (m);
            paint::ageArea (g, r, rnd, 1.3f);
            paint::edgeWear (g, r, rnd, 1.5f);
        }

        // 3. Header rail: a heavier, more rusted plate across the top.
        {
            const auto h = layout::header.toFloat().reduced (8.0f, 6.0f);
            paint::plate (g, h, c::steelLight.darker (0.25f), 3.0f);
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (h.toNearestInt());
            paint::ageArea (g, h, rnd, 2.0f);
            paint::edgeWear (g, h, rnd, 2.0f);
            for (int i = 0; i < 5; ++i)
                paint::rustBloom (g, { h.getX() + rnd.nextFloat() * h.getWidth(), h.getBottom() - rnd.nextFloat() * 10.0f },
                                  14.0f + rnd.nextFloat() * 16.0f, rnd);
        }

        // 4. Rivets: module corners + a row along the header rail.
        for (auto m : modules)
        {
            const auto r = m.toFloat().reduced (9.0f);
            for (auto p : { r.getTopLeft(), r.getTopRight(), r.getBottomLeft(), r.getBottomRight() })
                paint::rivet (g, p, 4.2f, rnd);
        }
        for (float x = 26.0f; x < (float) kWidth - 20.0f; x += 58.0f)
        {
            paint::rivet (g, { x, 16.0f }, 3.6f, rnd, false);
            paint::rivet (g, { x, 58.0f }, 3.6f, rnd, true);
        }

        // 5. Title: raised cast lettering on the rail.
        {
            auto title = Rectangle<float> (34.0f, 15.0f, 600.0f, 46.0f);
            paint::plate (g, title, c::plateBlack, 3.0f);
            g.setFont (industrialFont (40.0f).withExtraKerningFactor (0.12f));
            g.setColour (Colours::black);
            g.drawText ("TWISTED BREAKS", title.translated (2.0f, 2.5f).withTrimmedLeft (16.0f), Justification::centredLeft, false);
            g.setColour (c::bakelite);
            g.drawText ("TWISTED BREAKS", title.withTrimmedLeft (16.0f), Justification::centredLeft, false);

            // Worn paint on the cast letters.
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (title.toNearestInt());
            for (int i = 0; i < 320; ++i)
            {
                g.setColour (c::plateBlack.withAlpha (0.4f + rnd.nextFloat() * 0.5f));
                const float sz = 0.8f + rnd.nextFloat() * 2.2f;
                g.fillEllipse (title.getX() + rnd.nextFloat() * title.getWidth(),
                               title.getY() + rnd.nextFloat() * title.getHeight(), sz, sz);
            }
            g.setColour (c::plateText.withAlpha (0.8f));
            g.setFont (industrialFont (12.0f, false));
            g.drawText (cyr ("ПУЛЬТ УПРАВЛЕНИЯ БРЕЙКАМИ  ·  ТБ-1"), title.reduced (12.0f, 4.0f),
                        Justification::bottomRight, false);
        }

        paint::labelPlate (g, { 890.0f, 20.0f, 220.0f, 36.0f }, "TWISTED GENETICS",
                           cyr ("ЗАВОД № 4  ·  ЗАВ. № 0001  ·  1979"), paint::PlateStyle::bakelite, rnd);
        trefoilSticker (g, { 1146.0f, 38.0f }, 20.0f, rnd);
        paint::stencilText (g, cyr ("ОСТОРОЖНО"), { 650.0f, 24.0f, 220.0f, 28.0f }, 24.0f,
                            Justification::centred, c::hazardYellow.withAlpha (0.55f), rnd);

        // 6. Section captions.
        auto caption = [&] (Rectangle<int> panel, const String& text, float width)
        {
            paint::labelPlate (g, { (float) panel.getX() + 22.0f, (float) panel.getY() + 9.0f, width, 22.0f },
                               text, {}, paint::PlateStyle::bakelite, rnd);
        };
        caption (layout::padPanel,    "PAD MATRIX",     170.0f);
        caption (layout::transPanel,  "HOST TRANSPORT", 190.0f);
        caption (layout::fxPanel,     "PERFORMANCE FX", 190.0f);
        caption (layout::masterPanel, "MASTER",         120.0f);

        // 7. Engraved seam between the pad matrix and the selected-pad controls.
        g.setColour (Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (layout::padEditor.getY() - 9, 30.0f, 582.0f);
        g.setColour (Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine (layout::padEditor.getY() - 8, 30.0f, 582.0f);

        // 8. Master section legends.
        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (11.5f));
        g.drawText ("LEVEL", layout::masterKnob.toFloat().withY ((float) layout::masterKnob.getBottom()).withHeight (14.0f).expanded (14.0f, 0.0f),
                    Justification::centred, false);

        // 9. Panel seams between cabinet sections.
        g.setColour (Colours::black.withAlpha (0.55f));
        g.drawLine (604.0f, 128.0f, 604.0f, 792.0f, 2.0f);
        g.drawLine (8.0f, 126.0f, 1192.0f, 126.0f, 2.0f);
        g.setColour (Colours::white.withAlpha (0.05f));
        g.drawLine (606.0f, 128.0f, 606.0f, 792.0f, 1.0f);
    }
}
