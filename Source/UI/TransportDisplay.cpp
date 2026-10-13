#include "UI/TransportDisplay.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include <cmath>

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    TransportDisplay::TransportDisplay (const TransportUiState& s) : ui (s)
    {
        setInterceptsMouseClicks (false, false);
    }

    void TransportDisplay::refresh()
    {
        const bool   h  = ui.hostProvidesInfo.load (std::memory_order_relaxed);
        const bool   pl = ui.isPlaying.load (std::memory_order_relaxed);
        const double b  = ui.bpm.load (std::memory_order_relaxed);
        const double q  = ui.ppq.load (std::memory_order_relaxed);
        const int    br = ui.bar.load (std::memory_order_relaxed);
        const int    bt = ui.beat.load (std::memory_order_relaxed);
        const int    st = ui.step.load (std::memory_order_relaxed);
        const int    n  = ui.timeSigNum.load (std::memory_order_relaxed);
        const int    d  = ui.timeSigDen.load (std::memory_order_relaxed);

        if (h != host || pl != playing || std::abs (b - bpm) > 0.005 || std::abs (q - ppq) > 1.0e-4
            || br != bar || bt != beat || st != step || n != num || d != den)
        {
            host = h; playing = pl; bpm = b; ppq = q; bar = br; beat = bt; step = st; num = n; den = d;
            repaint();
        }
    }

    void TransportDisplay::paint (Graphics& g)
    {
        auto area = getLocalBounds().toFloat();
        auto top  = area.removeFromTop (area.getHeight() * 0.62f);

        // ---- Segment windows ---------------------------------------------------
        auto window = [&] (Rectangle<float> r, const String& text, int digits, const String& caption)
        {
            g.setColour (c::bakelite.withAlpha (0.75f));
            g.setFont (industrialFont (12.0f));
            g.drawText (caption, r.removeFromTop (15.0f), Justification::centredLeft, false);

            paint::recess (g, r, 4.0f);
            auto glass = r.reduced (8.0f, 7.0f);
            g.setColour (Colour (0xff120804));
            g.fillRect (glass);
            paint::sevenSegment (g, glass, text, c::vfd, c::vfdGhost, digits);

            // Smoked glass reflection.
            g.setColour (Colours::white.withAlpha (0.05f));
            g.fillRect (glass.withHeight (glass.getHeight() * 0.4f));
        };

        const float w = top.getWidth();
        auto bpmArea  = top.removeFromLeft (w * 0.33f).reduced (4.0f, 0.0f);
        auto posArea  = top.removeFromLeft (w * 0.42f).reduced (4.0f, 0.0f);
        auto lampArea = top.reduced (6.0f, 0.0f);

        window (bpmArea, String (bpm, 1), 4, cyr ("ТЕМП / BPM"));

        // Bar . beat . 16th, all 1-based like Ableton's own display.
        const double beatLen   = 4.0 / (double) jmax (1, den);
        const int    sixteenth = (int) std::floor (std::fmod (std::max (0.0, ppq), beatLen) / 0.25 + 1.0e-6) + 1;
        const String pos = String (bar + 1).paddedLeft ('0', 3) + "." + String (beat + 1) + "." + String (sixteenth);
        window (posArea, pos, 5, cyr ("ТАКТ / BAR . BEAT . 16"));

        // ---- Lamps: RUN / HOST, and the time signature on a plate -------------
        g.setColour (c::bakelite.withAlpha (0.75f));
        g.setFont (industrialFont (12.0f));
        g.drawText (cyr ("СОСТОЯНИЕ"), lampArea.removeFromTop (15.0f), Justification::centredLeft, false);

        auto lamps = lampArea.reduced (2.0f, 4.0f);
        auto runCell  = lamps.removeFromLeft (lamps.getWidth() / 3.0f);
        auto hostCell = lamps.removeFromLeft (lamps.getWidth() / 2.0f);
        auto sigCell  = lamps;

        auto lampWithCaption = [&] (Rectangle<float> cell, Colour lit, Colour dim, float on, const String& cap)
        {
            paint::lamp (g, { cell.getCentreX(), cell.getY() + 16.0f }, 10.0f, lit, dim, on);
            g.setColour (c::bakelite.withAlpha (0.8f));
            g.setFont (industrialFont (12.0f));
            g.drawText (cap, cell.withTrimmedTop (32.0f), Justification::centredTop, false);
        };
        lampWithCaption (runCell,  c::green, c::greenDim, playing ? 1.0f : 0.0f, cyr ("ХОД"));
        lampWithCaption (hostCell, c::amber, c::amberDim, host ? 0.8f : 0.0f, cyr ("СИНХР"));

        paint::recess (g, sigCell.withSizeKeepingCentre (sigCell.getWidth() - 6.0f, 34.0f).translated (0, -2), 3.0f);
        g.setColour (c::vfd);
        g.setFont (monoFont (17.0f));
        g.drawText (String (num) + "/" + String (den), sigCell.withSizeKeepingCentre (sigCell.getWidth(), 34.0f).translated (0, -2),
                    Justification::centred, false);
        g.setColour (c::bakelite.withAlpha (0.8f));
        g.setFont (industrialFont (12.0f));
        g.drawText (cyr ("МЕТР"), sigCell.withTrimmedTop (32.0f), Justification::centredTop, false);

        // ---- 32-step playhead lamp row ------------------------------------------
        area.removeFromTop (6.0f);
        auto row = area.reduced (4.0f, 0.0f);
        const float cellW = row.getWidth() / 32.0f;
        const float lampR = jmin (cellW * 0.32f, 6.5f);

        for (int i = 0; i < 32; ++i)
        {
            const float cx = row.getX() + cellW * ((float) i + 0.5f);
            const float cy = row.getY() + lampR + 4.0f;
            const bool  isBeat = (i % 4) == 0;
            const bool  on     = playing && i == step;

            paint::lamp (g, { cx, cy }, lampR, isBeat ? c::red : c::amber,
                         isBeat ? c::redDim : c::amberDim, on ? 1.0f : 0.0f);

            if (isBeat)
            {
                g.setColour (c::bakelite.withAlpha (0.7f));
                g.setFont (monoFont (10.0f));
                g.drawText (String (i + 1), Rectangle<float> (cellW * 2.0f, 12.0f).withCentre ({ cx + cellW * 0.5f, cy + lampR + 10.0f })
                                                .withX (cx - lampR), Justification::centredLeft, false);
            }
        }
    }
}
