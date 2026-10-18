#include "UI/StretchMenu.h"
#include "PluginProcessor.h"

namespace tb::ui::stretch
{
    namespace
    {
        float rawOf (TwistedBreaksProcessor& p, int pad, const char* name)
        {
            return p.getParameters().getRawParameterValue (params::padId (pad, name))->load();
        }

        void setParam (TwistedBreaksProcessor& p, int pad, const char* name, float v)
        {
            if (auto* r = p.getParameters().getParameter (params::padId (pad, name)))
            {
                r->beginChangeGesture();
                r->setValueNotifyingHost (r->convertTo0to1 (v));
                r->endChangeGesture();
            }
        }
    }

    int effectiveMode (TwistedBreaksProcessor& p, int pad)
    {
        return params::effectiveStretch (juce::roundToInt (rawOf (p, pad, params::padStretch)),
                                         rawOf (p, pad, params::padSync) > 0.5f,
                                         rawOf (p, pad, params::padFit) > 0.5f,
                                         params::isLoopMode (juce::roundToInt (rawOf (p, pad, params::padMode))));
    }

    void setMode (TwistedBreaksProcessor& p, int pad, int mode)
    {
        mode = juce::jlimit (0, 5, mode);
        auto& ops = p.getPadOps();
        ops.beginEdit();
        setParam (p, pad, params::padStretch, (float) mode);
        setParam (p, pad, params::padSync, mode != params::stretchOff ? 1.0f : 0.0f);   // LOOP: OFF = free, else locked
        setParam (p, pad, params::padFit, 0.0f);                                        // 0.21 FIT lives in STRETCH now
        ops.endEdit ("Stretch " + juce::String (params::kStretchNames[mode]), PadOps::settings);
    }

    double ownTempo (TwistedBreaksProcessor& p, int pad, bool& isSet)
    {
        const float obpm = rawOf (p, pad, params::padOrigBpm);
        isSet = obpm >= 1.0f;
        if (isSet) return obpm;
        const auto info = p.getSamples().getInfo (pad);
        if (info.buffer == nullptr || info.buffer->getNumSamples() <= 0) return 0.0;
        if (params::isLoopMode (juce::roundToInt (rawOf (p, pad, params::padMode))))
        {
            float a = juce::jlimit (0.0f, 1.0f, rawOf (p, pad, params::padStart));
            float b = juce::jlimit (0.0f, 1.0f, rawOf (p, pad, params::padEnd));
            if (b < a) std::swap (a, b);
            const double secs = (double) (b - a) * info.buffer->getLengthSeconds();
            if (secs <= 0.0) return 0.0;
            return PadVoice::loopBeatsFor (secs) * 60.0 / secs;
        }
        return AudioEngine::detectTempo (*info.buffer, info.slices);
    }

    juce::String describe (int mode)
    {
        static const char* tips[] = { "plays at its own speed (a loop runs free)",
                                      "follows Live's tempo, pitch moves with it (a loop: the old SYNC)",
                                      "each slice on Live's grid, pitch kept",
                                      "grains: pitch kept, smooth",
                                      "the 90s S950 / S1000 cyclic stretch, metallic",
                                      "cyclic, in-phase splices: cleaner" };
        mode = juce::jlimit (0, 5, mode);
        return juce::String (params::kStretchNames[mode]) + ": " + tips[mode];
    }

    void showTempoMenu (TwistedBreaksProcessor& p, int pad, juce::Component& target)
    {
        bool set = false;
        const double own = ownTempo (p, pad, set);
        juce::PopupMenu m;
        m.addSectionHeader ("THE BREAK'S OWN TEMPO: " + (own > 0.0 ? juce::String (own, 2) + " BPM" : juce::String ("-"))
                            + (set ? " (SET)" : " (WORKED OUT)"));
        juce::Component::SafePointer<juce::Component> safe (&target);
        m.addItem ("Double it (x2)", own > 0.0, false, [&p, pad, own] { setParam (p, pad, params::padOrigBpm, (float) juce::jmin (300.0, own * 2.0)); });
        m.addItem ("Halve it (/2)", own > 0.0, false, [&p, pad, own] { setParam (p, pad, params::padOrigBpm, (float) juce::jmax (40.0, own / 2.0)); });
        m.addItem ("Type it in...", [&p, pad, own, safe]
        {
            if (safe == nullptr) return;
            auto* w = new juce::AlertWindow ("Original tempo", "The tempo this break was played at (BPM)", juce::MessageBoxIconType::NoIcon, safe.getComponent());
            w->addTextEditor ("bpm", own > 0.0 ? juce::String (own, 2) : juce::String());
            w->addButton ("SET", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([&p, w, pad] (int r)
            {
                const double v = w->getTextEditorContents ("bpm").getDoubleValue();
                if (r != 0 && v >= 40.0 && v <= 300.0) setParam (p, pad, params::padOrigBpm, (float) v);
            }), true);
        });
        m.addItem ("Work it out again", set, false, [&p, pad] { setParam (p, pad, params::padOrigBpm, 0.0f); });
        m.addSeparator();
        juce::PopupMenu gaps, grains;
        const int gap = juce::roundToInt (rawOf (p, pad, params::padGap));
        for (int k = 0; k < 3; ++k) gaps.addItem (params::kGapNames[k], true, gap == k, [&p, pad, k] { setParam (p, pad, params::padGap, (float) k); });
        m.addSubMenu ("BEATS: fill the gaps", gaps);
        const float gr = rawOf (p, pad, params::padGrain);
        for (float ms : { 8.0f, 12.0f, 18.0f, 25.0f, 30.0f, 40.0f, 60.0f, 90.0f, 140.0f })
            grains.addItem (juce::String (ms, 0) + " ms", true, std::abs (gr - ms) < 0.5f, [&p, pad, ms] { setParam (p, pad, params::padGrain, ms); });
        m.addSubMenu ("TEXTURE grain / AKAI cycle (" + juce::String (gr, 0) + " ms)", grains);
        m.addSeparator();
        juce::PopupMenu modes;
        const int cur = effectiveMode (p, pad);
        for (int k = 0; k < 6; ++k) modes.addItem (describe (k), true, cur == k, [&p, pad, k] { setMode (p, pad, k); });
        m.addSubMenu ("Mode", modes);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
    }
}
