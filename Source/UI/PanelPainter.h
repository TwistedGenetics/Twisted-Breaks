#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Palette.h"

/*
    PanelPainter
    ------------
    Procedural drawing for the aged control-room skin. No bitmap assets: the
    rust, paint wear, scratches and grime are all generated from a seeded
    juce::Random so the panel looks the same every time it opens, yet every
    rivet and plate is subtly different.

    The expensive "ageing" work is meant to be baked ONCE into a cached
    background image (see MainView::rebuildBackground) — never in a
    per-frame paint().
*/
namespace tb::ui::paint
{
    using juce::Graphics;
    using juce::Rectangle;
    using juce::Point;
    using juce::Colour;
    using juce::Random;

    // ---- Structure -------------------------------------------------------------
    /** Raised enamelled steel plate with bevel and a dark gap around it. */
    void plate (Graphics&, Rectangle<float> r, Colour base, float corner = 3.0f);

    /** A recessed area (sunken bezel) e.g. behind displays. */
    void recess (Graphics&, Rectangle<float> r, float corner = 4.0f);

    /** Domed rivet with rust ring; optionally leaves a rust drip below it. */
    void rivet (Graphics&, Point<float> c, float radius, Random&, bool allowDrip = true);

    /** Slotted screw head. */
    void screw (Graphics&, Point<float> c, float radius, float slotAngle);

    // ---- Ageing ----------------------------------------------------------------
    /** Grime speckle, scratches, water stains and rust over an area. */
    void ageArea (Graphics&, Rectangle<float> area, Random&, float intensity);

    /** Paint worn through to primer / bare steel along a plate's edges. */
    void edgeWear (Graphics&, Rectangle<float> plateBounds, Random&, float intensity);

    void rustBloom (Graphics&, Point<float> c, float radius, Random&);
    void rustDrip  (Graphics&, Point<float> top, float length, Random&);

    // ---- Labels ----------------------------------------------------------------
    enum class PlateStyle { bakelite, black };

    /** Engraved label plate held by two screws. `sub` is a smaller second line. */
    void labelPlate (Graphics&, Rectangle<float> r, const juce::String& text,
                     const juce::String& sub, PlateStyle, Random&);

    /** Paint-stencilled text straight onto the steel (slightly worn). */
    void stencilText (Graphics&, const juce::String& text, Rectangle<float> r,
                      float height, juce::Justification, Colour, Random&);

    /** A yellowed paper tag taped onto the panel, rotated a little. */
    void paperTag (Graphics&, Rectangle<float> r, float angleRadians,
                   const juce::StringArray& lines, Random&);

    void hazardStripes (Graphics&, const juce::Path& area, float stripeWidth);

    // ---- Indicators ------------------------------------------------------------
    /** Incandescent lamp in a chrome bezel. brightness 0 = off, 1 = full. */
    void lamp (Graphics&, Point<float> c, float radius, Colour lit, Colour dim, float brightness);

    /** Seven-segment readout; unlit segments faintly visible like real tubes. */
    void sevenSegment (Graphics&, Rectangle<float> area, const juce::String& text,
                       Colour lit, Colour ghost, int numDigits);

    /** Fine crack lines across glass. */
    void glassCrack (Graphics&, Rectangle<float> glass, Random&);
}
