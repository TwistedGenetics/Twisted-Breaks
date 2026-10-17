#include "Core/PresetManager.h"
#include "Core/Parameters.h"
#include "PluginProcessor.h"

/*
    Factory kits (A11): made on this Mac the first time the browser opens, from
    drum sounds synthesised here (no third-party audio ships with the plugin).
*/
namespace tb
{
    namespace
    {
        constexpr double kRate = 44100.0;

        juce::AudioBuffer<float> kick()
        {
            juce::AudioBuffer<float> b (1, (int) (kRate * 0.55));
            double phase = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double t = i / kRate;
                const double f = 46.0 + 120.0 * std::exp (-t * 28.0);
                phase += juce::MathConstants<double>::twoPi * f / kRate;
                const double click = i < 40 ? (1.0 - i / 40.0) * 0.4 : 0.0;
                b.setSample (0, i, (float) ((std::sin (phase) * std::exp (-t * 6.5) + click) * 0.9));
            }
            return b;
        }

        juce::AudioBuffer<float> snare (juce::Random& rnd)
        {
            juce::AudioBuffer<float> b (1, (int) (kRate * 0.38));
            float prev = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double t = i / kRate;
                const float n = rnd.nextFloat() * 2.0f - 1.0f;
                const float hp = n - prev; prev = n;
                const double body = std::sin (juce::MathConstants<double>::twoPi * 190.0 * t) * std::exp (-t * 22.0) * 0.55;
                b.setSample (0, i, (float) (body + hp * 0.45 * std::exp (-t * 13.0)));
            }
            return b;
        }

        juce::AudioBuffer<float> hat (juce::Random& rnd, double seconds, double decay, float level)
        {
            juce::AudioBuffer<float> b (1, (int) (kRate * seconds));
            float p1 = 0.0f, p2 = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double t = i / kRate;
                const float n = rnd.nextFloat() * 2.0f - 1.0f;
                const float hp = n - 2.0f * p1 + p2; p2 = p1; p1 = n;
                b.setSample (0, i, (float) (hp * 0.25 * level * std::exp (-t * decay)));
            }
            return b;
        }

        juce::AudioBuffer<float> ride (juce::Random& rnd)
        {
            auto b = hat (rnd, 1.3, 3.2, 0.5f);
            const double partials[] = { 3127.0, 4211.0, 5303.0, 6619.0, 7457.0 };
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double t = i / kRate;
                double s = 0.0;
                for (double f : partials) s += std::sin (juce::MathConstants<double>::twoPi * f * t);
                b.addSample (0, i, (float) (s * 0.035 * std::exp (-t * 2.6)));
            }
            return b;
        }

        juce::AudioBuffer<float> reese()
        {
            juce::AudioBuffer<float> b (2, (int) (kRate * 1.4));
            double a = 0.0, c = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double t = i / kRate;
                a += 55.0 / kRate; c += 55.6 / kRate;
                const double sa = 2.0 * (a - std::floor (a)) - 1.0, sc = 2.0 * (c - std::floor (c)) - 1.0;
                const double env = juce::jmin (1.0, t * 40.0) * juce::jmin (1.0, (1.4 - t) * 10.0);
                b.setSample (0, i, (float) ((sa * 0.7 + sc * 0.3) * 0.32 * env));
                b.setSample (1, i, (float) ((sa * 0.3 + sc * 0.7) * 0.32 * env));
            }
            return b;
        }

        void mixAt (juce::AudioBuffer<float>& dst, const juce::AudioBuffer<float>& src, int at, float gain)
        {
            const int n = juce::jmin (src.getNumSamples(), dst.getNumSamples() - at);
            for (int ch = 0; ch < dst.getNumChannels(); ++ch)
                if (n > 0) dst.addFrom (ch, at, src, juce::jmin (ch, src.getNumChannels() - 1), 0, n, gain);
        }

        /** Two bars of a broken beat at `bpm`, 16 slices on the eighth notes. */
        juce::AudioBuffer<float> breakLoop (double bpm, const juce::AudioBuffer<float>& k, const juce::AudioBuffer<float>& s,
                                            const juce::AudioBuffer<float>& h, juce::String& slices)
        {
            const double six = 60.0 / bpm / 4.0;                 // a sixteenth, seconds
            juce::AudioBuffer<float> b (1, (int) std::lround (kRate * six * 32));
            b.clear();
            const int kicks[]  = { 0, 2, 10, 11, 16, 18, 26 };
            const int snares[] = { 4, 7, 9, 12, 15, 20, 23, 25, 28, 31 };
            for (int x : kicks)  mixAt (b, k, (int) std::lround (x * six * kRate), 0.85f);
            for (int x : snares) mixAt (b, s, (int) std::lround (x * six * kRate), x % 4 == 0 ? 0.8f : 0.45f);
            for (int x = 0; x < 32; x += 2) mixAt (b, h, (int) std::lround (x * six * kRate), x % 4 == 0 ? 0.9f : 0.6f);
            juce::StringArray st;
            for (int i = 0; i < 16; ++i) st.add (juce::String ((int) std::lround (i * 2 * six * kRate)));
            slices = st.joinIntoString (",");
            return b;
        }

        juce::File writeWav (const juce::File& f, const juce::AudioBuffer<float>& b)
        {
            f.getParentDirectory().createDirectory();
            f.deleteFile();
            juce::WavAudioFormat wav;
            if (auto out = f.createOutputStream())
            {
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (out.get(), kRate, (unsigned int) b.getNumChannels(), 24, {}, 0));
                if (w != nullptr)
                {
                    out.release();
                    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                }
            }
            return f;
        }
    }

    void PresetManager::ensureFactoryContent()
    {
        const auto folder = factoryFolder();
        if (folder.isDirectory() && ! folder.findChildFiles (juce::File::findFiles, false, "*.tbkit").isEmpty()) return;
        folder.createDirectory();

        // Sounds.
        juce::Random rnd (1729);
        const auto src = root.getChildFile ("Library").getChildFile ("Factory Sounds");
        const auto kk = kick(), sn = snare (rnd), hh = hat (rnd, 0.11, 48.0, 1.0f), oh = hat (rnd, 0.45, 9.0, 0.8f), rd = ride (rnd), rs = reese();
        juce::String slices170, slices85;
        const auto br170 = breakLoop (170.0, kk, sn, hh, slices170);
        const auto br85  = breakLoop (170.0, kk, sn, hh, slices85);     // same loop, played halftime by the kit
        const auto fKick  = writeWav (src.getChildFile ("TB Kick.wav"), kk);
        const auto fSnare = writeWav (src.getChildFile ("TB Snare.wav"), sn);
        const auto fHat   = writeWav (src.getChildFile ("TB Hat Closed.wav"), hh);
        const auto fOpen  = writeWav (src.getChildFile ("TB Hat Open.wav"), oh);
        const auto fRide  = writeWav (src.getChildFile ("TB Ride.wav"), rd);
        const auto fReese = writeWav (src.getChildFile ("TB Reese.wav"), rs);
        const auto fBreak = writeWav (src.getChildFile ("TB Break 170.wav"), br170);
        juce::ignoreUnused (br85, slices85);

        // A kit: every parameter at its default, then the changes.
        auto makeKit = [this] (std::function<void (std::map<juce::String, float>&, juce::ValueTree&, SequencerData&)> fill)
        {
            std::map<juce::String, float> values;
            for (auto* prm : processor.getParameters().processor.getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                    values[r->getParameterID()] = r->convertFrom0to1 (r->getDefaultValue());
            juce::ValueTree pads (SampleManager::padsId);
            SequencerData seq;
            fill (values, pads, seq);

            juce::ValueTree kit ("KIT");
            juce::ValueTree ps ("PARAMS");
            for (const auto& [id, v] : values)
            {
                if (id == params::snapRecall || id == params::seqFill || id == params::mutGo || id == params::rsmpRec || id == params::masterPanic || id == params::twistGo) continue;
                juce::ValueTree n ("PARAM");
                n.setProperty ("id", id, nullptr);
                n.setProperty ("value", v, nullptr);
                ps.appendChild (n, nullptr);
            }
            kit.appendChild (ps, nullptr);
            kit.appendChild (pads, nullptr);
            kit.appendChild (seq.toValueTree(), nullptr);
            kit.appendChild (SliceSeqData().toValueTree(), nullptr);
            kit.appendChild (juce::ValueTree (SnapshotManager::snapshotsId), nullptr);
            kit.appendChild (juce::ValueTree (BreakBank::bankId), nullptr);
            juce::ValueTree fx ("FX");
            fx.setProperty ("lockedMask", (juce::int64) fx::kDefaultLockedMask, nullptr);
            fx.setProperty ("group", 0, nullptr);
            kit.appendChild (fx, nullptr);
            return kit;
        };
        auto addPad = [] (juce::ValueTree& pads, int index, const juce::File& f, const juce::String& slices = {})
        {
            juce::ValueTree p ("PAD");
            p.setProperty ("index", index, nullptr);
            p.setProperty ("path", f.getFullPathName(), nullptr);
            if (slices.isNotEmpty()) { p.setProperty ("slices", slices, nullptr); p.setProperty ("detected", slices, nullptr); }
            p.setProperty ("sensitivity", 0.5f, nullptr);
            p.setProperty ("sliceCount", 16, nullptr);
            pads.appendChild (p, nullptr);
        };
        auto step = [] (SequencerData& seq, int pad, std::initializer_list<int> at, int vel = 110)
        {
            for (int k : at) { SeqStep s; s.on = true; s.velocity = vel; seq.set (pad, k, s); }
        };
        auto common = [&] (juce::ValueTree& pads)
        {
            addPad (pads, 1, fKick); addPad (pads, 2, fSnare); addPad (pads, 3, fHat);
            addPad (pads, 4, fOpen); addPad (pads, 5, fRide); addPad (pads, 6, fReese);
        };

        Meta m;
        m.type = Type::kit;
        m.pads = 7;

        // 1. Jungle 170: the break on pad 01 (synced loop) and pad 08 (slices), hits on 02-07.
        m.name = "Factory Jungle 170"; m.bpm = 170.0; m.tags = { "Factory", "Jungle", "Break" };
        m.note = "A synced break on 01, the same break sliced on 08, hits on 02-07 and a starter pattern.";
        writePreset (folder.getChildFile ("Factory Jungle 170.tbkit"), m, makeKit ([&] (auto& v, auto& pads, auto& seq)
        {
            addPad (pads, 0, fBreak, slices170);
            addPad (pads, 7, fBreak, slices170);
            common (pads);
            v[params::padId (0, params::padMode)] = (float) params::modeLoop;
            v[params::padId (7, params::padMode)] = (float) params::modeSlice;
            v[params::padId (4, params::padChoke)] = 1.0f;
            v[params::padId (3, params::padChoke)] = 1.0f;
            v[params::seqLength] = 16.0f;
            step (seq, 1, { 0, 10 });
            step (seq, 2, { 4, 12 });
            step (seq, 3, { 0, 2, 4, 6, 8, 10, 12, 14 }, 80);
        }));

        // 2. Halftime 85: kick on 1, snare on 3, the hats busier.
        m.name = "Factory Halftime 85"; m.bpm = 85.0; m.tags = { "Factory", "Halftime", "DnB" };
        m.note = "Halftime groove: kick, snare on the 3, shuffled hats, a reese on 07.";
        m.pads = 6;
        writePreset (folder.getChildFile ("Factory Halftime 85.tbkit"), m, makeKit ([&] (auto& v, auto& pads, auto& seq)
        {
            common (pads);
            v[params::padId (4, params::padChoke)] = 1.0f;
            v[params::padId (3, params::padChoke)] = 1.0f;
            v[params::padId (6, params::padMode)] = (float) params::modeLoop;
            v[params::seqLength] = 16.0f;
            step (seq, 1, { 0, 7 });
            step (seq, 2, { 8 });
            step (seq, 3, { 0, 2, 3, 6, 10, 11, 14 }, 75);
            step (seq, 4, { 12 }, 70);
        }));

        // 3. Init: everything at its default, no audio.
        m.name = "Factory Init"; m.bpm = 0.0; m.tags = { "Factory", "Template" };
        m.note = "Every setting at its default and no samples: a clean start.";
        m.pads = 0;
        writePreset (folder.getChildFile ("Factory Init.tbkit"), m, makeKit ([] (auto&, auto&, auto&) {}));
        listDirty = true;
        setStatus ({});
    }
}
