#include "UI/NeedleMeter.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include <cmath>

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        constexpr float kMinDb = -30.0f, kMaxDb = 3.0f;
        constexpr float kSweep = 1.75f;   // radians of needle travel

        Rectangle<float> dialArea (Rectangle<float> b) { return b.withTrimmedBottom (34.0f).reduced (4.0f); }
        float radiusOf (Rectangle<float> dial) { return dial.getWidth() * 0.40f; }
        Point<float> pivotOf (Rectangle<float> dial) { return { dial.getCentreX(), dial.getY() + 24.0f + radiusOf (dial) }; }
    }

    NeedleMeter::NeedleMeter (EngineBridge& b) : bridge (b)
    {
        setInterceptsMouseClicks (false, false);
        lastClip = bridge.clipCount.load();
    }

    float NeedleMeter::angleFor (float db) const noexcept
    {
        // Scale is expanded toward the top like a real VU: most of the arc
        // is spent on the last 10 dB where mixing decisions happen.
        const float t = std::pow (jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb)), 1.6f);
        return -kSweep * 0.5f + t * kSweep;
    }

    void NeedleMeter::refresh()
    {
        const float peak = jmax (bridge.peakLeft.exchange (0.0f), bridge.peakRight.exchange (0.0f));
        const float db   = Decibels::gainToDecibels (peak, -60.0f);

        // Ballistics: quick rise, slow fall, like a damped coil.
        const float coeff = db > needleDb ? 0.55f : 0.10f;
        const float before = needleDb;
        needleDb += (db - needleDb) * coeff;

        const bool lim = bridge.limiterReductionDb.load() > 0.5f;
        const auto clips = bridge.clipCount.load();
        if (clips != lastClip) { overloadHold = 45.0f; lastClip = clips; }
        else if (overloadHold > 0.0f) overloadHold -= 1.0f;

        const bool overloadOn = overloadHold > 0.0f;
        if (std::abs (needleDb - before) > 0.05f || lim != limiting || overloadOn != overloadShown)
        {
            limiting      = lim;
            overloadShown = overloadOn;
            repaint();
        }
    }

    void NeedleMeter::resized() { rebuildFace(); }

    void NeedleMeter::rebuildFace()
    {
        const auto b = getLocalBounds();
        if (b.isEmpty()) return;
        const float s = 2.0f;
        face = Image (Image::ARGB, roundToInt (b.getWidth() * s), roundToInt (b.getHeight() * s), true);
        Graphics g (face);
        g.addTransform (AffineTransform::scale (s));
        Random rnd (4242);

        const auto bounds = b.toFloat();
        const auto dial   = dialArea (bounds);

        // Bezel + paper face, yellowed and water-stained.
        paint::recess (g, dial.expanded (4.0f), 5.0f);
        ColourGradient paper (Colour (0xffe2d6b0), dial.getCentreX(), dial.getY(),
                              Colour (0xffb9a674), dial.getCentreX(), dial.getBottom(), false);
        g.setGradientFill (paper);
        g.fillRoundedRectangle (dial, 3.0f);
        {
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (dial.toNearestInt());
            for (int i = 0; i < 300; ++i)
            {
                g.setColour (Colour (0xff6a5430).withAlpha (0.03f + rnd.nextFloat() * 0.08f));
                const float sz = 0.6f + rnd.nextFloat() * 2.0f;
                g.fillEllipse (dial.getX() + rnd.nextFloat() * dial.getWidth(),
                               dial.getY() + rnd.nextFloat() * dial.getHeight(), sz, sz);
            }
            ColourGradient stain (Colour (0x30704a18), dial.getRight() - 20.0f, dial.getY() + 10.0f,
                                  Colour (0x00704a18), dial.getRight() - 60.0f, dial.getY() + 40.0f, true);
            g.setGradientFill (stain);
            g.fillRect (dial);
        }

        // Scale arc, red zone, ticks and numbers.
        const auto pivot = pivotOf (dial);
        const float rad  = radiusOf (dial);

        auto arcPath = [&] (float db1, float db2, float r)
        {
            Path p;
            p.addCentredArc (pivot.x, pivot.y, r, r, 0.0f, angleFor (db1), angleFor (db2), true);
            return p;
        };

        g.setColour (c::engraving);
        g.strokePath (arcPath (kMinDb, 0.0f, rad), PathStrokeType (1.4f));
        g.setColour (Colour (0xffb3200e));
        g.strokePath (arcPath (0.0f, kMaxDb, rad), PathStrokeType (4.5f));
        g.strokePath (arcPath (0.0f, kMaxDb, rad - 3.0f), PathStrokeType (1.0f));

        for (int idb : { -30, -20, -15, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3 })
        {
            const float db = (float) idb;
            const float a  = angleFor (db);
            const bool major = idb == -30 || idb == -20 || idb == -10 || idb == -5 || idb == -3 || idb == 0 || idb == 3;
            const auto p1 = pivot.getPointOnCircumference (rad, a);
            const auto p2 = pivot.getPointOnCircumference (rad + (major ? 8.0f : 4.5f), a);
            g.setColour (db > 0.0f ? Colour (0xffb3200e) : c::engraving);
            g.drawLine ({ p1, p2 }, major ? 1.5f : 1.0f);

            if (major)
            {
                const auto tp = pivot.getPointOnCircumference (rad + 15.0f, a);
                g.setFont (industrialFont (11.0f));
                g.drawText (idb > 0 ? "+" + String (idb) : String (idb),
                            Rectangle<float> (28.0f, 14.0f).withCentre (tp), Justification::centred, false);
            }
        }

        g.setColour (c::engraving);
        g.setFont (industrialFont (13.0f));
        g.drawText ("LEVEL  dB", dial.withTrimmedTop (dial.getHeight() * 0.5f).withTrimmedBottom (4.0f),
                    Justification::centredBottom, false);
        g.setFont (monoFont (8.0f));
        g.drawText ("MODEL M4200", dial.reduced (6.0f, 4.0f), Justification::bottomLeft, false);
    }

    void NeedleMeter::paint (Graphics& g)
    {
        auto bounds = getLocalBounds().toFloat();
        const auto dial = dialArea (bounds);

        g.drawImage (face, bounds);

        // Needle with a slight shadow onto the paper.
        {
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (dial.toNearestInt());
            const auto pivot = pivotOf (dial);
            const float a    = angleFor (needleDb);
            const auto tip   = pivot.getPointOnCircumference (radiusOf (dial) + 6.0f, a);

            g.setColour (Colours::black.withAlpha (0.25f));
            g.drawLine ({ pivot.translated (3.0f, 2.0f), tip.translated (3.0f, 2.0f) }, 1.6f);
            g.setColour (Colour (0xff141210));
            g.drawLine ({ pivot, tip }, 1.6f);

            // Glass reflection across the window.
            ColourGradient refl (Colours::white.withAlpha (0.16f), dial.getX(), dial.getY(),
                                 Colours::white.withAlpha (0.0f), dial.getX() + dial.getWidth() * 0.6f, dial.getBottom(), false);
            g.setGradientFill (refl);
            Path glare;
            glare.addTriangle (dial.getTopLeft(), dial.getTopRight().withX (dial.getX() + dial.getWidth() * 0.7f),
                               dial.getBottomLeft().withY (dial.getY() + dial.getHeight() * 0.7f));
            g.fillPath (glare);
        }

        // Status lamps under the meter.
        auto lamps = bounds.removeFromBottom (32.0f);
        auto left  = lamps.removeFromLeft (lamps.getWidth() * 0.5f);
        auto lampRow = [&] (Rectangle<float> r, Colour lit, Colour dim, float on, const String& cap)
        {
            paint::lamp (g, { r.getX() + 16.0f, r.getCentreY() }, 7.5f, lit, dim, on);
            g.setColour (c::bakelite.withAlpha (0.85f));
            g.setFont (industrialFont (12.5f));
            g.drawText (cap, r.withTrimmedLeft (30.0f), Justification::centredLeft, false);
        };
        lampRow (left,  c::amber, c::amberDim, limiting ? 1.0f : 0.0f,                    "LIMIT");
        lampRow (lamps, c::red,   c::redDim,   overloadHold > 0.0f ? 1.0f : 0.0f, "CLIP");
    }
}
