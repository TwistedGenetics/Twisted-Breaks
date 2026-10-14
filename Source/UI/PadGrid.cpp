#include "UI/PadGrid.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    PadGrid::PadGrid (TwistedBreaksProcessor& p) : processor (p)
    {
        shownNotes.fill (-2);
        shownVersions.fill (0xffffffff);

        for (int i = 0; i < kPadsPerScene; ++i)
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
        const int gap  = 10, gapV = 4;        // tighter rows since the scene strip (0.13)
        const int cell = (getWidth()  - gap * 3) / 4;
        const int rowH = (getHeight() - gapV * 3) / 4;

        for (int i = 0; i < kPadsPerScene; ++i)
            pads[(size_t) i]->setBounds ((i % 4) * (cell + gap), (i / 4) * (rowH + gapV), cell, rowH);
    }

    void PadGrid::setSelectedPad (int pad)
    {
        selectedPad = juce::jlimit (0, kNumPads - 1, pad);
        if (sceneOf (selectedPad) != scene) setScene (sceneOf (selectedPad));
        for (int i = 0; i < kPadsPerScene; ++i)
            pads[(size_t) i]->setSelected (pads[(size_t) i]->getPadIndex() == selectedPad);
        if (onSelectionChanged) onSelectionChanged (selectedPad);
    }

    void PadGrid::setScene (int s)
    {
        s = juce::jlimit (0, kNumScenes - 1, s);
        if (s == scene) return;
        scene = s;
        for (int i = 0; i < kPadsPerScene; ++i)
        {
            pads[(size_t) i]->setPadIndex (scene * kPadsPerScene + i);
            pads[(size_t) i]->setSelected (scene * kPadsPerScene + i == selectedPad);
        }
        shownNotes.fill (-2);
        shownVersions.fill (0xffffffff);
        shownTags.fill ({});
        refresh();
    }

    void PadGrid::refresh()
    {
        auto& bridge  = processor.getBridge();
        auto& engine  = processor.getEngine();
        auto& samples = processor.getSamples();

        bool anySolo = false;
        for (int i = 0; i < kNumPads; ++i)
            anySolo = anySolo || processor.getParameters().getRawParameterValue (params::padId (i, params::padSolo))->load() > 0.5f;

        for (int t = 0; t < kPadsPerScene; ++t)
        {
            auto& pad = *pads[(size_t) t];
            const int i = pad.getPadIndex();      // engine pad
            pad.setActivity (bridge.padActivity[(size_t) i].load (std::memory_order_relaxed));

            auto raw = [&] (const char* name) { return processor.getParameters().getRawParameterValue (params::padId (i, name))->load(); };
            const bool muted  = raw (params::padMute) > 0.5f;
            const bool soloed = raw (params::padSolo) > 0.5f;
            pad.setMuteSolo (muted, soloed, muted || (anySolo && ! soloed));
            pad.setQueued (bridge.padQueued[(size_t) i].load (std::memory_order_relaxed));
            pad.setGroup (juce::roundToInt (raw (params::padGroup)));

            // Footer tag: playback mode, choke and SWAP group, e.g. "LOOP C2 SW1".
            const int mode  = juce::roundToInt (raw (params::padMode));
            const int choke = juce::roundToInt (raw (params::padChoke));
            const int swap  = juce::roundToInt (raw (params::padSwap));
            const juce::String modeTag = mode == params::modeSeqOne  ? juce::String ("SEQ1")
                                       : mode == params::modeSeqLoop ? juce::String ("SEQL")
                                       : mode == params::modeLoop  ? juce::String ("LOOP")
                                       : mode == params::modeSlice ? "SL " + juce::String (juce::roundToInt (raw (params::padSlice)))
                                                                   : juce::String ("ONE");
            const juce::String tag = modeTag + (choke > 0 ? " C" + juce::String (choke) : juce::String())
                                             + (swap > 0 ? " SW" + juce::String (swap) : juce::String())
                                             + (raw (params::padFollowBank) > 0.5f ? juce::String (" BK") : juce::String())
                                             + juce::String (std::array<const char*, 5> { "", " SP12", " MPC", " S950", " LOFI" }[(size_t) juce::jlimit (0, 4, juce::roundToInt (raw (params::padCharacter)))]);

            const int  note    = engine.getPadNote (i);
            const auto version = samples.getVersion (i);
            if (note == shownNotes[(size_t) t] && version == shownVersions[(size_t) t] && tag == shownTags[(size_t) t])
                continue;

            shownNotes[(size_t) t]    = note;
            shownVersions[(size_t) t] = version;
            shownTags[(size_t) t]     = tag;

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

            pad.setLabels (upper, name.toUpperCase(), note, tag, warn, info.buffer != nullptr);
        }
    }
}
