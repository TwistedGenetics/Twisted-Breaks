#include "UI/ReactorLookAndFeel.h"
#include "UI/Palette.h"
#include "UI/PanelPainter.h"
#include <cmath>

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    ReactorLookAndFeel::ReactorLookAndFeel()
    {
        setColour (Slider::textBoxTextColourId,       c::vfd);
        setColour (Slider::textBoxBackgroundColourId, Colours::black);
        setColour (Slider::textBoxOutlineColourId,    Colours::transparentBlack);
        setColour (TooltipWindow::textColourId,       c::engraving);
        setColour (Label::textColourId,               c::plateText);
    }

    void ReactorLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                               float startAngle, float endAngle, Slider&)
    {
        const auto bounds = Rectangle<int> (x, y, w, h).toFloat();
        const float size  = jmin (bounds.getWidth(), bounds.getHeight());
        const auto  ctr   = bounds.getCentre();
        const float rad   = size * 0.5f;
        const float angle = startAngle + pos * (endAngle - startAngle);

        // Printed scale ticks on the panel (cream paint, part worn).
        for (int i = 0; i <= 10; ++i)
        {
            const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
            const bool major = (i % 5) == 0;
            const auto p1 = ctr.getPointOnCircumference (rad * 0.86f, a);
            const auto p2 = ctr.getPointOnCircumference (rad * (major ? 1.0f : 0.95f), a);
            g.setColour (c::bakelite.withAlpha (major ? 0.85f : 0.55f));
            g.drawLine ({ p1, p2 }, major ? 2.0f : 1.2f);
        }

        // Knurled skirt.
        const float skirtR = rad * 0.78f;
        auto skirt = Rectangle<float> (skirtR * 2.0f, skirtR * 2.0f).withCentre (ctr);
        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillEllipse (skirt.translated (2.0f, 3.5f));

        ColourGradient skirtGrad (Colour (0xff3a3835), skirt.getX(), skirt.getY(),
                                  Colour (0xff0b0b0a), skirt.getRight(), skirt.getBottom(), false);
        g.setGradientFill (skirtGrad);
        g.fillEllipse (skirt);

        g.setColour (Colours::white.withAlpha (0.08f));
        for (int i = 0; i < 48; ++i)
        {
            const float a = angle + MathConstants<float>::twoPi * (float) i / 48.0f;
            g.drawLine ({ ctr.getPointOnCircumference (skirtR * 0.86f, a),
                          ctr.getPointOnCircumference (skirtR * 0.99f, a) }, 1.0f);
        }

        // Bakelite cap with a soft top-light and a worn, shiny centre.
        const float capR = skirtR * 0.74f;
        auto cap = Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (ctr);
        ColourGradient capGrad (Colour (0xff4a4744), cap.getX() + capR * 0.4f, cap.getY() + capR * 0.3f,
                                Colour (0xff121110), cap.getRight(), cap.getBottom(), true);
        g.setGradientFill (capGrad);
        g.fillEllipse (cap);
        g.setColour (Colours::white.withAlpha (0.10f));
        g.drawEllipse (cap.reduced (1.0f), 1.0f);

        // Pointer line: inlaid cream paint.
        const auto tipOuter = ctr.getPointOnCircumference (skirtR * 0.95f, angle);
        const auto tipInner = ctr.getPointOnCircumference (capR * 0.25f, angle);
        g.setColour (Colours::black.withAlpha (0.6f));
        g.drawLine ({ tipInner.translated (0.5f, 1.0f), tipOuter.translated (0.5f, 1.0f) }, 3.4f);
        g.setColour (c::bakelite);
        g.drawLine ({ tipInner, tipOuter }, 2.8f);
    }

    void ReactorLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos,
                                               float, float, Slider::SliderStyle style, Slider& s)
    {
        if (style != Slider::LinearVertical && style != Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }

        const bool vert = style == Slider::LinearVertical;
        auto b = Rectangle<int> (x, y, w, h).toFloat();
        auto slot = vert ? b.withSizeKeepingCentre (6.0f, b.getHeight())
                         : b.withSizeKeepingCentre (b.getWidth(), 6.0f);
        paint::recess (g, slot, 3.0f);

        auto capB = vert ? Rectangle<float> (jmin (34.0f, b.getWidth()), 16.0f).withCentre ({ b.getCentreX(), pos })
                         : Rectangle<float> (16.0f, jmin (34.0f, b.getHeight())).withCentre ({ pos, b.getCentreY() });
        paint::plate (g, capB, c::bareMetal.darker (0.3f), 2.0f);
        g.setColour (c::engraving);
        if (vert) g.drawLine (capB.getX() + 3, capB.getCentreY(), capB.getRight() - 3, capB.getCentreY(), 1.5f);
        else      g.drawLine (capB.getCentreX(), capB.getY() + 3, capB.getCentreX(), capB.getBottom() - 3, 1.5f);
    }

    void ReactorLookAndFeel::drawTooltip (Graphics& g, const String& text, int w, int h)
    {
        auto r = Rectangle<int> (w, h).toFloat();
        g.setColour (c::paperTag);
        g.fillRect (r);
        g.setColour (Colour (0xff8a1f14).withAlpha (0.7f));
        g.drawRect (r.reduced (1.5f), 1.0f);
        g.setColour (c::engraving);
        g.setFont (monoFont (13.0f));
        g.drawFittedText (text, r.reduced (8.0f, 4.0f).toNearestInt(), Justification::centredLeft, 3);
    }

    Rectangle<int> ReactorLookAndFeel::getTooltipBounds (const String& text, Point<int> screenPos,
                                                         Rectangle<int> parentArea)
    {
        GlyphArrangement ga;
        ga.addLineOfText (monoFont (13.0f), text, 0.0f, 0.0f);
        const int w = jmin (360, (int) ga.getBoundingBox (0, -1, true).getWidth() + 22);
        const int h = w >= 360 ? 44 : 26;
        return Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                               screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6,
                               w, h).constrainedWithin (parentArea);
    }

    Label* ReactorLookAndFeel::createSliderTextBox (Slider& s)
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (monoFont (13.0f));
        l->setColour (Label::backgroundColourId, Colours::black.withAlpha (0.8f));
        l->setColour (Label::textColourId, c::vfd);
        l->setColour (Label::outlineColourId, Colours::transparentBlack);
        return l;
    }
}
