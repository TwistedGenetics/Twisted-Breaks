#include "UI/ScopeDisplay.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include <cmath>

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    ScopeDisplay::ScopeDisplay (EngineBridge& b) : bridge (b)
    {
        setInterceptsMouseClicks (false, false);
    }

    Rectangle<float> ScopeDisplay::screenArea() const
    {
        return getLocalBounds().toFloat().reduced (20.0f, 18.0f);
    }

    void ScopeDisplay::resized() { rebuildLayers(); }

    void ScopeDisplay::refresh()
    {
        const int n = bridge.pullScope (scratch.data(), (int) scratch.size());
        float energy = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            history[(size_t) writePos] = scratch[(size_t) i];
            writePos = (writePos + 1) % historySize;
            energy += std::abs (scratch[(size_t) i]);
        }

        const bool signal = energy > 1.0e-3f;
        if (n > 0 && (signal || hadSignal))
            repaint();
        hadSignal = signal;
    }

    //==========================================================================
    void ScopeDisplay::rebuildLayers()
    {
        const auto b = getLocalBounds();
        if (b.isEmpty()) return;

        const float s = 2.0f;
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

            // Phosphor screen: deep green-black with a faint centre bloom.
            ColourGradient tube (Colour (0xff0d2214), scr.getCentreX(), scr.getCentreY(),
                                 c::crtGlass, scr.getX(), scr.getY(), true);
            g.setGradientFill (tube);
            g.fillRoundedRectangle (scr, 12.0f);

            // Graticule: 10 x 8 divisions, brighter centre axes with sub-ticks.
            g.setColour (c::phosphor.withAlpha (0.10f));
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
            g.setColour (c::phosphor.withAlpha (0.22f));
            for (int i = 1; i < 50; ++i)
            {
                const float x = scr.getX() + scr.getWidth() * (float) i / 50.0f;
                g.drawLine (x, scr.getCentreY() - 2.5f, x, scr.getCentreY() + 2.5f, 0.8f);
            }
        }

        // ---- Over: scanlines, vignette, glass reflection, burn-in ----------------
        over = Image (Image::ARGB, roundToInt (b.getWidth() * s), roundToInt (b.getHeight() * s), true);
        {
            Graphics g (over);
            g.addTransform (AffineTransform::scale (s));
            Graphics::ScopedSaveState ss (g);
            Path screen; screen.addRoundedRectangle (scr, 12.0f);
            g.reduceClipRegion (screen);

            g.setColour (Colours::black.withAlpha (0.22f));
            for (float y = scr.getY(); y < scr.getBottom(); y += 3.0f)
                g.fillRect (scr.getX(), y, scr.getWidth(), 1.0f);

            ColourGradient vig (Colours::transparentBlack, scr.getCentreX(), scr.getCentreY(),
                                Colours::black.withAlpha (0.75f), scr.getX() - 10.0f, scr.getY() - 10.0f, true);
            g.setGradientFill (vig);
            g.fillRect (scr);

            // Old burn-in ghost of a test card, decades of the same image.
            g.setColour (c::phosphor.withAlpha (0.035f));
            g.drawEllipse (scr.withSizeKeepingCentre (scr.getHeight() * 0.8f, scr.getHeight() * 0.8f), 6.0f);

            // Curved glass highlight: a soft elliptical sheen, top-left.
            ColourGradient sheen (Colours::white.withAlpha (0.06f), scr.getX() + scr.getWidth() * 0.22f, scr.getY() + scr.getHeight() * 0.12f,
                                  Colours::white.withAlpha (0.0f), scr.getX() + scr.getWidth() * 0.55f, scr.getY() + scr.getHeight() * 0.5f, true);
            g.setGradientFill (sheen);
            g.fillRect (scr);
        }
    }

    void ScopeDisplay::paint (Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto scr    = screenArea().reduced (8.0f, 6.0f);

        g.drawImage (under, bounds);

        // ---- Trace -------------------------------------------------------------------
        // Find a rising zero crossing in the older half of the history so the
        // newest samples fill the screen and the waveform stays steady.
        const int window = 900;
        int start = (writePos - window - 1 + historySize) % historySize;
        for (int k = 0; k < 600; ++k)
        {
            const int i0 = (writePos - window - 1 - k + historySize * 2) % historySize;
            const int i1 = (i0 + 1) % historySize;
            if (history[(size_t) i0] <= 0.0f && history[(size_t) i1] > 0.0f) { start = i1; break; }
        }

        Path trace;
        const float midY = scr.getCentreY(), halfH = scr.getHeight() * 0.46f;
        for (int k = 0; k < window; ++k)
        {
            const float v = jlimit (-1.1f, 1.1f, history[(size_t) ((start + k) % historySize)]);
            const float x = scr.getX() + scr.getWidth() * (float) k / (float) (window - 1);
            const float y = midY - v * halfH;
            if (k == 0) trace.startNewSubPath (x, y); else trace.lineTo (x, y);
        }

        // Phosphor glow: wide faint stroke, medium, then the hot core.
        g.setColour (c::phosphor.withAlpha (0.10f));
        g.strokePath (trace, PathStrokeType (7.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c::phosphor.withAlpha (0.35f));
        g.strokePath (trace, PathStrokeType (2.8f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c::phosphor.brighter (0.6f));
        g.strokePath (trace, PathStrokeType (1.1f));

        g.drawImage (over, bounds);

        // ---- On-screen legends ---------------------------------------------------
        auto txt = screenArea().reduced (18.0f, 12.0f);
        g.setColour (c::phosphor.withAlpha (0.8f));
        g.setFont (monoFont (12.0f));
        g.drawText (cyr ("КАНАЛ 1 · ВЫХОД / MASTER OUT"), txt, Justification::topLeft, false);
        g.drawText ("16 ms/DIV", txt, Justification::topRight, false);
        g.setColour (c::phosphor.withAlpha (0.45f));
        g.drawText (cyr ("РЕДАКТОР ВОЛНЫ — ФАЗА 2 / WAVEFORM EDITOR: PHASE 2"), txt, Justification::bottomLeft, false);
    }
}
