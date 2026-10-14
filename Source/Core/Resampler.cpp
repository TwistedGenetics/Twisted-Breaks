#include "Core/Resampler.h"
#include "Core/Parameters.h"
#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace tb
{
    Resampler::Resampler (TwistedBreaksProcessor& p)
        : processor (p),
          folder (juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Twisted Breaks").getChildFile ("Resamples"))
    {
        startTimerHz (30);
    }

    Resampler::~Resampler()
    {
        stopTimer();
    }

    void Resampler::record()
    {
        auto& apvts = processor.getParameters();
        auto raw = [&apvts] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
        if (saving || processor.getEngine().getResampleState() >= AudioEngine::rsRecording) return;

        const int source = juce::jlimit (0, 4, juce::roundToInt (raw (params::rsmpSource)));
        const int bars   = params::rsmpBarCounts[juce::jlimit (0, 3, juce::roundToInt (raw (params::rsmpBars)))];
        const bool atBar = raw (params::rsmpStart) > 0.5f;
        wantLoop  = raw (params::rsmpLoop) > 0.5f;
        wantSlice = raw (params::rsmpSlice) > 0.5f;
        busyPad = selectedPad;
        processor.getEngine().armResample (source, bars, atBar);
    }

    void Resampler::cancel()
    {
        if (saving) return;
        processor.getEngine().cancelResample();
        busyPad = -1;
    }

    Resampler::State Resampler::getState() const
    {
        if (saving) return State::saving;
        switch (processor.getEngine().getResampleState())
        {
            case AudioEngine::rsArmed:     return State::armed;
            case AudioEngine::rsRecording: return State::recording;
            case AudioEngine::rsDone:      return State::saving;
            default:                       return State::idle;
        }
    }

    float Resampler::getProgress() const
    {
        auto& e = processor.getEngine();
        if (e.getResampleState() == AudioEngine::rsDone || saving) return 1.0f;
        if (e.getResampleState() != AudioEngine::rsRecording) return 0.0f;
        return (float) e.getResampleWritten() / (float) juce::jmax (1, e.getResampleLength());
    }

    void Resampler::poll()
    {
        auto& apvts = processor.getParameters();
        auto& engine = processor.getEngine();

        // "Resample" parameter: MIDI-mappable REC, on its rising edge.
        const bool rec = apvts.getRawParameterValue (params::rsmpRec)->load() > 0.5f;
        if (rec && ! lastRec) record();
        lastRec = rec;

        // The take is in: copy it out, write it in the background, then load it.
        if (! saving && engine.getResampleState() == AudioEngine::rsDone)
        {
            const int len = engine.getResampleLength();
            auto take = std::make_shared<juce::AudioBuffer<float>> (2, len);
            for (int ch = 0; ch < 2; ++ch) take->copyFrom (ch, 0, engine.getResampleTake(), ch, 0, len);
            engine.resampleTaken();

            saving = true;
            const int pad = busyPad >= 0 ? busyPad : selectedPad;
            const double rate = engine.getSampleRate();
            const auto file = folder.getChildFile ("Resample " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S")
                                                   + " pad " + juce::String (pad + 1).paddedLeft ('0', 2) + ".wav")
                                    .getNonexistentSibling (false);
            juce::WeakReference<Resampler> weak (this);
            juce::Thread::launch ([take, file, rate, pad, weak]
            {
                bool ok = file.getParentDirectory().createDirectory().wasOk();
                if (ok)
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                    std::unique_ptr<juce::AudioFormatWriter> w;
                    if (out != nullptr)
                        w.reset (wav.createWriterFor (out.get(), rate, 2u, 24, juce::StringPairArray(), 0));
                    if (w != nullptr)
                    {
                        out.release();                     // the writer owns it now
                        ok = w->writeFromAudioSampleBuffer (*take, 0, take->getNumSamples());
                    }
                    else ok = false;
                }
                juce::MessageManager::callAsync ([weak, file, pad, ok]
                {
                    if (auto* self = weak.get()) self->saved (file, pad, ok);
                });
            });
        }

        // AUTO SLICE once the new sample has loaded.
        if (slicePad >= 0)
        {
            const auto info = processor.getSamples().getInfo (slicePad);
            if (info.buffer != nullptr && info.file == lastFile && ! processor.getSamples().isBusy())
            {
                processor.getSamples().autoSlice (slicePad);
                slicePad = -1;
            }
        }
    }

    void Resampler::saved (const juce::File& file, int pad, bool ok)
    {
        saving = false;
        busyPad = -1;
        if (! ok) return;

        lastFile = file;
        ++count;
        processor.getSamples().loadFile (pad, file);

        // AS LOOP: a tempo-synced loop, so it plays straight back in time.
        auto set = [this, pad] (const char* name, float plain)
        {
            if (auto* prm = processor.getParameters().getParameter (params::padId (pad, name)))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
                prm->endChangeGesture();
            }
        };
        if (wantLoop)
        {
            set (params::padMode, (float) params::modeLoop);
            set (params::padSync, 1.0f);
            set (params::padStart, 0.0f);
            set (params::padEnd, 1.0f);
        }
        if (wantSlice) slicePad = pad;
        processor.markStateChanged();
    }
}
