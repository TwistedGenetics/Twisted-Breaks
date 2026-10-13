#include "UI/PadGrid.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    PadGrid::PadGrid (TwistedBreaksProcessor& p) : processor (p)
    {
        shownNotes.fill (-2);
        shownVersions.fill (0xffffffff);

        for (int i = 0; i < kNumPads; ++i)
        {
            auto pad = std::make_unique<PadComponent> (i);

            pad->onPress       = [this] (int idx, float vel) { processor.triggerPadFromUi (idx, vel, true); };
            pad->onRelease     = [this] (int idx)            { processor.triggerPadFromUi (idx, 0.0f, false); };
            pad->onSelect      = [this] (int idx)            { setSelectedPad (idx); };
            pad->onFileDropped = [this] (int idx, const juce::File& f) { processor.getSamples().loadFile (idx, f); };

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
        auto& bridge  = processor.getBridge();
        auto& engine  = processor.getEngine();
        auto& samples = processor.getSamples();

        for (int i = 0; i < kNumPads; ++i)
        {
            auto& pad = *pads[(size_t) i];
            pad.setActivity (bridge.padActivity[(size_t) i].load (std::memory_order_relaxed));

            const int  note    = engine.getPadNote (i);
            const auto version = samples.getVersion (i);
            if (note == shownNotes[(size_t) i] && version == shownVersions[(size_t) i])
                continue;

            shownNotes[(size_t) i]    = note;
            shownVersions[(size_t) i] = version;

            const auto info = samples.getInfo (i);
            using S = SampleManager::Status;

            juce::String upper, name;
            bool warn = false;
            switch (info.status)
            {
                case S::empty:   upper = "EMPTY";   name = "-";  break;
                case S::loading: upper = "LOADING"; name = info.name; break;
                case S::ready:
                    upper = juce::String (info.buffer->getLengthSeconds(), 2) + " s  "
                          + (info.buffer->getNumChannels() > 1 ? "ST" : "MONO");
                    name  = info.name;
                    break;
                case S::missing:
                case S::unreadable:
                case S::tooLong:
                    upper = SampleManager::statusText (info.status);
                    name  = info.name;
                    warn  = true;
                    break;
            }

            pad.setLabels (upper, name.toUpperCase(), note, 'A', warn, info.buffer != nullptr);
        }
    }
}
