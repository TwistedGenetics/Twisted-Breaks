#include "UI/FXGrid.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    const StringArray& FXGrid::effectNames()
    {
        static const StringArray names {
            "MUTE",    "LPF",    "HPF",   "BPF",
            "CRUSH",   "RATE",   "DRIVE", "TAPE",
            "STUTTER", "REPEAT", "GATE",  "PITCH",
            "DELAY",   "REVERB", "COMB",  "REVERSE" };
        return names;
    }

    FXGrid::FXGrid()
    {
        setTooltip ("Performance FX grid - connected in Phase 6");
    }

    void FXGrid::resized()
    {
        const auto b = getLocalBounds();
        if (b.isEmpty()) return;

        const float s = 2.0f;
        cache = Image (Image::ARGB, roundToInt (b.getWidth() * s), roundToInt (b.getHeight() * s), true);
        Graphics g (cache);
        g.addTransform (AffineTransform::scale (s));
        Random rnd (606);

        const auto area = b.toFloat();
        const float gap = 6.0f;
        const float kw  = (area.getWidth()  - gap * 3.0f) / 4.0f;
        const float kh  = (area.getHeight() - gap * 3.0f) / 4.0f;

        for (int i = 0; i < 16; ++i)
        {
            Rectangle<float> key (area.getX() + (float) (i % 4) * (kw + gap),
                                  area.getY() + (float) (i / 4) * (kh + gap), kw, kh);

            // Square black push-key in a steel collar, lamp window on top.
            paint::plate (g, key, Colour (0xff1e1f1d), 3.0f);
            {
                Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (key.toNearestInt());
                paint::ageArea (g, key, rnd, 0.9f);
            }

            auto lampWin = key.reduced (6.0f).removeFromTop (8.0f);
            g.setColour (c::amberDim.darker (0.4f));
            g.fillRoundedRectangle (lampWin, 2.0f);

            g.setColour (c::plateText.withAlpha (0.45f));
            g.setFont (industrialFont (jmin (15.0f, kh * 0.32f)));
            g.drawFittedText (effectNames()[i], key.reduced (4.0f).withTrimmedTop (12.0f).toNearestInt(),
                              Justification::centred, 1, 0.6f);
        }

        // The tag that says it all.
        paint::paperTag (g, area.withSizeKeepingCentre (area.getWidth() * 0.78f, 58.0f).translated (0.0f, -6.0f),
                         -0.06f,
                         { "NOT CONNECTED", "OFFLINE - PHASE 6" }, rnd);
    }

    void FXGrid::paint (Graphics& g)
    {
        g.drawImage (cache, getLocalBounds().toFloat());
    }
}
