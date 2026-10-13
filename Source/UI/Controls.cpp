#include "UI/Controls.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    //==========================================================================
    //  ToggleSwitch
    //==========================================================================
    ToggleSwitch::ToggleSwitch (const String& captionText, const String& tooltip)
        : Button (captionText), caption (captionText)
    {
        setClickingTogglesState (true);
        setTooltip (tooltip);
    }

    void ToggleSwitch::paintButton (Graphics& g, bool, bool)
    {
        const auto b   = getLocalBounds().toFloat();
        const bool on  = getToggleState();
        auto plateR    = b.withSizeKeepingCentre (b.getWidth() - 6.0f, b.getHeight() - 22.0f).withY (b.getY() + 2.0f);

        paint::plate (g, plateR, c::plateBlack, 3.0f);
        g.setColour (c::plateText);
        g.setFont (industrialFont (11.0f));
        g.drawText (String ("ON"),  plateR.removeFromTop (14.0f), Justification::centred, false);
        g.drawText (String ("OFF"), plateR.removeFromBottom (14.0f), Justification::centred, false);

        // Nut and bat handle.
        const auto ctr = plateR.getCentre();
        auto nut = Rectangle<float> (18.0f, 18.0f).withCentre (ctr);
        ColourGradient nutG (c::bareMetal.brighter (0.4f), nut.getX(), nut.getY(),
                             c::bareMetal.darker (0.7f), nut.getRight(), nut.getBottom(), false);
        g.setGradientFill (nutG);
        g.fillEllipse (nut);

        const float dir = on ? -1.0f : 1.0f;
        const auto tip  = ctr.translated (0.0f, dir * 16.0f);
        g.setColour (Colours::black.withAlpha (0.5f));
        g.drawLine ({ ctr.translated (2.0f, 2.0f), tip.translated (2.0f, 2.0f) }, 6.0f);
        ColourGradient bat (Colours::white.withAlpha (0.9f), ctr.x - 3.0f, ctr.y,
                            c::bareMetal.darker (0.5f), ctr.x + 3.0f, ctr.y, false);
        g.setGradientFill (bat);
        g.drawLine ({ ctr, tip }, 5.5f);
        g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (tip));

        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (11.5f));
        g.drawText (caption, b.withTop (b.getBottom() - 16.0f), Justification::centred, false);
    }

    //==========================================================================
    //  PushKey
    //==========================================================================
    PushKey::PushKey (const String& label, const String& tooltip) : Button (label)
    {
        setTooltip (tooltip);
    }

    void PushKey::paintButton (Graphics& g, bool highlighted, bool down)
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        if (down) b.translate (0.0f, 1.0f);

        paint::plate (g, b, down ? Colour (0xff2c2e28) : Colour (0xff23251f), 3.0f);
        paint::lamp (g, { b.getX() + 12.0f, b.getCentreY() }, 5.0f, c::amber, c::amberDim,
                     down ? 1.0f : (highlighted ? 0.35f : 0.0f));

        g.setColour (c::plateText.withAlpha (isEnabled() ? 0.95f : 0.35f));
        g.setFont (industrialFont (14.0f));
        g.drawText (getButtonText(), b.withTrimmedLeft (20.0f), Justification::centred, false);
    }
}
