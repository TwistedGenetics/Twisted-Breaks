#include "UI/PanelPainter.h"
#include <cmath>

namespace tb::ui::paint
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        float rr (Random& r, float lo, float hi) { return lo + r.nextFloat() * (hi - lo); }

        Colour rustTone (Random& r)
        {
            const float t = r.nextFloat();
            return t < 0.4f ? c::rustDark : (t < 0.8f ? c::rust : c::rustBright);
        }

        /** An irregular blob path, used for paint chips and stains. */
        Path blob (Point<float> centre, float radius, Random& r, int points = 9)
        {
            Path p;
            for (int i = 0; i < points; ++i)
            {
                const float a  = MathConstants<float>::twoPi * (float) i / (float) points;
                const float rd = radius * rr (r, 0.45f, 1.0f);
                const auto  pt = centre.getPointOnCircumference (rd, a);
                if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
            }
            p.closeSubPath();
            return p.createPathWithRoundedCorners (radius * 0.3f);
        }
    }

    //==========================================================================
    void plate (Graphics& g, Rectangle<float> r, Colour base, float corner)
    {
        // Dark gap / shadow around the plate where it sits in the frame.
        g.setColour (c::grime.withAlpha (0.85f));
        g.fillRoundedRectangle (r.expanded (2.5f).translated (1.5f, 2.5f), corner + 2.0f);

        // Enamel face: light from above-left, slightly uneven.
        ColourGradient face (base.brighter (0.10f), r.getX(), r.getY(),
                             base.darker (0.25f),   r.getRight(), r.getBottom(), false);
        face.addColour (0.55, base);
        g.setGradientFill (face);
        g.fillRoundedRectangle (r, corner);

        // Bevel highlights / shadows.
        g.setColour (Colours::white.withAlpha (0.10f));
        g.drawLine (r.getX() + corner, r.getY() + 0.8f, r.getRight() - corner, r.getY() + 0.8f, 1.2f);
        g.drawLine (r.getX() + 0.8f, r.getY() + corner, r.getX() + 0.8f, r.getBottom() - corner, 1.0f);
        g.setColour (Colours::black.withAlpha (0.45f));
        g.drawLine (r.getX() + corner, r.getBottom() - 0.8f, r.getRight() - corner, r.getBottom() - 0.8f, 1.5f);
        g.drawLine (r.getRight() - 0.8f, r.getY() + corner, r.getRight() - 0.8f, r.getBottom() - corner, 1.2f);
    }

    void recess (Graphics& g, Rectangle<float> r, float corner)
    {
        g.setColour (Colours::black.withAlpha (0.7f));
        g.fillRoundedRectangle (r, corner);

        // Inner shadow from the top edge.
        ColourGradient shade (Colours::black.withAlpha (0.8f), r.getX(), r.getY(),
                              Colours::transparentBlack, r.getX(), r.getY() + 10.0f, false);
        g.setGradientFill (shade);
        g.fillRoundedRectangle (r, corner);

        g.setColour (Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (r.expanded (1.0f), corner + 1.0f, 1.0f);
    }

    void rivet (Graphics& g, Point<float> ctr, float radius, Random& r, bool allowDrip)
    {
        // Rust collar around the rivet where moisture sat for decades.
        if (r.nextFloat() < 0.75f)
        {
            ColourGradient halo (c::rust.withAlpha (rr (r, 0.25f, 0.6f)), ctr.x, ctr.y,
                                 c::rust.withAlpha (0.0f), ctr.x + radius * 2.6f, ctr.y, true);
            g.setGradientFill (halo);
            g.fillEllipse (Rectangle<float> (radius * 5.2f, radius * 5.2f).withCentre (ctr));
        }

        if (allowDrip && r.nextFloat() < 0.45f)
            rustDrip (g, ctr.translated (rr (r, -1.0f, 1.0f), radius * 0.6f), rr (r, 10.0f, 45.0f), r);

        auto body = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (ctr);
        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillEllipse (body.translated (0.8f, 1.2f));

        ColourGradient dome (c::bareMetal.brighter (0.2f), body.getX() + radius * 0.6f, body.getY() + radius * 0.5f,
                             c::steelDark, body.getRight(), body.getBottom(), true);
        g.setGradientFill (dome);
        g.fillEllipse (body);

        // Paint left on the rivet head (they were painted over with the panel).
        g.setColour (c::steelLight.withAlpha (rr (r, 0.3f, 0.7f)));
        g.fillEllipse (body.reduced (radius * 0.25f).translated (radius * 0.15f, radius * 0.15f));
    }

    void screw (Graphics& g, Point<float> ctr, float radius, float slotAngle)
    {
        auto body = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (ctr);
        g.setColour (Colours::black.withAlpha (0.5f));
        g.fillEllipse (body.translated (0.6f, 0.9f));

        ColourGradient head (c::bareMetal.brighter (0.3f), body.getX(), body.getY(),
                             c::bareMetal.darker (0.8f), body.getRight(), body.getBottom(), false);
        g.setGradientFill (head);
        g.fillEllipse (body);

        const auto d = Point<float> (std::cos (slotAngle), std::sin (slotAngle)) * (radius * 0.8f);
        g.setColour (Colours::black.withAlpha (0.8f));
        g.drawLine ({ ctr - d, ctr + d }, juce::jmax (1.0f, radius * 0.28f));
    }

    //==========================================================================
    void rustBloom (Graphics& g, Point<float> ctr, float radius, Random& r)
    {
        // Several overlapping irregular blobs, darkest in the middle.
        for (int i = 0; i < 4; ++i)
        {
            const auto off = Point<float> (rr (r, -0.4f, 0.4f), rr (r, -0.4f, 0.4f)) * radius;
            g.setColour (rustTone (r).withAlpha (rr (r, 0.08f, 0.22f)));
            g.fillPath (blob (ctr + off, radius * rr (r, 0.5f, 1.0f), r, 11));
        }

        // Pitting inside the bloom.
        for (int i = 0; i < (int) (radius * 0.8f); ++i)
        {
            const auto p = ctr + Point<float> (rr (r, -1.0f, 1.0f), rr (r, -1.0f, 1.0f)) * radius * 0.7f;
            g.setColour ((r.nextBool() ? c::rustDark : c::rustBright).withAlpha (rr (r, 0.2f, 0.55f)));
            const float s = rr (r, 0.6f, 2.0f);
            g.fillEllipse (p.x, p.y, s, s);
        }
    }

    void rustDrip (Graphics& g, Point<float> top, float length, Random& r)
    {
        // Tapered streak that fades downward, slightly wandering.
        Path drip;
        const float w = rr (r, 1.6f, 3.4f);
        const float wander = rr (r, -3.0f, 3.0f);
        drip.startNewSubPath (top.x - w * 0.5f, top.y);
        drip.quadraticTo (top.x - w * 0.4f + wander * 0.5f, top.y + length * 0.5f,
                          top.x + wander, top.y + length);
        drip.quadraticTo (top.x + w * 0.4f + wander * 0.5f, top.y + length * 0.5f,
                          top.x + w * 0.5f, top.y);
        drip.closeSubPath();

        ColourGradient grad (c::rust.withAlpha (rr (r, 0.45f, 0.75f)), top.x, top.y,
                             c::rustDark.withAlpha (0.0f), top.x, top.y + length, false);
        g.setGradientFill (grad);
        g.fillPath (drip);
    }

    void ageArea (Graphics& g, Rectangle<float> area, Random& r, float intensity)
    {
        const float areaK = area.getWidth() * area.getHeight() / 10000.0f;

        // 1. Grime speckle (dark) and dust (light).
        for (int i = 0; i < (int) (areaK * 45.0f * intensity); ++i)
        {
            const float x = rr (r, area.getX(), area.getRight());
            const float y = rr (r, area.getY(), area.getBottom());
            const float s = rr (r, 0.5f, 1.8f);
            g.setColour (r.nextFloat() < 0.7f ? Colours::black.withAlpha (rr (r, 0.08f, 0.3f))
                                              : c::bakelite.withAlpha (rr (r, 0.03f, 0.10f)));
            g.fillRect (x, y, s, s);
        }

        // 2. Water stains: faint rings with darker rims.
        for (int i = 0; i < (int) (areaK * 0.25f * intensity) + 1; ++i)
        {
            const Point<float> p (rr (r, area.getX(), area.getRight()), rr (r, area.getY(), area.getBottom()));
            const float rad = rr (r, 12.0f, 45.0f);
            auto stain = blob (p, rad, r, 13);
            g.setColour (Colour (0xff3a3220).withAlpha (rr (r, 0.05f, 0.12f)));
            g.fillPath (stain);
            g.setColour (Colour (0xff2a2015).withAlpha (rr (r, 0.10f, 0.22f)));
            g.strokePath (stain, PathStrokeType (rr (r, 0.6f, 1.4f)));
        }

        // 3. Scratches: fine bright lines (bare metal showing) and dark gouges.
        for (int i = 0; i < (int) (areaK * 3.0f * intensity); ++i)
        {
            const Point<float> a (rr (r, area.getX(), area.getRight()), rr (r, area.getY(), area.getBottom()));
            const float len = rr (r, 6.0f, 70.0f);
            const float ang = rr (r, 0.0f, MathConstants<float>::twoPi);
            const auto  b   = a.getPointOnCircumference (len, ang);
            Path s;
            s.startNewSubPath (a);
            s.quadraticTo ((a + b) * 0.5f + Point<float> (rr (r, -4.f, 4.f), rr (r, -4.f, 4.f)), b);
            g.setColour (r.nextFloat() < 0.75f ? c::bareMetal.withAlpha (rr (r, 0.06f, 0.22f))
                                               : Colours::black.withAlpha (rr (r, 0.15f, 0.35f)));
            g.strokePath (s, PathStrokeType (rr (r, 0.4f, 1.0f)));
        }

        // 4. Rust blooms, biased toward the lower part of the area (gravity, damp).
        for (int i = 0; i < (int) (areaK * 0.35f * intensity) + 1; ++i)
        {
            const float yBias = std::pow (r.nextFloat(), 0.45f);
            const Point<float> p (rr (r, area.getX(), area.getRight()),
                                  area.getY() + yBias * area.getHeight());
            rustBloom (g, p, rr (r, 6.0f, 26.0f), r);
        }
    }

    void edgeWear (Graphics& g, Rectangle<float> pb, Random& r, float intensity)
    {
        const int chips = (int) ((pb.getWidth() + pb.getHeight()) * 0.03f * intensity) + 2;
        for (int i = 0; i < chips; ++i)
        {
            // Pick a point on the perimeter; bottom edge and corners wear most.
            Point<float> p;
            const float t = r.nextFloat();
            if      (t < 0.35f) p = { rr (r, pb.getX(), pb.getRight()), pb.getBottom() - rr (r, 0.0f, 4.0f) };
            else if (t < 0.55f) p = { rr (r, pb.getX(), pb.getRight()), pb.getY() + rr (r, 0.0f, 3.0f) };
            else if (t < 0.78f) p = { pb.getX() + rr (r, 0.0f, 4.0f), rr (r, pb.getY(), pb.getBottom()) };
            else                p = { pb.getRight() - rr (r, 0.0f, 4.0f), rr (r, pb.getY(), pb.getBottom()) };

            const float size = rr (r, 2.0f, 9.0f);
            auto chip = blob (p, size, r);

            // Primer layer, then bare metal in the middle of larger chips.
            g.setColour (c::primer.withAlpha (rr (r, 0.55f, 0.9f)));
            g.fillPath (chip);
            if (size > 4.5f)
            {
                g.setColour (c::bareMetal.withAlpha (0.75f));
                g.fillPath (blob (p, size * 0.45f, r));
                if (r.nextFloat() < 0.5f)
                    rustBloom (g, p, size * 1.4f, r);
            }
        }
    }

    //==========================================================================
    void labelPlate (Graphics& g, Rectangle<float> rct, const String& text, const String& sub,
                     PlateStyle style, Random& r)
    {
        const bool bake = style == PlateStyle::bakelite;
        const Colour face = bake ? c::bakelite.interpolatedWith (c::bakeliteOld, rr (r, 0.1f, 0.6f))
                                 : c::plateBlack;

        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (rct.translated (1.0f, 1.5f), 2.0f);

        ColourGradient grad (face.brighter (0.08f), rct.getX(), rct.getY(),
                             face.darker (0.18f), rct.getX(), rct.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (rct, 2.0f);

        // Yellowing / dirt in the plate.
        for (int i = 0; i < (int) (rct.getWidth() * 0.8f); ++i)
        {
            g.setColour ((bake ? Colour (0xff5a4a2a) : Colours::white).withAlpha (rr (r, 0.03f, 0.10f)));
            g.fillRect (rr (r, rct.getX(), rct.getRight()), rr (r, rct.getY(), rct.getBottom()), 1.0f, 1.0f);
        }

        // Screws at each end.
        const float sr = juce::jmin (3.0f, rct.getHeight() * 0.16f);
        const bool  hasScrews = rct.getWidth() > rct.getHeight() * 2.5f;
        auto textArea = rct.reduced (hasScrews ? sr * 3.2f : 3.0f, 1.0f);
        if (hasScrews)
        {
            screw (g, { rct.getX() + sr * 1.8f,     rct.getCentreY() }, sr, rr (r, 0.f, 3.f));
            screw (g, { rct.getRight() - sr * 1.8f, rct.getCentreY() }, sr, rr (r, 0.f, 3.f));
        }

        // Engraved text: dark cut with a 1 px highlight below.
        const Colour ink = bake ? c::engraving : c::plateText;
        auto drawEngraved = [&] (const String& s, Rectangle<float> area, float h)
        {
            g.setFont (industrialFont (h));
            g.setColour (bake ? Colours::white.withAlpha (0.35f) : Colours::black);
            g.drawText (s, area.translated (0.0f, 0.8f), Justification::centred, true);
            g.setColour (ink);
            g.drawText (s, area, Justification::centred, true);
        };

        if (sub.isEmpty())
            drawEngraved (text, textArea, juce::jmin (rct.getHeight() * 0.62f, 22.0f));
        else
        {
            auto top = textArea.removeFromTop (textArea.getHeight() * 0.58f);
            drawEngraved (text, top, top.getHeight() * 0.95f);
            drawEngraved (sub, textArea, textArea.getHeight() * 0.9f);
        }

        // Chipped corner.
        if (r.nextFloat() < 0.4f)
        {
            g.setColour (c::steelDark);
            g.fillPath (blob (rct.getBottomRight().translated (-1.0f, -1.0f), rr (r, 2.0f, 4.0f), r, 6));
        }

        g.setColour (Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (rct, 2.0f, 0.8f);
    }

    void stencilText (Graphics& g, const String& text, Rectangle<float> rct, float height,
                      Justification just, Colour col, Random& r)
    {
        g.setFont (industrialFont (height).withExtraKerningFactor (0.08f));
        g.setColour (col.withAlpha (0.85f));
        g.drawText (text, rct, just, false);

        // Wear the paint: knock small holes out of the letters with the
        // panel colour so it reads as old stencilled paint, not fresh print.
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (rct.toNearestInt());
        for (int i = 0; i < (int) (rct.getWidth() * rct.getHeight() * 0.02f); ++i)
        {
            g.setColour (c::steel.withAlpha (rr (r, 0.4f, 0.9f)));
            const float sz = rr (r, 0.6f, 1.8f);
            g.fillEllipse (rr (r, rct.getX(), rct.getRight()), rr (r, rct.getY(), rct.getBottom()), sz, sz);
        }
    }

    void paperTag (Graphics& g, Rectangle<float> rct, float angle, const StringArray& lines, Random& r)
    {
        Graphics::ScopedSaveState s (g);
        g.addTransform (AffineTransform::rotation (angle, rct.getCentreX(), rct.getCentreY()));

        g.setColour (Colours::black.withAlpha (0.45f));
        g.fillRect (rct.translated (2.0f, 3.0f));

        ColourGradient paper (c::paperTag, rct.getX(), rct.getY(),
                              c::paperTag.darker (0.25f), rct.getRight(), rct.getBottom(), false);
        g.setGradientFill (paper);
        g.fillRect (rct);

        // Foxing / age spots.
        for (int i = 0; i < 18; ++i)
        {
            g.setColour (Colour (0xff8a6a30).withAlpha (rr (r, 0.05f, 0.18f)));
            const float sz = rr (r, 1.0f, 6.0f);
            g.fillEllipse (rr (r, rct.getX(), rct.getRight()), rr (r, rct.getY(), rct.getBottom()), sz, sz);
        }

        // Red ruled border, typewritten-style text.
        g.setColour (Colour (0xff8a1f14).withAlpha (0.8f));
        g.drawRect (rct.reduced (3.0f), 1.0f);

        auto inner = rct.reduced (6.0f, 5.0f);
        const float lh = inner.getHeight() / (float) juce::jmax (1, lines.size());
        for (int i = 0; i < lines.size(); ++i)
        {
            g.setColour (i == 0 ? Colour (0xff8a1f14) : Colour (0xff221d16));
            g.setFont (i == 0 ? industrialFont (lh * 0.95f) : monoFont (lh * 0.8f));
            g.drawText (lines[i], inner.removeFromTop (lh), Justification::centred, true);
        }

        // Yellowed sticky tape strips.
        g.setColour (Colour (0x66e8d68a));
        g.fillRect (Rectangle<float> (26.0f, 9.0f).withCentre (rct.getTopLeft().translated (8.0f, 2.0f)));
        g.fillRect (Rectangle<float> (26.0f, 9.0f).withCentre (rct.getTopRight().translated (-8.0f, 2.0f)));
    }

    void hazardStripes (Graphics& g, const Path& area, float stripeWidth)
    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (area);
        const auto b = area.getBounds();

        g.setColour (c::hazardYellow);
        g.fillRect (b);

        g.setColour (Colour (0xff121110));
        for (float x = b.getX() - b.getHeight(); x < b.getRight() + b.getHeight(); x += stripeWidth * 2.0f)
        {
            Path stripe;
            stripe.addQuadrilateral (x, b.getBottom(), x + stripeWidth, b.getBottom(),
                                     x + stripeWidth + b.getHeight(), b.getY(), x + b.getHeight(), b.getY());
            g.fillPath (stripe);
        }
    }

    //==========================================================================
    void lamp (Graphics& g, Point<float> ctr, float radius, Colour lit, Colour dim, float brightness)
    {
        brightness = jlimit (0.0f, 1.0f, brightness);

        // Bloom onto the surrounding panel when lit.
        if (brightness > 0.02f)
        {
            ColourGradient bloom (lit.withAlpha (0.45f * brightness), ctr.x, ctr.y,
                                  lit.withAlpha (0.0f), ctr.x + radius * 3.2f, ctr.y, true);
            g.setGradientFill (bloom);
            g.fillEllipse (Rectangle<float> (radius * 6.4f, radius * 6.4f).withCentre (ctr));
        }

        // Chrome bezel ring.
        auto bezel = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (ctr);
        ColourGradient ring (c::bareMetal.brighter (0.4f), bezel.getX(), bezel.getY(),
                             c::grime, bezel.getRight(), bezel.getBottom(), false);
        g.setGradientFill (ring);
        g.fillEllipse (bezel);

        // Glass lens.
        auto glass = bezel.reduced (radius * 0.22f);
        const Colour body = dim.interpolatedWith (lit, brightness);
        ColourGradient lens (body.brighter (0.3f + brightness * 0.6f), glass.getCentreX() - glass.getWidth() * 0.15f,
                             glass.getCentreY() - glass.getHeight() * 0.2f,
                             body.darker (0.6f - brightness * 0.4f), glass.getRight(), glass.getBottom(), true);
        g.setGradientFill (lens);
        g.fillEllipse (glass);

        // Hot filament core.
        if (brightness > 0.05f)
        {
            g.setColour (Colours::white.withAlpha (0.55f * brightness));
            g.fillEllipse (glass.reduced (glass.getWidth() * 0.34f));
        }

        // Specular reflection.
        g.setColour (Colours::white.withAlpha (0.22f));
        g.fillEllipse (glass.getX() + glass.getWidth() * 0.22f, glass.getY() + glass.getHeight() * 0.12f,
                       glass.getWidth() * 0.32f, glass.getHeight() * 0.22f);
    }

    void sevenSegment (Graphics& g, Rectangle<float> area, const String& text,
                       Colour lit, Colour ghost, int numDigits)
    {
        //  Segment bits:  a=1 b=2 c=4 d=8 e=16 f=32 g=64
        auto maskFor = [] (juce_wchar ch) -> int
        {
            switch (ch)
            {
                case '0': return 0x3f; case '1': return 0x06; case '2': return 0x5b;
                case '3': return 0x4f; case '4': return 0x66; case '5': return 0x6d;
                case '6': return 0x7d; case '7': return 0x07; case '8': return 0x7f;
                case '9': return 0x6f; case '-': return 0x40; case '_': return 0x08;
                case 'A': return 0x77; case 'b': return 0x7c; case 'C': return 0x39;
                case 'd': return 0x5e; case 'E': return 0x79; case 'F': return 0x71;
                case 'P': return 0x73; case 'S': return 0x6d; case 't': return 0x78;
                case 'o': return 0x5c; case 'n': return 0x54; case 'r': return 0x50;
                case 'L': return 0x38; case 'H': return 0x76; case 'U': return 0x3e;
                default:  return 0;
            }
        };

        // Split into digits with optional trailing decimal point each.
        struct Cell { int mask = 0; bool dp = false; };
        Array<Cell> cells;
        for (auto ch : text)
        {
            if (ch == '.' && ! cells.isEmpty()) { cells.getReference (cells.size() - 1).dp = true; continue; }
            cells.add ({ maskFor (ch), false });
        }
        while (cells.size() < numDigits) cells.insert (0, {});   // right-align
        while (cells.size() > numDigits) cells.remove (0);

        const float cellW  = area.getWidth() / (float) numDigits;
        const float h      = area.getHeight();
        const float w      = juce::jmin (cellW * 0.78f, h * 0.55f);
        const float t      = juce::jmax (1.5f, w * 0.17f);        // segment thickness
        const float skew   = h * 0.08f;                            // italic lean

        auto segPath = [&] (Point<float> p1, Point<float> p2) -> Path
        {
            // Hexagonal segment between p1 and p2.
            Path p;
            const auto dir  = (p2 - p1);
            const float len = dir.getDistanceFromOrigin();
            const auto u    = dir / len;
            const Point<float> n (-u.y, u.x);
            const float ht = t * 0.5f, gap = t * 0.15f;
            const auto a = p1 + u * gap, b = p2 - u * gap;
            p.startNewSubPath (a);
            p.lineTo (a + u * ht + n * ht);
            p.lineTo (b - u * ht + n * ht);
            p.lineTo (b);
            p.lineTo (b - u * ht - n * ht);
            p.lineTo (a + u * ht - n * ht);
            p.closeSubPath();
            return p;
        };

        for (int i = 0; i < numDigits; ++i)
        {
            const auto& cell = cells.getReference (i);
            const float x0 = area.getX() + cellW * (float) i + (cellW - w) * 0.5f;
            const float y0 = area.getY() + t * 0.5f;
            const float y1 = area.getCentreY();
            const float y2 = area.getBottom() - t * 0.5f;

            auto P = [&] (float x, float y)   // apply italic skew
            {
                const float k = (y2 - y) / (y2 - y0);
                return Point<float> (x + skew * k, y);
            };

            const float xl = x0 + t * 0.5f, xr = x0 + w - t * 0.5f;
            const Path segs[7] = {
                segPath (P (xl, y0), P (xr, y0)),   // a
                segPath (P (xr, y0), P (xr, y1)),   // b
                segPath (P (xr, y1), P (xr, y2)),   // c
                segPath (P (xl, y2), P (xr, y2)),   // d
                segPath (P (xl, y1), P (xl, y2)),   // e
                segPath (P (xl, y0), P (xl, y1)),   // f
                segPath (P (xl, y1), P (xr, y1)),   // g
            };

            for (int s = 0; s < 7; ++s)
            {
                const bool on = (cell.mask >> s) & 1;
                if (on)
                {
                    // Glow pass then crisp segment.
                    g.setColour (lit.withAlpha (0.25f));
                    g.strokePath (segs[s], PathStrokeType (t * 0.9f, PathStrokeType::curved, PathStrokeType::rounded));
                    g.setColour (lit);
                }
                else
                {
                    g.setColour (ghost);
                }
                g.fillPath (segs[s]);
            }

            const auto dpc = P (x0 + w + t * 0.6f, y2);
            g.setColour (cell.dp ? lit : ghost);
            g.fillEllipse (Rectangle<float> (t * 1.1f, t * 1.1f).withCentre (dpc));
        }
    }

    void glassCrack (Graphics& g, Rectangle<float> glass, Random& r)
    {
        // Impact point near an edge, with radiating fractures.
        const Point<float> origin (rr (r, glass.getX(), glass.getRight()),
                                   r.nextBool() ? glass.getY() + rr (r, 2.f, 14.f)
                                                : glass.getBottom() - rr (r, 2.f, 14.f));
        const int arms = 3 + r.nextInt (3);
        for (int i = 0; i < arms; ++i)
        {
            Path crack;
            auto p = origin;
            crack.startNewSubPath (p);
            float ang = rr (r, 0.0f, MathConstants<float>::twoPi);
            const int segs = 3 + r.nextInt (4);
            for (int s = 0; s < segs; ++s)
            {
                ang += rr (r, -0.5f, 0.5f);
                p = p.getPointOnCircumference (rr (r, 5.0f, 18.0f), ang);
                p = { jlimit (glass.getX(), glass.getRight(), p.x), jlimit (glass.getY(), glass.getBottom(), p.y) };
                crack.lineTo (p);
            }
            g.setColour (Colours::white.withAlpha (0.35f));
            g.strokePath (crack, PathStrokeType (0.7f));
            g.setColour (Colours::black.withAlpha (0.35f));
            g.strokePath (crack, PathStrokeType (0.6f), AffineTransform::translation (0.6f, 0.6f));
        }
    }
}
