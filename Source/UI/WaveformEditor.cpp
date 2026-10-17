#include "UI/WaveformEditor.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"
#include "Engine/PadVoice.h"
#include <cmath>

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr float kMarkerGrab   = 8.0f;    // px either side of a marker
        constexpr int   kMinViewSamps = 64;      // deepest zoom
        constexpr float kImageScale   = 2.0f;    // bake at 2x for Retina
    }

    WaveformEditor::WaveformEditor (TwistedBreaksProcessor& p)
        : processor (p), bridge (p.getBridge())
    {
        setTooltip ("Drag S / E to set start and end. Wheel or pinch to zoom, drag to scroll, "
                    "double-click to zoom out. Drop a WAV / AIFF here to load it.");
    }

    //==========================================================================
    Rectangle<float> WaveformEditor::screenArea() const
    {
        return getLocalBounds().toFloat().reduced (20.0f, 18.0f);
    }

    Rectangle<float> WaveformEditor::plotArea() const
    {
        // Leave room for the legends at top and bottom of the glass.
        return screenArea().reduced (14.0f, 0.0f).withTrimmedTop (30.0f).withTrimmedBottom (26.0f);
    }

    float WaveformEditor::xForFraction (double f) const
    {
        const auto r = plotArea();
        return r.getX() + (float) ((f - viewStart) / viewLength) * r.getWidth();
    }

    double WaveformEditor::fractionForX (float x) const
    {
        const auto r = plotArea();
        return viewStart + (double) ((x - r.getX()) / jmax (1.0f, r.getWidth())) * viewLength;
    }

    RangedAudioParameter* WaveformEditor::param (const char* name) const
    {
        return processor.getParameters().getParameter (params::padId (pad, name));
    }

    float WaveformEditor::paramValue (const char* name) const
    {
        if (auto* v = processor.getParameters().getRawParameterValue (params::padId (pad, name)))
            return v->load();
        return 0.0f;
    }

    WaveformEditor::Marker WaveformEditor::markerAt (float x) const
    {
        if (info.buffer == nullptr) return Marker::none;
        const float xs = xForFraction (paramValue (params::padStart));
        const float xe = xForFraction (paramValue (params::padEnd));
        const float ds = std::abs (x - xs), de = std::abs (x - xe);
        if (ds > kMarkerGrab && de > kMarkerGrab) return Marker::none;
        return ds <= de ? Marker::start : Marker::end;
    }

    //==========================================================================
    void WaveformEditor::setPad (int newPad)
    {
        if (newPad == pad && shownVersion != 0xffffffff) return;
        pad = jlimit (0, kNumPads - 1, newPad);
        shownVersion = 0xffffffff;      // force a reload of the pad's info
        viewStart = 0.0; viewLength = 1.0;
        refresh();
        repaint();
    }

    void WaveformEditor::refresh()
    {
        // Keep the scope FIFO drained even in waveform mode.
        const int n = bridge.pullScope (scratch.data(), (int) scratch.size());
        float energy = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            history[(size_t) writePos] = scratch[(size_t) i];
            writePos = (writePos + 1) % historySize;
            energy += std::abs (scratch[(size_t) i]);
        }

        auto& samples = processor.getSamples();
        if (samples.getVersion (pad) != shownVersion)
        {
            info = samples.getInfo (pad);
            shownVersion = info.version;
            waveDirty = true;
            if (info.buffer == nullptr) { viewStart = 0.0; viewLength = 1.0; }
            repaint();
        }

        if (info.buffer != nullptr)
        {
            const float s = paramValue (params::padStart);
            const float e = paramValue (params::padEnd);
            const bool  r = paramValue (params::padReverse) > 0.5f;
            const float h = bridge.padPlayhead[(size_t) pad].load (std::memory_order_relaxed);

            auto moved = [] (float a, float b) { return std::abs (a - b) > 1.0e-6f; };
            const int how = roundToInt (paramValue (params::padSlice)) * 1000 + roundToInt (paramValue (params::padMode)) * 100
                          + roundToInt (paramValue (params::padRate)) * 10 + (paramValue (params::padSync) > 0.5f ? 1 : 0);
            if (moved (s, lastStart) || moved (e, lastEnd) || r != lastReverse || moved (h, lastHead) || how != lastHow)
            {
                lastStart = s; lastEnd = e; lastReverse = r; lastHead = h; lastHow = how;
                repaint();
            }
        }
        else
        {
            const bool signal = energy > 1.0e-3f;
            if (n > 0 && (signal || hadSignal)) repaint();
            hadSignal = signal;
        }
    }

    //==========================================================================
    void WaveformEditor::clampView()
    {
        double minLen = 1.0;
        if (info.buffer != nullptr)
            minLen = jmin (1.0, (double) kMinViewSamps / jmax (1, info.buffer->getNumSamples()));
        viewLength = jlimit (minLen, 1.0, viewLength);
        viewStart  = jlimit (0.0, 1.0 - viewLength, viewStart);
    }

    void WaveformEditor::zoomAround (float x, double factor)
    {
        if (info.buffer == nullptr) return;
        const double anchor = fractionForX (x);
        const double rel    = (anchor - viewStart) / viewLength;
        viewLength *= factor;
        clampView();
        viewStart = anchor - rel * viewLength;
        clampView();
        waveDirty = true;
        repaint();
    }

    void WaveformEditor::scrollBy (double fraction)
    {
        viewStart += fraction;
        clampView();
        waveDirty = true;
        repaint();
    }

    //==========================================================================
    bool WaveformEditor::editingSlices() const
    {
        if (info.buffer == nullptr) return false;
        return sliceView || (roundToInt (paramValue (params::padMode)) == params::modeSlice && ! info.slices.isEmpty());
    }

    int WaveformEditor::sliceMarkerAt (float x) const
    {
        if (! editingSlices()) return -1;
        const double len = (double) info.buffer->getNumSamples();
        int best = -1; float bestD = kMarkerGrab;
        for (int i = 0; i < info.slices.count; ++i)
        {
            const float d = std::abs (x - xForFraction (info.slices.starts[(size_t) i] / len));
            if (d <= bestD) { bestD = d; best = i; }
        }
        return best;
    }

    int WaveformEditor::sampleAt (float x) const
    {
        return jlimit (0, info.buffer->getNumSamples() - 1,
                       (int) std::lround (fractionForX (x) * info.buffer->getNumSamples()));
    }

    void WaveformEditor::mouseMove (const MouseEvent& e)
    {
        const bool onMarker = editingSlices() ? sliceMarkerAt ((float) e.x) >= 0
                                              : markerAt ((float) e.x) != Marker::none;
        setMouseCursor (onMarker ? MouseCursor::LeftRightResizeCursor : MouseCursor::NormalCursor);
    }

    void WaveformEditor::syncInfo()
    {
        // Always act on the latest markers, not the last 30 Hz snapshot.
        auto& samples = processor.getSamples();
        if (samples.getVersion (pad) != shownVersion)
        {
            info = samples.getInfo (pad);
            shownVersion = info.version;
            waveDirty = true;
            repaint();
        }
    }

    void WaveformEditor::mouseDown (const MouseEvent& e)
    {
        syncInfo();
        dragging = Marker::none;
        draggingSlice = -1;
        draggingView = false;
        auditioning = false;

        if (editingSlices())
        {
            draggingSlice = sliceMarkerAt ((float) e.x);
            if (draggingSlice >= 0) return;
        }
        else
        {
            dragging = markerAt ((float) e.x);
            if (dragging != Marker::none)
            {
                if (auto* p = param (dragging == Marker::start ? params::padStart : params::padEnd))
                    p->beginChangeGesture();
                return;
            }
        }

        if (info.buffer != nullptr && viewLength < 1.0)
        {
            draggingView = true;
            dragAnchorFraction  = fractionForX ((float) e.x);
            dragAnchorViewStart = viewStart;
        }
    }

    void WaveformEditor::mouseDrag (const MouseEvent& e)
    {
        if (draggingSlice >= 0 && info.buffer != nullptr)
        {
            processor.getSamples().moveMarker (pad, draggingSlice, sampleAt ((float) e.x));
            syncInfo();
        }
        else if (dragging != Marker::none && info.buffer != nullptr)
        {
            // Keep at least 64 samples between the markers.
            const double gap = jmin (0.5, 64.0 / jmax (1, info.buffer->getNumSamples()));
            const double f   = jlimit (0.0, 1.0, fractionForX ((float) e.x));
            const bool   isStart = dragging == Marker::start;
            const double v = isStart ? jmin (f, (double) paramValue (params::padEnd) - gap)
                                     : jmax (f, (double) paramValue (params::padStart) + gap);

            if (auto* p = param (isStart ? params::padStart : params::padEnd))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) jlimit (0.0, 1.0, v)));
        }
        else if (draggingView && e.getDistanceFromDragStart() > 3)
        {
            setMouseCursor (MouseCursor::DraggingHandCursor);
            const double nowAt = (double) (((float) e.x - plotArea().getX()) / jmax (1.0f, plotArea().getWidth())) * viewLength;
            viewStart = dragAnchorFraction - nowAt;
            juce::ignoreUnused (dragAnchorViewStart);
            clampView();
            waveDirty = true;
            repaint();
        }
    }

    void WaveformEditor::mouseUp (const MouseEvent& e)
    {
        syncInfo();
        if (dragging != Marker::none)
            if (auto* p = param (dragging == Marker::start ? params::padStart : params::padEnd))
                p->endChangeGesture();

        // A plain click inside a slice selects it (and plays it when the pad is in SLICE mode).
        const bool click = e.getDistanceFromDragStart() <= 3 && draggingSlice < 0 && dragging == Marker::none;
        if (click && editingSlices() && ! info.slices.isEmpty() && e.getNumberOfClicks() == 1)
        {
            const int k = info.slices.sliceAt (sampleAt ((float) e.x));
            if (auto* p = param (params::padSlice))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 ((float) (k + 1)));
                p->endChangeGesture();
            }
            if (roundToInt (paramValue (params::padMode)) == params::modeSlice)
            {
                processor.triggerPadFromUi (pad, 0.9f, true);
                processor.triggerPadFromUi (pad, 0.0f, false);
            }
        }

        dragging = Marker::none;
        draggingSlice = -1;
        draggingView = false;
        mouseMove (e);
    }

    void WaveformEditor::mouseDoubleClick (const MouseEvent& e)
    {
        syncInfo();
        if (editingSlices())
        {
            // Double-click a marker to delete it, empty space to add one.
            const int m = sliceMarkerAt ((float) e.x);
            if (m >= 0) processor.getSamples().deleteMarker (pad, m);
            else        processor.getSamples().addMarker (pad, sampleAt ((float) e.x));
            return;
        }
        fitView();
    }

    void WaveformEditor::fitView()
    {
        viewStart = 0.0; viewLength = 1.0;
        waveDirty = true;
        repaint();
    }

    void WaveformEditor::setSliceView (bool shouldShow)
    {
        if (shouldShow != sliceView) { sliceView = shouldShow; repaint(); }
    }

    void WaveformEditor::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
    {
        if (info.buffer == nullptr) return;

        // Horizontal swipe (or shift + wheel) scrolls; vertical wheel zooms.
        auto nonZero = [] (float v) { return std::abs (v) > 1.0e-6f; };
        const float horiz = nonZero (w.deltaX) ? w.deltaX : (e.mods.isShiftDown() ? w.deltaY : 0.0f);
        if (nonZero (horiz))
            scrollBy (-(double) horiz * viewLength * 0.5);
        else if (nonZero (w.deltaY))
            zoomAround ((float) e.x, std::exp (-(double) w.deltaY * (w.isReversed ? -1.5 : 1.5)));
    }

    void WaveformEditor::mouseMagnify (const MouseEvent& e, float scale)
    {
        if (scale > 0.0f) zoomAround ((float) e.x, 1.0 / (double) scale);
    }

    //==========================================================================
    bool WaveformEditor::isInterestedInFileDrag (const StringArray& files)
    {
        for (auto& f : files)
            if (SampleManager::isSupportedFile (File (f))) return true;
        return false;
    }

    void WaveformEditor::fileDragEnter (const StringArray&, int, int) { fileHover = true;  repaint(); }
    void WaveformEditor::fileDragExit  (const StringArray&)           { fileHover = false; repaint(); }

    void WaveformEditor::filesDropped (const StringArray& files, int, int)
    {
        fileHover = false;
        for (auto& f : files)
            if (SampleManager::isSupportedFile (File (f)))
            {
                processor.getSamples().loadFile (pad, File (f));
                break;
            }
        repaint();
    }

    //==========================================================================
    void WaveformEditor::resized()
    {
        rebuildLayers();
        waveDirty = true;
    }

    void WaveformEditor::rebuildLayers()
    {
        const auto b = getLocalBounds();
        if (b.isEmpty()) return;

        const float s = kImageScale;
        const auto  bounds = b.toFloat();
        const auto  scr    = screenArea();
        Random rnd (1986);

        // ---- Under: bezel, screen, graticule ------------------------------------
        under = Image (Image::ARGB, roundToInt (b.getWidth() * s), roundToInt (b.getHeight() * s), true);
        {
            Graphics g (under);
            g.addTransform (AffineTransform::scale (s));

            paint::plate (g, bounds.reduced (2.0f), c::steelDark.brighter (0.15f), 8.0f);
            {
                Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (bounds.reduced (2.0f).toNearestInt());
                paint::ageArea (g, bounds, rnd, 1.2f);
                paint::edgeWear (g, bounds.reduced (2.0f), rnd, 1.0f);
            }
            for (auto p : { bounds.getTopLeft().translated (10, 9),    bounds.getTopRight().translated (-10, 9),
                            bounds.getBottomLeft().translated (10, -9), bounds.getBottomRight().translated (-10, -9) })
                paint::screw (g, p, 4.0f, rnd.nextFloat() * 3.0f);

            paint::recess (g, scr.expanded (5.0f), 14.0f);

            ColourGradient tube (Colour (0xff0d2214), scr.getCentreX(), scr.getCentreY(),
                                 c::crtGlass, scr.getX(), scr.getY(), true);
            g.setGradientFill (tube);
            g.fillRoundedRectangle (scr, 12.0f);

            g.setColour (c::phosphor.withAlpha (0.08f));
            for (int i = 1; i < 10; ++i)
            {
                const float x = scr.getX() + scr.getWidth() * (float) i / 10.0f;
                g.drawVerticalLine (roundToInt (x), scr.getY() + 6, scr.getBottom() - 6);
            }
            for (int i = 1; i < 8; ++i)
            {
                const float y = scr.getY() + scr.getHeight() * (float) i / 8.0f;
                g.drawHorizontalLine (roundToInt (y), scr.getX() + 6, scr.getRight() - 6);
            }
        }

        // ---- Over: scanlines, vignette, burn-in, glass sheen --------------------
        over = Image (Image::ARGB, roundToInt (b.getWidth() * s), roundToInt (b.getHeight() * s), true);
        {
            Graphics g (over);
            g.addTransform (AffineTransform::scale (s));
            Graphics::ScopedSaveState ss (g);
            Path screen; screen.addRoundedRectangle (scr, 12.0f);
            g.reduceClipRegion (screen);

            g.setColour (Colours::black.withAlpha (0.20f));
            for (float y = scr.getY(); y < scr.getBottom(); y += 3.0f)
                g.fillRect (scr.getX(), y, scr.getWidth(), 1.0f);

            ColourGradient vig (Colours::transparentBlack, scr.getCentreX(), scr.getCentreY(),
                                Colours::black.withAlpha (0.7f), scr.getX() - 10.0f, scr.getY() - 10.0f, true);
            g.setGradientFill (vig);
            g.fillRect (scr);

            g.setColour (c::phosphor.withAlpha (0.03f));
            g.drawEllipse (scr.withSizeKeepingCentre (scr.getHeight() * 0.8f, scr.getHeight() * 0.8f), 6.0f);

            ColourGradient sheen (Colours::white.withAlpha (0.06f), scr.getX() + scr.getWidth() * 0.22f, scr.getY() + scr.getHeight() * 0.12f,
                                  Colours::white.withAlpha (0.0f), scr.getX() + scr.getWidth() * 0.55f, scr.getY() + scr.getHeight() * 0.5f, true);
            g.setGradientFill (sheen);
            g.fillRect (scr);
        }
    }

    void WaveformEditor::rebuildWaveImage()
    {
        waveDirty = false;
        const auto area = plotArea();
        if (area.isEmpty() || info.buffer == nullptr) { wave = {}; return; }

        const int w = roundToInt (area.getWidth()  * kImageScale);
        const int h = roundToInt (area.getHeight() * kImageScale);
        wave = Image (Image::ARGB, w, h, true);
        Graphics g (wave);

        const auto& buf = *info.buffer;
        const double total  = (double) buf.getNumSamples();
        const double first  = viewStart * total;
        const double perCol = viewLength * total / (double) w;
        const float  mid    = (float) h * 0.5f;
        const float  half   = (float) h * 0.47f;

        Path shape;
        std::vector<float> lows ((size_t) w);
        for (int x = 0; x < w; ++x)
        {
            const int s0 = (int) std::floor (first + perCol * x);
            const int s1 = jmax (s0 + 1, (int) std::floor (first + perCol * (x + 1)));
            const auto r = buf.peakRange (s0, s1);
            const float yTop = mid - jlimit (-1.0f, 1.0f, r.getEnd())   * half;
            lows[(size_t) x] = mid - jlimit (-1.0f, 1.0f, r.getStart()) * half;
            if (x == 0) shape.startNewSubPath ((float) x, yTop); else shape.lineTo ((float) x, yTop);
        }
        for (int x = w - 1; x >= 0; --x)
            shape.lineTo ((float) x, lows[(size_t) x] + 1.0f);
        shape.closeSubPath();

        // Phosphor: soft wide glow, body, bright edge.
        g.setColour (c::phosphor.withAlpha (0.12f));
        g.strokePath (shape, PathStrokeType (6.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c::phosphor.withAlpha (0.55f));
        g.fillPath (shape);
        g.setColour (c::phosphor.brighter (0.4f).withAlpha (0.9f));
        g.strokePath (shape, PathStrokeType (1.2f));

        g.setColour (c::phosphor.withAlpha (0.25f));
        g.drawHorizontalLine (roundToInt (mid), 0.0f, (float) w);
    }

    //==========================================================================
    void WaveformEditor::paint (Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.drawImage (under, bounds);

        {
            Graphics::ScopedSaveState ss (g);
            Path screen; screen.addRoundedRectangle (screenArea(), 12.0f);
            g.reduceClipRegion (screen);

            if (info.buffer != nullptr) paintWave (g, plotArea());
            else                        paintScope (g, screenArea().reduced (8.0f, 6.0f));
        }

        g.drawImage (over, bounds);
        paintLegend (g);

        if (fileHover)
        {
            g.setColour (c::amber.withAlpha (0.8f));
            g.drawRoundedRectangle (screenArea().reduced (3.0f), 10.0f, 3.0f);
        }
    }

    void WaveformEditor::paintWave (Graphics& g, Rectangle<float> area)
    {
        if (waveDirty || wave.isNull()) rebuildWaveImage();
        g.drawImage (wave, area);

        const auto  scr = screenArea();
        auto dimSpan = [&] (float x0, float x1)
        {
            x0 = jmax (x0, scr.getX()); x1 = jmin (x1, scr.getRight());
            if (x1 > x0) g.fillRect (Rectangle<float>::leftTopRightBottom (x0, scr.getY(), x1, scr.getBottom()));
        };

        if (editingSlices())
        {
            paintSlices (g, area);
        }
        else
        {
            const float s = jmin (paramValue (params::padStart), paramValue (params::padEnd));
            const float e = jmax (paramValue (params::padStart), paramValue (params::padEnd));
            const float xs = xForFraction (s), xe = xForFraction (e);

            // Dim what won't play.
            g.setColour (Colours::black.withAlpha (0.62f));
            dimSpan (scr.getX(), xs);
            dimSpan (xe, scr.getRight());

            // Markers: amber lines with flags.
            auto marker = [&] (float x, const String& label, bool flagRight)
        {
            if (x < scr.getX() - 1.0f || x > scr.getRight() + 1.0f) return;
            g.setColour (c::amber.withAlpha (0.25f));
            g.fillRect (x - 2.5f, area.getY() - 4.0f, 5.0f, area.getHeight() + 8.0f);
            g.setColour (c::amber);
            g.fillRect (x - 0.75f, area.getY() - 4.0f, 1.5f, area.getHeight() + 8.0f);

            auto flag = Rectangle<float> (16.0f, 14.0f).withPosition (flagRight ? x : x - 16.0f, area.getY() - 18.0f);
            g.fillRect (flag);
            g.setColour (Colour (0xff1a0e02));
            g.setFont (industrialFont (12.0f));
            g.drawText (label, flag, Justification::centred, false);
        };
            marker (xs, "S", true);
            marker (xe, "E", false);
        }

        // Playback cursor.
        const float head = bridge.padPlayhead[(size_t) pad].load (std::memory_order_relaxed);
        if (head >= 0.0f)
        {
            const float xh = xForFraction (head);
            if (xh >= area.getX() && xh <= area.getRight())
            {
                g.setColour (Colours::white.withAlpha (0.25f));
                g.fillRect (xh - 3.0f, area.getY(), 6.0f, area.getHeight());
                g.setColour (Colour (0xffe8ffe8));
                g.fillRect (xh - 0.75f, area.getY(), 1.5f, area.getHeight());
            }
        }
    }

    void WaveformEditor::paintSlices (Graphics& g, Rectangle<float> area)
    {
        const auto scr = screenArea();
        const auto& t  = info.slices;
        const double len = (double) info.buffer->getNumSamples();
        const bool sliceMode = roundToInt (paramValue (params::padMode)) == params::modeSlice;
        const int  current   = jlimit (0, jmax (0, t.count - 1), roundToInt (paramValue (params::padSlice)) - 1);

        // The slice this pad plays: everything else slightly dimmed.
        if (! t.isEmpty())
        {
            const auto r = t.region (current, (int) len);
            const float x0 = xForFraction (r.getStart() / len), x1 = xForFraction (r.getEnd() / len);
            g.setColour (Colours::black.withAlpha (sliceMode ? 0.45f : 0.25f));
            if (x0 > scr.getX())     g.fillRect (Rectangle<float>::leftTopRightBottom (scr.getX(), scr.getY(), jmin (x0, scr.getRight()), scr.getBottom()));
            if (x1 < scr.getRight()) g.fillRect (Rectangle<float>::leftTopRightBottom (jmax (x1, scr.getX()), scr.getY(), scr.getRight(), scr.getBottom()));
            g.setColour (c::amber.withAlpha (0.07f));
            g.fillRect (Rectangle<float>::leftTopRightBottom (jmax (x0, scr.getX()), area.getY(), jmin (x1, scr.getRight()), area.getBottom()));
        }

        // Numbered markers.
        for (int i = 0; i < t.count; ++i)
        {
            const float x = xForFraction (t.starts[(size_t) i] / len);
            if (x < scr.getX() - 1.0f || x > scr.getRight() + 1.0f) continue;
            const bool on = i == current;
            g.setColour ((on ? c::amber : c::amber.withAlpha (0.65f)));
            g.fillRect (x - 0.6f, area.getY() - 2.0f, on ? 1.6f : 1.2f, area.getHeight() + 6.0f);

            auto flag = Rectangle<float> (i + 1 >= 10 ? 18.0f : 13.0f, 13.0f).withPosition (x, area.getY() - 2.0f);
            g.setColour (on ? c::amber : c::amber.withAlpha (0.55f));
            g.fillRect (flag);
            g.setColour (Colour (0xff1a0e02));
            g.setFont (industrialFont (11.5f));
            g.drawText (String (i + 1), flag, Justification::centred, false);
        }
    }

    void WaveformEditor::paintScope (Graphics& g, Rectangle<float> scr)
    {
        const int window = 900;
        int start = (writePos - window - 1 + historySize) % historySize;
        for (int k = 0; k < 600; ++k)
        {
            const int i0 = (writePos - window - 1 - k + historySize * 2) % historySize;
            const int i1 = (i0 + 1) % historySize;
            if (history[(size_t) i0] <= 0.0f && history[(size_t) i1] > 0.0f) { start = i1; break; }
        }

        Path trace;
        const float midY = scr.getCentreY(), halfH = scr.getHeight() * 0.40f;
        for (int k = 0; k < window; ++k)
        {
            const float v = jlimit (-1.1f, 1.1f, history[(size_t) ((start + k) % historySize)]);
            const float x = scr.getX() + scr.getWidth() * (float) k / (float) (window - 1);
            const float y = midY - v * halfH;
            if (k == 0) trace.startNewSubPath (x, y); else trace.lineTo (x, y);
        }

        g.setColour (c::phosphor.withAlpha (0.10f));
        g.strokePath (trace, PathStrokeType (7.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c::phosphor.withAlpha (0.35f));
        g.strokePath (trace, PathStrokeType (2.8f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c::phosphor.brighter (0.6f));
        g.strokePath (trace, PathStrokeType (1.1f));
    }

    void WaveformEditor::paintLegend (Graphics& g)
    {
        auto txt = screenArea().reduced (18.0f, 10.0f);
        const String padName = "PAD " + padLabel (pad);
        g.setFont (monoFont (12.0f));

        if (info.buffer != nullptr)
        {
            const auto& buf = *info.buffer;
            g.setColour (c::phosphor.withAlpha (0.9f));
            g.drawText (padName + "  " + buf.name.toUpperCase(), txt.withTrimmedRight (240.0f),
                        Justification::topLeft, true);

            const String fmt = String (AudioEngine::tempoOf (buf), 1) + " BPM   " + String (buf.getLengthSeconds(), 3) + " s   "
                             + String (buf.sourceSampleRate / 1000.0, 1) + " kHz   "
                             + (buf.getNumChannels() > 1 ? "STEREO" : "MONO");
            g.drawText (fmt, txt, Justification::topRight, false);

            const double s = jmin (lastStart, lastEnd), e = jmax (lastStart, lastEnd);
            String region = "REGION " + String ((e - s) * buf.getLengthSeconds(), 3) + " s";
            if (editingSlices() && ! info.slices.isEmpty())
            {
                const int k = jlimit (0, info.slices.count - 1, roundToInt (paramValue (params::padSlice)) - 1);
                const auto r = info.slices.region (k, buf.getNumSamples());
                region = "SLICE " + String (k + 1) + "  " + String (r.getLength() / buf.sourceSampleRate, 3) + " s";
            }
            region << "   ZOOM " << String (1.0 / viewLength, 1) << "x";
            g.setColour (c::phosphor.withAlpha (0.6f));
            g.drawText (region, txt, Justification::bottomLeft, false);

            // Bottom right: how this pad plays.
            String how;
            if (info.status == SampleManager::Status::loading)
                how = "LOADING...";
            else
            {
                const int mode = roundToInt (paramValue (params::padMode));
                if (mode == params::modeLoop)
                {
                    const String rate = String (params::loopRates[jlimit (0, 3, roundToInt (paramValue (params::padRate)))]) + "x";
                    if (paramValue (params::padSync) > 0.5f)
                    {
                        const double beats = PadVoice::loopBeatsFor ((e - s) * buf.getLengthSeconds());
                        how = "LOOP  SYNC " + String (beats, beats < 1.0 ? 2 : 0) + (beats < 1.5 && beats > 0.75 ? " BEAT  " : " BEATS  ") + rate;
                    }
                    else
                    {
                        how = "LOOP  FREE  " + rate;
                    }
                }
                if (editingSlices())
                {
                    if (info.slicing)                how = "SLICING...";
                    else if (info.slices.isEmpty())  how = "NO SLICES - AUTO SLICE, EQUAL, OR DOUBLE-CLICK TO ADD";
                    else how = (mode == params::modeSlice ? "SLICE " + String (jlimit (1, info.slices.count, roundToInt (paramValue (params::padSlice))))
                                                              + " / " + String (info.slices.count)
                                                          : String (info.slices.count) + " SLICES")
                               + "   DBL-CLICK: ADD / DELETE";
                }
                if (lastReverse) how = "<< REVERSE   " + how;
            }
            g.setColour (c::amber);
            g.drawText (how, txt, Justification::bottomRight, false);
        }
        else
        {
            g.setColour (c::phosphor.withAlpha (0.8f));
            g.drawText ("CH 1 - MASTER OUT", txt, Justification::topLeft, false);

            String line1 = padName + " - " + SampleManager::statusText (info.status);
            String line2 = "DROP A WAV / AIFF HERE OR PRESS LOAD";
            if (info.status == SampleManager::Status::loading)
                line2 = "LOADING " + info.name.toUpperCase() + " ...";
            else if (info.status == SampleManager::Status::missing)
                line2 = "FILE NOT FOUND: " + info.file.getFullPathName();
            else if (info.status == SampleManager::Status::unreadable)
                line2 = "CANNOT READ: " + info.file.getFileName();
            else if (info.status == SampleManager::Status::tooLong)
                line2 = "LONGER THAN 5 MINUTES: " + info.file.getFileName();

            const bool problem = info.status == SampleManager::Status::missing
                              || info.status == SampleManager::Status::unreadable
                              || info.status == SampleManager::Status::tooLong;
            g.setColour (problem ? c::red : c::amber.withAlpha (0.9f));
            g.setFont (monoFont (13.0f));
            auto bottom = txt.removeFromBottom (34.0f);
            g.drawText (line1, bottom.removeFromTop (17.0f), Justification::bottomLeft, true);
            g.setColour (c::phosphor.withAlpha (0.6f));
            g.drawText (line2, bottom, Justification::bottomLeft, true);
        }
    }
}
