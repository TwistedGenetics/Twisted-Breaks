#include "UI/PadGrid.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    namespace
    {
        // Phase 1 legends describe the built-in calibration voices.
        const char* const kRowNames[] = { "SUB KICK", "SNARE", "HAT", "TICK" };
    }

    PadGrid::PadGrid (TwistedBreaksProcessor& p) : processor (p)
    {
        shownNotes.fill (-2);

        for (int i = 0; i < kNumPads; ++i)
        {
            auto pad = std::make_unique<PadComponent> (i);

            pad->onPress   = [this] (int idx, float vel) { processor.triggerPadFromUi (idx, vel, true); };
            pad->onRelease = [this] (int idx)            { processor.triggerPadFromUi (idx, 0.0f, false); };
            pad->onSelect  = [this] (int idx)            { setSelectedPad (idx); };

            addAndMakeVisible (*pad);
            pads[(size_t) i] = std::move (pad);
        }

        setSelectedPad (0);
        refresh();
    }

    void PadGrid::resized()
    {
        const int gap  = 10;
        const int cell = (getWidth()  - gap * 3) / 4;
        const int rowH = (getHeight() - gap * 3) / 4;

        for (int i = 0; i < kNumPads; ++i)
            pads[(size_t) i]->setBounds ((i % 4) * (cell + gap), (i / 4) * (rowH + gap), cell, rowH);
    }

    void PadGrid::setSelectedPad (int pad)
    {
        selectedPad = juce::jlimit (0, kNumPads - 1, pad);
        for (int i = 0; i < kNumPads; ++i)
            pads[(size_t) i]->setSelected (i == selectedPad);
        if (onSelectionChanged) onSelectionChanged (selectedPad);
    }

    void PadGrid::refresh()
    {
        auto& bridge = processor.getBridge();
        auto& engine = processor.getEngine();

        for (int i = 0; i < kNumPads; ++i)
        {
            auto& pad = *pads[(size_t) i];
            pad.setActivity (bridge.padActivity[(size_t) i].load (std::memory_order_relaxed));

            const int note = engine.getPadNote (i);
            if (note != shownNotes[(size_t) i])
            {
                shownNotes[(size_t) i] = note;
                pad.setLabels ("CALIBRATION", kRowNames[i / 4], note, 'A');
            }
        }
    }
}
