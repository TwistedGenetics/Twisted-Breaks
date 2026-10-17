#include "Core/Exporter.h"
#include "Core/Parameters.h"
#include "PluginProcessor.h"

namespace tb
{
    namespace
    {
        struct OfflineHead final : juce::AudioPlayHead
        {
            double ppq = 0.0, bpm = 120.0, sampleRate = 48000.0;
            int num = 4, den = 4;
            juce::int64 samples = 0;

            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo p;
                p.setIsPlaying (true);
                p.setBpm (bpm);
                p.setPpqPosition (ppq);
                p.setTimeSignature (juce::AudioPlayHead::TimeSignature { num, den });
                const double barLen = num * 4.0 / den;
                p.setPpqPositionOfLastBarStart (std::floor (ppq / barLen + 1.0e-9) * barLen);
                p.setTimeInSamples (samples);
                p.setTimeInSeconds ((double) samples / sampleRate);
                return p;
            }
        };

        juce::String fileSafe (const juce::String& s) { return juce::File::createLegalFileName (s.trim()); }
    }

    struct Exporter::Job : juce::Thread
    {
        Job() : juce::Thread ("Twisted Breaks export") {}
        ~Job() override { stopThread (10000); }

        std::unique_ptr<TwistedBreaksProcessor> offline;
        OfflineHead head;
        int pad = -1, bars = 4;
        juce::File out;
        std::function<void (juce::Result, juce::File)> done;
        std::atomic<float>* progress = nullptr;
        std::atomic<bool> finished { false };
        juce::Result result = juce::Result::ok();
        bool started = false;
        double waitedMs = 0.0;

        void run() override
        {
            constexpr int block = 512;
            auto& p = *offline;
            p.setPlayHead (&head);
            p.prepareToPlay (head.sampleRate, block);
            const double beats = bars * head.num * 4.0 / head.den;
            const auto total = (int) std::llround (beats * 60.0 / head.bpm * head.sampleRate);
            juce::AudioBuffer<float> render (2, total), buf (2, block);
            juce::MidiBuffer midi;
            for (int pos = 0; pos < total && ! threadShouldExit(); pos += block)
            {
                const int n = juce::jmin (block, total - pos);
                juce::AudioBuffer<float> part (buf.getArrayOfWritePointers(), 2, n);
                part.clear();
                p.processBlock (part, midi);
                midi.clear();
                for (int ch = 0; ch < 2; ++ch) render.copyFrom (ch, pos, part, ch, 0, n);
                head.ppq += n * head.bpm / 60.0 / head.sampleRate;
                head.samples += n;
                if (progress != nullptr) progress->store ((float) pos / (float) juce::jmax (1, total));
            }
            p.releaseResources();
            p.setPlayHead (nullptr);
            if (threadShouldExit()) { result = juce::Result::fail ("Export stopped"); finished = true; return; }

            out.getParentDirectory().createDirectory();
            out.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> stream (out.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w;
            if (stream != nullptr) w.reset (wav.createWriterFor (stream.get(), head.sampleRate, 2, 24, {}, 0));
            if (w == nullptr) { result = juce::Result::fail ("Couldn't write " + out.getFullPathName()); finished = true; return; }
            stream.release();
            w->writeFromAudioSampleBuffer (render, 0, total);
            w.reset();
            finished = true;
        }
    };

    //==========================================================================
    Exporter::Exporter (TwistedBreaksProcessor& p) : processor (p), folder (defaultFolder()) {}

    Exporter::~Exporter()
    {
        stopTimer();
        job.reset();
    }

    juce::File Exporter::defaultFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Twisted Breaks").getChildFile ("Exports");
    }

    juce::File Exporter::wavFileFor (int pad, int bars) const
    {
        const auto bpm = juce::roundToInt (processor.getBridge().transport.bpm.load());
        const auto what = pad >= 0 ? "Pad " + padLabel (pad) : juce::String ("Kit");
        folder.createDirectory();
        return folder.getNonexistentChildFile (fileSafe (processor.getPresets().getKitName() + " - " + what + " - " + juce::String (bars) + " bars "
                                                         + juce::String (bpm) + " BPM"), ".wav", false);
    }

    juce::File Exporter::midiFileFor (int pad, int bars) const
    {
        const auto what = pad >= 0 ? "Pad " + padLabel (pad) : juce::String ("Sequence");
        folder.createDirectory();
        return folder.getNonexistentChildFile (fileSafe (processor.getPresets().getKitName() + " - " + what + " - " + juce::String (bars) + " bars"), ".mid", false);
    }

    bool Exporter::startWav (int pad, int bars, std::function<void (juce::Result, juce::File)> done)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (busy) return false;
        bars = juce::jlimit (1, 32, bars);

        // A copy of this plugin, without controller settings.
        juce::MemoryBlock mb;
        processor.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        if (xml == nullptr) return false;
        for (const auto& tag : { PushController::pushId.toString(), MikroController::mikroId.toString(), PadLights::lightsId.toString() })
            if (auto* e = xml->getChildByName (tag)) xml->removeChildElement (e, true);
        juce::MemoryBlock clean;
        juce::AudioProcessor::copyXmlToBinary (*xml, clean);

        job = std::make_unique<Job>();
        job->offline = std::make_unique<TwistedBreaksProcessor> (false);   // never grabs Push / the Mikro
        job->offline->setStateInformation (clean.getData(), (int) clean.getSize());
        job->pad = pad;
        job->bars = bars;
        job->head.bpm = juce::jmax (20.0, processor.getBridge().transport.bpm.load());
        job->head.num = juce::jlimit (1, 32, processor.getBridge().transport.timeSigNum.load());
        job->head.den = juce::jlimit (1, 32, processor.getBridge().transport.timeSigDen.load());
        job->head.sampleRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        job->out = wavFileFor (pad, bars);
        job->done = std::move (done);
        job->progress = &progress;
        progress = 0.0f;
        busy = true;
        startTimer (30);
        return true;
    }

    void Exporter::timerCallback()
    {
        if (job == nullptr) { stopTimer(); return; }
        if (! job->started)
        {
            // Wait for the copy's samples (decoded in the background), then solo and render.
            job->waitedMs += 30.0;
            auto& off = *job->offline;
            bool loading = off.getSamples().isBusy();
            for (int s = 0; s < BreakBank::kSlots; ++s) loading = loading || off.getBank().get (s).status == BreakBank::Status::loading;
            if (loading && job->waitedMs < 20000.0) return;
            if (job->pad >= 0)
                for (int p = 0; p < kNumPads; ++p)
                    if (auto* r = off.getParameters().getParameter (params::padId (p, params::padSolo)))
                        r->setValueNotifyingHost (p == job->pad ? 1.0f : 0.0f);
            job->started = true;
            job->startThread();
            return;
        }
        if (job->finished.load())
        {
            stopTimer();
            auto r = job->result;
            auto f = job->out;
            auto cb = std::move (job->done);
            job.reset();                 // joins the (finished) thread, deletes the copy on this thread
            finish (r, f);
            if (cb) cb (r, f);
        }
    }

    void Exporter::finish (juce::Result r, juce::File f)
    {
        busy = false;
        progress = 1.0f;
        if (r.wasOk()) lastFile = f;
    }

    //==========================================================================
    juce::AudioBuffer<float> Exporter::akaiRender (const juce::AudioBuffer<float>& src, double rate, double timeFactor,
                                                   float semis, float cycleMs, bool revised)
    {
        // A PadVoice in AKAI stretch, at the file's own rate: the play head crosses the
        // sample in timeFactor x its length, each cycle read at the pitch.
        auto buf = SampleBuffer::Ptr (new SampleBuffer());
        buf->audio.makeCopyOf (src);
        buf->sourceSampleRate = rate;
        PadVoice v;
        v.prepare (rate);
        PadVoice::Settings s;
        s.regionStart = 0;
        s.regionEnd = src.getNumSamples();
        s.stretch = revised ? 4 : 3;
        s.increment = 1.0 / juce::jlimit (0.25, 20.0, timeFactor);
        s.readInc = std::pow (2.0, juce::jlimit (-36.0f, 36.0f, semis) / 12.0);
        s.grainSamples = juce::jmax (32, (int) (juce::jlimit (2.0f, 400.0f, cycleMs) * 0.001 * rate));
        v.start (buf, s, 1.0f);
        const int total = (int) std::ceil (src.getNumSamples() * juce::jlimit (0.25, 20.0, timeFactor)) + (int) (rate * 0.05);
        juce::AudioBuffer<float> out (2, total);
        out.clear();
        constexpr int block = 4096;
        for (int pos = 0; pos < total && v.isActive(); pos += block)
        {
            juce::AudioBuffer<float> part (out.getArrayOfWritePointers(), 2, pos, juce::jmin (block, total - pos));
            v.renderAdding (part, 0, part.getNumSamples());
        }
        return out;
    }

    juce::Result Exporter::renderAkai (int pad, double timeFactor, float semis, float cycleMs, bool revised, juce::File& outFile)
    {
        const auto info = processor.getSamples().getInfo (pad);
        if (info.buffer == nullptr) return juce::Result::fail ("Pad " + padLabel (pad) + " has no sample");
        const auto rendered = akaiRender (info.buffer->audio, info.buffer->sourceSampleRate, timeFactor, semis, cycleMs, revised);
        const auto dir = folder.getParentDirectory().getChildFile ("Akai");
        dir.createDirectory();
        const auto name = fileSafe (info.name + " AKAI " + juce::String (juce::roundToInt (timeFactor * 100.0)) + "% "
                                    + (semis >= 0.0f ? "+" : "") + juce::String (semis, 1) + "st " + juce::String (cycleMs, 0) + "ms" + (revised ? " R" : ""));
        outFile = dir.getNonexistentChildFile (name, ".wav", false);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w;
        if (stream != nullptr) w.reset (wav.createWriterFor (stream.get(), info.buffer->sourceSampleRate, 2, 24, {}, 0));
        if (w == nullptr) return juce::Result::fail ("Couldn't write " + outFile.getFullPathName());
        stream.release();
        w->writeFromAudioSampleBuffer (rendered, 0, rendered.getNumSamples());
        return juce::Result::ok();
    }

    //==========================================================================
    juce::Result Exporter::writeMidi (int pad, int bars, const juce::File& file)
    {
        auto& apvts = processor.getParameters();
        auto& seq = processor.getBridge().seq;
        const double bpm = juce::jmax (20.0, processor.getBridge().transport.bpm.load());
        const int num = juce::jlimit (1, 32, processor.getBridge().transport.timeSigNum.load());
        const int den = juce::jlimit (1, 32, processor.getBridge().transport.timeSigDen.load());
        const int globalLen = juce::jlimit (1, SequencerData::kSteps, juce::roundToInt (apvts.getRawParameterValue (params::seqLength)->load()));
        const double stepPpq = params::seqResPpq[juce::jlimit (0, 2, juce::roundToInt (apvts.getRawParameterValue (params::seqRes)->load()))];
        const double totalBeats = juce::jlimit (1, 64, bars) * num * 4.0 / den;
        constexpr int tpq = 960;

        juce::MidiMessageSequence track;
        track.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60.0e6 / bpm)), 0.0);
        track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (num, den), 0.0);
        int notes = 0;
        for (int p = 0; p < kNumPads; ++p)
        {
            if (pad >= 0 && p != pad) continue;
            if (! seq.laneHasSteps (p)) continue;
            const int mode = juce::roundToInt (apvts.getRawParameterValue (params::padId (p, params::padMode))->load());
            if (pad < 0 && params::isSeqMode (mode)) continue;              // launched by hitting the pad, not the main sequence
            const int note = processor.getEngine().getPadNote (p);
            if (note < 0 || note > 127) continue;
            const int own = juce::roundToInt (apvts.getRawParameterValue (params::padId (p, params::padSeqLen))->load());
            const int len = own > 0 ? own : globalLen;
            for (int k = 0; k * stepPpq < totalBeats - 1.0e-9; ++k)
            {
                const auto st = seq.get (p, k % len);
                if (! st.on || ! st.hit) continue;
                const int r = juce::jlimit (1, 4, st.ratchet);
                const double t0 = juce::jmax (0.0, k * stepPpq + st.micro / 128.0 * stepPpq);
                for (int j = 0; j < r; ++j)
                {
                    const double t = t0 + j * stepPpq / r;
                    if (t >= totalBeats) break;
                    const double dur = stepPpq / r * 0.9;
                    track.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) juce::jlimit (1, 127, st.velocity)), t * tpq);
                    track.addEvent (juce::MidiMessage::noteOff (1, note), juce::jmin (totalBeats, t + dur) * tpq - 1.0);
                    ++notes;
                }
            }
        }
        if (notes == 0) return juce::Result::fail ("No steps to export");
        track.addEvent (juce::MidiMessage::endOfTrack(), totalBeats * tpq);
        track.updateMatchedPairs();

        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (tpq);
        mf.addTrack (track);
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::FileOutputStream out (file);
        if (! out.openedOk() || ! mf.writeTo (out)) return juce::Result::fail ("Couldn't write " + file.getFullPathName());
        out.flush();
        lastFile = file;
        return juce::Result::ok();
    }
}
