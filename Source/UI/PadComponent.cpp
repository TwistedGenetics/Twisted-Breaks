#include "UI/PadComponent.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "Core/SampleManager.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    PadComponent::PadComponent (int padIndex) : index (padIndex)
    {
        setRepaintsOnMouseActivity (false);
        setTooltip ("Pad " + String (index + 1) + " - click to fire (higher = harder). Drop a WAV / AIFF here to load it.");
    }

    void PadComponent::setActivity (float level)
    {
        level = jlimit (0.0f, 1.0f, level);
        if (std::abs (level - activity) > 0.01f || (level <= 0.0f && activity > 0.0f))
        {
            activity = level;
            repaint();
        }
    }

    void PadComponent::setSelected (bool s)
    {
        if (s != selected) { selected = s; repaint(); }
    }

    void PadComponent::setMuteSolo (bool m, bool s, bool sil)
    {
        if (m != muted || s != soloed || sil != silenced)
        {
            muted = m; soloed = s; silenced = sil;
            repaint();
        }
    }

    void PadComponent::setQueued (bool q)
    {
        // Flash at ~4 Hz while queued (driven by the 30 Hz refresh).
        const bool flash = q && (Time::getMillisecondCounter() / 125) % 2 == 0;
        if (q != queued || flash != queuedFlash)
        {
            queued = q; queuedFlash = flash;
            repaint();
        }
    }

    void PadComponent::setLabels (const String& upper, const String& name, int midiNote, const String& newTag,
                                  bool isWarning, bool sampleLoaded)
    {
        upperLabel = upper; nameLabel = name; note = midiNote; tag = newTag;
        warning = isWarning; hasSample = sampleLoaded;
        repaint();
    }

    bool PadComponent::isInterestedInFileDrag (const StringArray& files)
    {
        for (auto& f : files)
            if (SampleManager::isSupportedFile (File (f))) return true;
        return false;
    }

    void PadComponent::fileDragEnter (const StringArray&, int, int) { fileHover = true;  repaint(); }
    void PadComponent::fileDragExit  (const StringArray&)           { fileHover = false; repaint(); }

    void PadComponent::filesDropped (const StringArray& files, int, int)
    {
        fileHover = false;
        repaint();
        for (auto& f : files)
            if (SampleManager::isSupportedFile (File (f)))
            {
                if (onSelect)      onSelect (index);
                if (onFileDropped) onFileDropped (index, File (f));
                return;
            }
    }

    Rectangle<float> PadComponent::glassArea() const
    {
        return getLocalBounds().toFloat().reduced (11.0f);
    }

    void PadComponent::resized()
    {
        rebuildOverlay();
    }

    //==========================================================================
    void PadComponent::rebuildOverlay()
    {
        const auto b = getLocalBounds();
        if (b.isEmpty()) return;

        const float scale = 2.0f;   // bake at 2x so it stays crisp on Retina
        overlay = Image (Image::ARGB, roundToInt (b.getWidth() * scale), roundToInt (b.getHeight() * scale), true);
        Graphics g (overlay);
        g.addTransform (AffineTransform::scale (scale));

        Random rnd (0x7B5 + (int64) index * 7919);
        const auto outer = b.toFloat().reduced (1.0f);
        const auto glass = glassArea();

        // Bezel: a steel frame with the glass cut out.
        Path frame;
        frame.addRoundedRectangle (outer, 4.0f);
        frame.addRoundedRectangle (glass, 2.0f);
        frame.setUsingNonZeroWinding (false);

        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillPath (frame, AffineTransform::translation (1.5f, 2.5f));
        ColourGradient bez (c::steelEdge, outer.getX(), outer.getY(),
                            c::steelDark, outer.getRight(), outer.getBottom(), false);
        g.setGradientFill (bez);
        g.fillPath (frame);

        {
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (frame);
            paint::ageArea (g, outer, rnd, 1.6f);
            paint::edgeWear (g, outer, rnd, 1.4f);
        }

        // Inner lip shadow onto the glass.
        g.setColour (Colours::black.withAlpha (0.65f));
        g.drawRoundedRectangle (glass.expanded (0.5f), 2.0f, 2.0f);

        // Four screws hold the tile in.
        const float sr = 3.2f;
        for (auto p : { outer.getTopLeft().translated (6, 6),  outer.getTopRight().translated (-6, 6),
                        outer.getBottomLeft().translated (6, -6), outer.getBottomRight().translated (-6, -6) })
            paint::screw (g, p, sr, rnd.nextFloat() * 3.0f);

        // Grime on the glass: heavier in the corners, finger-worn centre.
        {
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (glass.toNearestInt());
            for (int i = 0; i < 260; ++i)
            {
                const float x = glass.getX() + rnd.nextFloat() * glass.getWidth();
                const float y = glass.getY() + rnd.nextFloat() * glass.getHeight();
                const float dx = std::abs (x - glass.getCentreX()) / glass.getWidth();
                const float dy = std::abs (y - glass.getCentreY()) / glass.getHeight();
                const float edge = jmax (dx, dy);                // 0 centre .. 0.5 edge
                g.setColour (Colour (0xff1a140c).withAlpha (0.04f + edge * 0.35f * rnd.nextFloat()));
                const float sz = 0.8f + rnd.nextFloat() * 2.2f;
                g.fillEllipse (x, y, sz, sz);
            }

            ColourGradient vignette (Colours::transparentBlack, glass.getCentreX(), glass.getCentreY(),
                                     Colour (0xff120d06).withAlpha (0.55f), glass.getX(), glass.getY(), true);
            g.setGradientFill (vignette);
            g.fillRect (glass);

            // Some tiles have taken a knock over the years.
            if (index == 5 || index == 12 || index == 14)
                paint::glassCrack (g, glass, rnd);

            // Glass sheen across the top.
            ColourGradient sheen (Colours::white.withAlpha (0.10f), glass.getX(), glass.getY(),
                                  Colours::white.withAlpha (0.0f), glass.getX(), glass.getY() + glass.getHeight() * 0.45f, false);
            g.setGradientFill (sheen);
            g.fillRect (glass.withHeight (glass.getHeight() * 0.45f));
        }
    }

    //==========================================================================
    void PadComponent::paint (Graphics& g)
    {
        const auto glass = glassArea();
        const float lit = jmax (activity, pressed ? 0.85f : 0.0f);

        // 1. Backlit frosted glass. Unlit glass is a murky amber-brown.
        const Colour base = c::amberDim.interpolatedWith (c::amber, lit);
        ColourGradient face (base.brighter (0.25f * lit + 0.05f), glass.getCentreX(), glass.getCentreY() - glass.getHeight() * 0.1f,
                             base.darker (0.55f - 0.25f * lit), glass.getX(), glass.getBottom(), true);
        g.setGradientFill (face);
        g.fillRect (glass);

        // 2. Engraved legend. Dark when lit (like a real annunciator), faint cream when dark.
        const Colour ink = lit > 0.25f ? Colour (0xff2a1704).withAlpha (0.9f)
                                       : c::bakelite.withAlpha (0.38f);
        auto t = glass.reduced (8.0f, 5.0f);

        g.setColour (ink);
        g.setFont (industrialFont (22.0f));
        g.drawText (String (index + 1).paddedLeft ('0', 2), t.removeFromTop (22.0f),
                    Justification::centredLeft, false);

        g.setColour (warning ? (lit > 0.25f ? Colour (0xff7a0c02) : c::red.withAlpha (0.9f)) : ink);
        g.setFont (industrialFont (11.0f, false));
        g.drawText (upperLabel, t.removeFromTop (12.0f), Justification::centredLeft, true);
        g.setColour (ink.withMultipliedAlpha (hasSample || lit > 0.25f ? 1.6f : 1.0f));
        g.setFont (industrialFont (15.0f));
        g.drawText (nameLabel, t.removeFromTop (16.0f), Justification::centredLeft, true);
        g.setColour (ink);

        auto footer = t.removeFromBottom (14.0f);
        g.setFont (monoFont (11.0f));
        const String noteText = note >= 0 ? MidiMessage::getMidiNoteName (note, true, true, 3) + " " + String (note)
                                          : String ("--");
        g.drawText (noteText, footer, Justification::centredLeft, false);
        g.drawText (tag, footer, Justification::centredRight, false);

        // 3. Static overlay (bezel, grime, cracks).
        g.drawImage (overlay, getLocalBounds().toFloat());

        // 4. Selection: green witness lamp in the top-right of the bezel.
        paint::lamp (g, { (float) getWidth() - 18.0f, 18.0f }, 4.2f,
                     c::green, c::greenDim, selected ? 1.0f : 0.0f);

        // 5. Mute / solo: muted or solo-silenced pads go dark behind smoked
        //    glass; small M / S lamps on the bezel show which is which.
        if (silenced)
        {
            g.setColour (Colours::black.withAlpha (0.5f));
            g.fillRect (glass);
        }
        if (muted)  paint::lamp (g, { 18.0f, (float) getHeight() - 18.0f }, 4.2f, c::red,   c::redDim,   1.0f);
        if (soloed) paint::lamp (g, { (float) getWidth() - 18.0f, (float) getHeight() - 18.0f }, 4.2f, c::green, c::greenDim, 1.0f);

        // 6a. SWAP queued: flashing amber frame until the hand-off lands.
        if (queuedFlash)
        {
            g.setColour (c::amber);
            g.drawRoundedRectangle (glass.expanded (2.0f), 3.0f, 2.5f);
        }

        // 6. A file is being dragged over this pad: amber outline.
        if (fileHover)
        {
            g.setColour (c::amber);
            g.drawRoundedRectangle (glass.expanded (2.0f), 3.0f, 3.0f);
        }

        // 7. Pressed: the whole tile sinks a little.
        if (pressed)
        {
            g.setColour (Colours::black.withAlpha (0.18f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);
        }
    }

    void PadComponent::mouseDown (const MouseEvent& e)
    {
        pressed = true;
        const float rel = jlimit (0.0f, 1.0f, (float) e.y / (float) jmax (1, getHeight()));
        const float velocity = jmap (rel, 1.0f, 0.35f);

        if (onSelect) onSelect (index);
        if (onPress)  onPress (index, velocity);
        repaint();
    }

    void PadComponent::mouseUp (const MouseEvent&)
    {
        pressed = false;
        if (onRelease) onRelease (index);
        repaint();
    }
}
