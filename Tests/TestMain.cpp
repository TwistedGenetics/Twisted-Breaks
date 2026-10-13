/*
    TwistedBreaksTests — headless verification for each development phase.

    Runs the real processor (same sources as the plugin) against a fake host
    playhead and synthetic MIDI, so each phase's checklist is proven without
    opening Ableton.

    Phase 1: plugin shell, MIDI routing, host transport, master section, state
    Phase 2: WAV / AIFF loading (background thread), sample-exact playback,
             velocity, gain, pan, pitch, start / end, reverse, sample-rate
             conversion, click-free retrigger, live edits, sample swap / clear,
             missing / unreadable files, saved sample paths, buffer lifetime,
             and ZERO heap allocations or frees inside processBlock().

    Also renders the editor to a PNG:  TwistedBreaksTests --snapshot out.png
*/
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

//==============================================================================
//  Allocation tracking: counts heap allocations / frees made on the thread
//  that is inside processBlock(). The real-time rule is "none, ever".
//==============================================================================
namespace
{
    thread_local bool   tTrackAllocations = false;
    std::atomic<long>   gAudioAllocs { 0 }, gAudioFrees { 0 };

    void* countedAlloc (std::size_t n)
    {
        if (tTrackAllocations) gAudioAllocs.fetch_add (1);
        if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
        throw std::bad_alloc();
    }

    void countedFree (void* p) noexcept
    {
        if (p != nullptr && tTrackAllocations) gAudioFrees.fetch_add (1);
        std::free (p);
    }
}

void* operator new   (std::size_t n)                     { return countedAlloc (n); }
void* operator new[] (std::size_t n)                     { return countedAlloc (n); }
void  operator delete   (void* p) noexcept               { countedFree (p); }
void  operator delete[] (void* p) noexcept               { countedFree (p); }
void  operator delete   (void* p, std::size_t) noexcept  { countedFree (p); }
void  operator delete[] (void* p, std::size_t) noexcept  { countedFree (p); }

namespace
{
    int failures = 0, checks = 0;

    void check (bool ok, const juce::String& what)
    {
        ++checks;
        if (! ok) ++failures;
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
    }

    /** A host playhead we fully control, standing in for Ableton. */
    struct FakeHost final : juce::AudioPlayHead
    {
        bool   provide = true;
        bool   playing = false;
        double bpm     = 174.0;
        double ppq     = 0.0;
        int    num = 4, den = 4;

        juce::Optional<PositionInfo> getPosition() const override
        {
            if (! provide) return {};
            PositionInfo p;
            p.setIsPlaying (playing);
            p.setBpm (bpm);
            p.setPpqPosition (ppq);
            p.setTimeSignature (TimeSignature { num, den });
            const double barLen = num * 4.0 / den;
            p.setPpqPositionOfLastBarStart (std::floor (ppq / barLen) * barLen);
            return p;
        }

        void advance (int samples, double sr) { if (playing) ppq += samples * (bpm / 60.0) / sr; }
    };

    constexpr double kSr    = 48000.0;
    constexpr int    kBlock = 512;

    float peakOf (const juce::AudioBuffer<float>& b, int start = 0, int num = -1)
    {
        if (num < 0) num = b.getNumSamples() - start;
        float m = 0.0f;
        for (int c = 0; c < b.getNumChannels(); ++c)
            m = juce::jmax (m, b.getMagnitude (c, start, num));
        return m;
    }

    bool isSilent (float peak) { return peak < 1.0e-9f; }

    void runBlock (TwistedBreaksProcessor& p, FakeHost& host, juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi)
    {
        tTrackAllocations = true;
        p.processBlock (buf, midi);
        tTrackAllocations = false;
        host.advance (buf.getNumSamples(), kSr);
        midi.clear();
    }

    void setParam (TwistedBreaksProcessor& p, const juce::String& id, float plainValue)
    {
        auto* param = p.getParameters().getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    void setPadParam (TwistedBreaksProcessor& p, int pad, const char* name, float v)
    {
        setParam (p, tb::params::padId (pad, name), v);
    }

    //==========================================================================
    //  Test audio files
    //==========================================================================
    juce::File testDir()
    {
        static juce::File dir = []
        {
            auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("twisted-breaks-tests-" + juce::String (juce::Time::currentTimeMillis()));
            d.createDirectory();
            return d;
        }();
        return dir;
    }

    /** Decaying 220 Hz sine (L) / 330 Hz sine (R): smooth, never exactly silent. */
    juce::AudioBuffer<float> makeTone (int length, int channels)
    {
        juce::AudioBuffer<float> b (channels, length);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < length; ++i)
            {
                const double f   = ch == 0 ? 220.0 : 330.0;
                const double env = 0.35 + 0.45 * std::exp (-3.0 * i / (double) length);
                b.setSample (ch, i, (float) (env * std::sin (juce::MathConstants<double>::twoPi * f * i / 48000.0 + 0.3)));
            }
        return b;
    }

    /** A synthetic two-bar break (kick / snare / hats) — for the screenshot. */
    juce::AudioBuffer<float> makeBreak (double rate)
    {
        const double bpm = 170.0;
        const int    stepLen = (int) (rate * 60.0 / bpm / 4.0);
        const int    steps = 32;
        juce::AudioBuffer<float> b (2, stepLen * steps);
        b.clear();
        juce::Random rnd (99);

        const char* kick  = "x.........x.....x.x.......x.....";
        const char* snare = "....x.......x..x....x.......x...";
        const char* hats  = "x.x.x.xxx.x.x.x.x.x.x.xxx.x.x.xx";

        auto add = [&] (int start, int len, auto&& fn)
        {
            for (int i = 0; i < len && start + i < b.getNumSamples(); ++i)
            {
                const float v = fn (i);
                b.addSample (0, start + i, v);
                b.addSample (1, start + i, v * 0.92f);
            }
        };

        for (int s = 0; s < steps; ++s)
        {
            const int at = s * stepLen;
            if (kick[s] == 'x')
                add (at, (int) (rate * 0.35), [&] (int i)
                {
                    const double t = i / rate;
                    const double f = 50.0 + 120.0 * std::exp (-t * 30.0);
                    return (float) (0.9 * std::exp (-t * 9.0) * std::sin (juce::MathConstants<double>::twoPi * f * t));
                });
            if (snare[s] == 'x')
                add (at, (int) (rate * 0.25), [&] (int i)
                {
                    const double t = i / rate;
                    return (float) (std::exp (-t * 18.0) * (0.55 * (rnd.nextFloat() * 2.0f - 1.0f)
                                    + 0.35 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * t)));
                });
            if (hats[s] == 'x')
                add (at, (int) (rate * 0.05), [&] (int i)
                {
                    const double t = i / rate;
                    return (float) (0.3 * std::exp (-t * 70.0) * (rnd.nextFloat() * 2.0f - 1.0f));
                });
        }
        return b;
    }

    enum class Fmt { wavFloat, wav16, aiff24 };

    juce::File writeAudio (const juce::String& name, const juce::AudioBuffer<float>& audio, double rate, Fmt fmt)
    {
        const bool aiff = fmt == Fmt::aiff24;
        auto file = testDir().getChildFile (name + (aiff ? ".aiff" : ".wav"));
        file.deleteFile();

        std::unique_ptr<juce::AudioFormat> format;
        if (aiff) format = std::make_unique<juce::AiffAudioFormat>();
        else      format = std::make_unique<juce::WavAudioFormat>();

        const int bits = fmt == Fmt::wavFloat ? 32 : (fmt == Fmt::wav16 ? 16 : 24);
        auto* stream = new juce::FileOutputStream (file);
        std::unique_ptr<juce::AudioFormatWriter> w (format->createWriterFor (stream, rate,
                                                       (unsigned int) audio.getNumChannels(), bits, {}, 0));
        if (w == nullptr) { delete stream; return {}; }
        w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        return file;
    }

    /** Wait (pumping the message loop) until no pad is still loading. */
    bool waitForLoads (TwistedBreaksProcessor& p, int timeoutMs = 5000)
    {
        const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (p.getSamples().isBusy() && juce::Time::getMillisecondCounter() < until)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        return ! p.getSamples().isBusy();
    }

    /** Renders `blocks` blocks after triggering `note` at sample `offset` of the first block. */
    juce::AudioBuffer<float> renderNote (TwistedBreaksProcessor& p, FakeHost& host, int note, int velocity,
                                         int blocks, int offset = 0)
    {
        juce::AudioBuffer<float> out (2, blocks * kBlock), buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), offset);
        for (int b = 0; b < blocks; ++b)
        {
            runBlock (p, host, buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
        }
        return out;
    }

    /** Index of the last non-silent sample, or -1. */
    int lastSound (const juce::AudioBuffer<float>& b)
    {
        for (int i = b.getNumSamples(); --i >= 0;)
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                if (std::abs (b.getSample (ch, i)) > 1.0e-7f) return i;
        return -1;
    }

    struct Rig
    {
        TwistedBreaksProcessor p;
        FakeHost host;
        Rig() { p.setPlayHead (&host); p.prepareToPlay (kSr, kBlock); }

        /** Loads a file into a pad and lets one block pass so the engine picks it up. */
        bool load (int pad, const juce::File& f)
        {
            p.getSamples().loadFile (pad, f);
            const bool ok = waitForLoads (p);
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            runBlock (p, host, buf, midi);
            return ok;
        }

        /** Silence any ringing voices between sub-tests. */
        void settle()
        {
            p.requestPanic();
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            runBlock (p, host, buf, midi);
            runBlock (p, host, buf, midi);
        }
    };

    //==========================================================================
    //  PHASE 1
    //==========================================================================
    void testBasics()
    {
        std::cout << "\n== Phase 1: plugin shell ==" << std::endl;
        TwistedBreaksProcessor p;
        check (p.getTotalNumOutputChannels() == 2, "stereo output bus");
        check (p.getTotalNumInputChannels()  == 0, "no audio input (instrument)");
        check (p.acceptsMidi(), "accepts MIDI");
        check (p.getParameters().getParameter (tb::params::masterGain) != nullptr, "master_gain parameter exists");
        check (p.getParameters().getParameter (tb::params::masterLimiter) != nullptr, "master_limiter parameter exists");
        check (p.getParameters().getParameter (tb::params::masterPanic) != nullptr, "master_panic parameter exists");

        bool allPadParams = true;
        for (int pad = 0; pad < tb::kNumPads; ++pad)
            for (auto* n : { tb::params::padGain, tb::params::padPan, tb::params::padPitch,
                             tb::params::padStart, tb::params::padEnd, tb::params::padReverse })
                allPadParams &= p.getParameters().getParameter (tb::params::padId (pad, n)) != nullptr;
        check (allPadParams, "96 per-pad parameters exist (pad01_gain .. pad16_reverse)");
        check (tb::params::padId (6, tb::params::padPitch) == "pad07_pitch", "stable per-pad parameter IDs");
        check (p.getParameters().processor.getParameters().size() == 99, "99 automatable parameters in total");

        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "silent with no input");

        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 120), 0);
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "empty pad is silent when triggered");
        check (p.getBridge().padTriggerCount[0].load() == 1, "...but the trigger is still counted (lamp blinks)");
    }

    void testMidiRouting (const juce::File& tone)
    {
        std::cout << "\n== MIDI reaches processor / audio output ==" << std::endl;
        Rig r;
        for (int pad = 0; pad < tb::kNumPads; ++pad) r.p.getSamples().loadFile (pad, tone);
        waitForLoads (r.p);

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 120), 100);
        runBlock (r.p, r.host, buf, midi);

        auto& br = r.p.getBridge();
        check (isSilent (peakOf (buf, 0, 100)), "nothing before the note's sample offset (sample-accurate)");
        check (peakOf (buf, 100) > 0.001f, "audio after note 36 (pad 1 fires)");
        check (br.lastMidiNote.load() == 36, "UI bridge sees last MIDI note 36");
        check (br.padTriggerCount[0].load() == 1, "pad 1 trigger count = 1");

        for (int n = 37; n <= 51; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), n - 37);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 40);   // unmapped
        runBlock (r.p, r.host, buf, midi);

        bool allMapped = true;
        for (int pad = 1; pad < 16; ++pad)
            allMapped &= br.padTriggerCount[(size_t) pad].load() == 1;
        check (allMapped, "notes 37..51 trigger pads 2..16");
        check (br.midiEventCount.load() == 17, "17 note-ons counted (incl. unmapped 60)");

        r.p.triggerPadFromUi (5, 1.0f, true);
        runBlock (r.p, r.host, buf, midi);
        check (br.padTriggerCount[5].load() == 2, "UI pad press reaches the audio thread");

        r.p.getEngine().setPadNote (0, 48);
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        runBlock (r.p, r.host, buf, midi);
        check (br.padTriggerCount[0].load() == 2, "remapped pad 1 to note 48");
    }

    void testTransport()
    {
        std::cout << "\n== Host transport (BPM / PPQ / bar-beat) ==" << std::endl;
        TwistedBreaksProcessor p;
        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        auto& t = p.getBridge().transport;

        host.bpm = 174.0; host.ppq = 7.5; host.playing = false;
        runBlock (p, host, buf, midi);
        check (std::abs (t.bpm.load() - 174.0) < 1e-9, "reads host BPM 174");
        check (! t.isPlaying.load(), "reads stopped state");

        tb::TransportSync sync;
        sync.prepare (kSr);
        FakeHost h2; h2.bpm = 174.0; h2.ppq = 7.5;
        auto s = sync.update (&h2, kBlock);
        check (! s.isPlaying && ! s.justStarted, "stopped: no start edge");
        h2.playing = true;
        s = sync.update (&h2, kBlock);
        check (s.justStarted, "start edge detected");
        check (s.barIndex == 1 && s.beatInBar == 3, "PPQ 7.5 in 4/4 -> bar 2 beat 4 (0-based 1,3)");
        check (s.stepAt (0) == 30, "32-step playhead at step 31 (0-based 30)");

        h2.advance (kBlock, kSr);
        s = sync.update (&h2, kBlock);
        check (! s.jumped, "continuous playback: no jump");

        h2.ppq = 0.0;
        s = sync.update (&h2, kBlock);
        check (s.jumped, "loop wrap / relocate detected as jump");

        h2.playing = false;
        s = sync.update (&h2, kBlock);
        check (s.justStopped, "stop edge detected");

        tb::TransportSync sync2; sync2.prepare (kSr);
        FakeHost h3; h3.bpm = 120.0; h3.ppq = 0.9; h3.playing = true;
        const auto s3 = sync2.update (&h3, 4096);
        const auto b  = s3.nextBoundary (0.25);
        check (b.has_value() && *b == 2400, "next 16th boundary found at sample 2400");
        check (! s3.nextBoundary (4.0).has_value(), "no bar boundary inside this block");

        tb::TransportSync sync3; sync3.prepare (kSr);
        FakeHost h4; h4.num = 7; h4.den = 8; h4.ppq = 3.5 + 0.75; h4.playing = true;
        const auto s4 = sync3.update (&h4, kBlock);
        check (s4.barIndex == 1 && s4.beatInBar == 1, "7/8 time signature bar/beat");

        host.provide = false;
        runBlock (p, host, buf, midi);
        check (! t.hostProvidesInfo.load(), "no playhead info handled without crashing");
    }

    void testPanicAndLimiter (const juce::File& tone)
    {
        std::cout << "\n== Master: panic, limiter, level ==" << std::endl;
        Rig r;
        for (int pad = 0; pad < tb::kNumPads; ++pad) r.p.getSamples().loadFile (pad, tone);
        waitForLoads (r.p);
        auto& p = r.p; auto& host = r.host;
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        setParam (p, tb::params::masterGain, 6.0f);
        for (int n = 36; n <= 51; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 127), 0);
        float worst = 0.0f;
        for (int i = 0; i < 20; ++i) { runBlock (p, host, buf, midi); worst = juce::jmax (worst, peakOf (buf)); }
        check (worst <= 0.9661f, "limiter ceiling holds (-0.3 dBFS) with 16 pads at +6 dB: peak "
                                     + juce::String (worst, 4));
        check (p.getBridge().limiterReductionDb.load() > 0.5f, "limiter reports gain reduction");

        setParam (p, tb::params::masterPanic, 1.0f);
        runBlock (p, host, buf, midi);
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "panic parameter silences all voices within one block");

        setParam (p, tb::params::masterPanic, 0.0f);
        setParam (p, tb::params::masterGain, 0.0f);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (p, host, buf, midi);
        p.requestPanic();
        runBlock (p, host, buf, midi);
        float maxStep = 0.0f;
        for (int i = 1; i < 240; ++i)
            maxStep = juce::jmax (maxStep, std::abs (buf.getSample (0, i) - buf.getSample (0, i - 1)));
        check (isSilent (peakOf (buf, 300)), "UI panic silences after the 5 ms fade");
        check (maxStep < 0.05f, "panic fade is click-free (max step " + juce::String (maxStep, 4) + ")");

        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (p, host, buf, midi);
        midi.addEvent (juce::MidiMessage::allSoundOff (1), 0);
        runBlock (p, host, buf, midi);
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "MIDI CC 120 (all sound off) acts as panic");

        setParam (p, tb::params::masterGain, tb::params::masterGainMinDb);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        for (int i = 0; i < 10; ++i) runBlock (p, host, buf, midi);
        check (peakOf (buf) < 1.0e-3f, "master at -inf is effectively silent");
    }

    //==========================================================================
    //  PHASE 2
    //==========================================================================
    void testLoading (const juce::File& tone)
    {
        std::cout << "\n== Phase 2: sample loading ==" << std::endl;
        Rig r;
        auto& sm = r.p.getSamples();

        const auto wav16 = writeAudio ("stereo16", makeTone (24000, 2), 48000.0, Fmt::wav16);
        const auto aiff  = writeAudio ("mono24",   makeTone (44100, 1), 44100.0, Fmt::aiff24);

        sm.loadFile (0, wav16);
        sm.loadFile (1, aiff);
        sm.loadFile (2, tone);
        check (sm.getInfo (0).status == tb::SampleManager::Status::loading, "pad shows LOADING while decoding");
        check (waitForLoads (r.p), "loads finish on the background thread");

        const auto a = sm.getInfo (0), b = sm.getInfo (1);
        check (a.status == tb::SampleManager::Status::ready && a.buffer != nullptr, "16-bit stereo WAV loads");
        check (a.buffer != nullptr && a.buffer->getNumChannels() == 2 && a.buffer->getNumSamples() == 24000
                 && std::abs (a.buffer->sourceSampleRate - 48000.0) < 1e-9, "...2 ch, 24000 samples, 48 kHz");
        check (a.name == "stereo16", "...named after the file");
        check (b.status == tb::SampleManager::Status::ready && b.buffer != nullptr
                 && b.buffer->getNumChannels() == 1 && b.buffer->getNumSamples() == 44100
                 && std::abs (b.buffer->sourceSampleRate - 44100.0) < 1e-9, "24-bit mono AIFF loads (1 ch, 44100 samples, 44.1 kHz)");
        check (b.buffer != nullptr && ! b.buffer->overviewMax.empty(), "waveform overview built off the audio thread");

        sm.loadFile (3, testDir().getChildFile ("does-not-exist.wav"));
        auto junk = testDir().getChildFile ("junk.wav");
        junk.replaceWithText ("this is not audio");
        sm.loadFile (4, junk);
        waitForLoads (r.p);
        check (sm.getInfo (3).status == tb::SampleManager::Status::missing, "missing file -> MISSING, no crash");
        check (sm.getInfo (4).status == tb::SampleManager::Status::unreadable, "corrupt file -> UNREADABLE, no crash");
        check (! tb::SampleManager::isSupportedFile (juce::File ("/x/y.mp3"))
                 && tb::SampleManager::isSupportedFile (juce::File ("/x/AMEN.WAV"))
                 && tb::SampleManager::isSupportedFile (juce::File ("/x/think.aif")), "accepts .wav / .aif(f), case-insensitive");

        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buf (2, kBlock);
        runBlock (r.p, r.host, buf, midi);
        midi.addEvent (juce::MidiMessage::noteOn (1, 39, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (peakOf (buf)), "pad with a missing sample stays silent");
    }

    void testPlayback (const juce::AudioBuffer<float>& src, const juce::File& tone)
    {
        std::cout << "\n== Phase 2: playback ==" << std::endl;
        Rig r;
        r.load (0, tone);
        const int len = src.getNumSamples();

        // Sample-exact at unity: same rate, 0 dB, centre, pitch 0, full velocity.
        auto out = renderNote (r.p, r.host, 36, 127, 4, 100);
        float maxErr = 0.0f;
        for (int i = 0; i < 1500; ++i)
            maxErr = juce::jmax (maxErr, std::abs (out.getSample (0, 100 + i) - src.getSample (0, i)),
                                         std::abs (out.getSample (1, 100 + i) - src.getSample (0, i)));
        check (maxErr < 1.0e-6f, "plays the file sample-exactly, starting on the MIDI note's sample (err "
                                    + juce::String (maxErr, 9) + ")");
        check (isSilent (peakOf (out, 0, 100)), "...and not a sample earlier");
        r.settle();

        out = renderNote (r.p, r.host, 36, 64, 2);
        const float velRatio = out.getSample (0, 500) / src.getSample (0, 500);
        check (std::abs (velRatio - 64.0f / 127.0f) < 1.0e-3f, "velocity 64 plays at 64/127 level");
        r.settle();

        setPadParam (r.p, 0, tb::params::padGain, -6.0206f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, 2);
        check (std::abs (out.getSample (0, 500) / src.getSample (0, 500) - 0.5f) < 2.0e-3f, "gain -6 dB halves the level");
        setPadParam (r.p, 0, tb::params::padGain, 0.0f);

        setPadParam (r.p, 0, tb::params::padPan, -1.0f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, 2);
        check (isSilent (out.getMagnitude (1, 0, out.getNumSamples()))
                 && std::abs (out.getSample (0, 500) - src.getSample (0, 500)) < 1.0e-5f, "pan hard left: right silent, left unity");
        setPadParam (r.p, 0, tb::params::padPan, 0.0f);
        r.settle();

        const int blocks = (len * 2) / kBlock + 8;
        auto lengthAt = [&] () { return lastSound (renderNote (r.p, r.host, 36, 127, blocks)) + 1; };

        const int unityLen = lengthAt();
        check (std::abs (unityLen - len) <= 2, "full region plays for the whole file (" + juce::String (unityLen) + " samples)");

        setPadParam (r.p, 0, tb::params::padPitch, 12.0f);
        r.settle();
        const int upLen = lengthAt();
        check (std::abs (upLen - len / 2) <= 4, "pitch +12 plays twice as fast (" + juce::String (upLen) + " samples)");

        setPadParam (r.p, 0, tb::params::padPitch, -12.0f);
        r.settle();
        const int downLen = lengthAt();
        check (std::abs (downLen - len * 2) <= 6, "pitch -12 plays half speed (" + juce::String (downLen) + " samples)");

        setPadParam (r.p, 0, tb::params::padPitch, 7.0f);
        r.settle();
        const int fifthLen = lengthAt();
        const double expected = len / std::pow (2.0, 7.0 / 12.0);
        check (std::abs (fifthLen - expected) <= 4.0, "pitch +7 st follows the equal-tempered ratio");
        setPadParam (r.p, 0, tb::params::padPitch, 0.0f);

        setPadParam (r.p, 0, tb::params::padStart, 0.5f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, blocks);
        check (std::abs ((lastSound (out) + 1) - len / 2) <= 2, "START 50% plays the second half only");
        check (std::abs (out.getSample (0, 300) - src.getSample (0, len / 2 + 300)) < 1.0e-5f, "...from the right sample");
        check (std::abs (out.getSample (0, 0)) < 1.0e-4f, "...with a fade-in when starting mid-file (no click)");
        setPadParam (r.p, 0, tb::params::padStart, 0.0f);

        setPadParam (r.p, 0, tb::params::padEnd, 0.25f);
        r.settle();
        check (std::abs (lengthAt() - len / 4) <= 2, "END 25% stops a quarter of the way in");
        setPadParam (r.p, 0, tb::params::padEnd, 1.0f);

        setPadParam (r.p, 0, tb::params::padReverse, 1.0f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, blocks);
        check (std::abs (out.getSample (0, 300) - src.getSample (0, len - 1 - 300)) < 1.0e-5f, "REVERSE plays backwards from the end");
        check (std::abs ((lastSound (out) + 1) - len) <= 2, "...for the full region");
        check (std::abs (out.getSample (0, 0)) < 1.0e-4f, "...with a fade-in (no click at the reversed start)");

        setPadParam (r.p, 0, tb::params::padStart, 0.25f);
        setPadParam (r.p, 0, tb::params::padEnd, 0.75f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, blocks);
        check (std::abs (out.getSample (0, 300) - src.getSample (0, (3 * len) / 4 - 1 - 300)) < 1.0e-5f
                 && std::abs ((lastSound (out) + 1) - len / 2) <= 2, "REVERSE + START/END plays the region backwards");
        setPadParam (r.p, 0, tb::params::padStart, 0.0f);
        setPadParam (r.p, 0, tb::params::padEnd, 1.0f);
        setPadParam (r.p, 0, tb::params::padReverse, 0.0f);
        r.settle();

        // Clicks: the edge of the region and a retrigger mid-sample.
        auto maxStep = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            float m = 0.0f;
            for (int i = juce::jmax (1, from); i < to; ++i)
                m = juce::jmax (m, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
            return m;
        };
        const float natural = maxStep (src, 1, len);

        setPadParam (r.p, 0, tb::params::padEnd, 0.5f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, blocks);
        const int endAt = lastSound (out);
        check (maxStep (out, endAt - 200, endAt + 50) <= natural * 1.05f, "region end fades out (no click when END cuts mid-wave)");
        setPadParam (r.p, 0, tb::params::padEnd, 1.0f);
        r.settle();

        {
            juce::AudioBuffer<float> o (2, kBlock * 8), buf (2, kBlock);
            juce::MidiBuffer midi;
            for (int b = 0; b < 8; ++b)
            {
                if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
                if (b == 3) midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 200);
                runBlock (r.p, r.host, buf, midi);
                for (int ch = 0; ch < 2; ++ch) o.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            }
            // The new hit starts with the file's own first sample (that step is
            // in the audio itself); the old voice must fade, adding no click of its own.
            const int at = 3 * kBlock + 200;
            const float onset = std::abs (src.getSample (0, 0));
            check (maxStep (o, at - 10, at + 400) <= onset + natural * 1.05f,
                   "retrigger mid-sample crossfades the old voice (no extra click)");
        }
        r.settle();
    }

    void testRateConversionAndStereo()
    {
        std::cout << "\n== Phase 2: file rates and channels ==" << std::endl;
        Rig r;
        const auto src = makeTone (22050, 2);
        r.load (0, writeAudio ("rate441", src, 44100.0, Fmt::wavFloat));

        auto out = renderNote (r.p, r.host, 36, 127, 60);
        const int len = lastSound (out) + 1;
        const int expected = (int) std::lround (22050.0 * 48000.0 / 44100.0);
        check (std::abs (len - expected) <= 3, "44.1 kHz file in a 48 kHz host plays at the right speed ("
                                                   + juce::String (len) + " vs " + juce::String (expected) + ")");

        r.load (1, writeAudio ("stereo48", src, 48000.0, Fmt::wavFloat));
        out = renderNote (r.p, r.host, 37, 127, 4);
        check (std::abs (out.getSample (0, 700) - src.getSample (0, 700)) < 1e-6f
                 && std::abs (out.getSample (1, 700) - src.getSample (1, 700)) < 1e-6f, "stereo file keeps left and right separate");
    }

    void testLiveEditsAndLifetime (const juce::File& tone, const juce::File& other)
    {
        std::cout << "\n== Phase 2: live edits, swaps, buffer lifetime ==" << std::endl;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (tb::SampleBuffer::liveInstances() == 0, "no sample buffers leaked by earlier tests");

        Rig r;
        r.load (0, tone);
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        // END pulled in while the pad is playing: the voice stops early.
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        setPadParam (r.p, 0, tb::params::padEnd, 0.05f);    // well behind the read head
        runBlock (r.p, r.host, buf, midi);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (peakOf (buf)), "moving END behind a playing voice stops it (with a short fade)");
        setPadParam (r.p, 0, tb::params::padEnd, 1.0f);

        // Playhead is published for the waveform cursor.
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        runBlock (r.p, r.host, buf, midi);
        const float head = r.p.getBridge().padPlayhead[0].load();
        check (head > 0.0f && head < 0.1f, "playhead position published to the UI (" + juce::String (head, 4) + ")");

        // Swap the sample while it plays, then clear it.
        const auto* oldBuffer = r.p.getSamples().getInfo (0).buffer.get();
        r.load (0, other);
        check (r.p.getSamples().getInfo (0).buffer.get() != oldBuffer, "loading over a playing pad swaps the sample");
        for (int i = 0; i < 4; ++i) runBlock (r.p, r.host, buf, midi);

        r.p.getSamples().clearPad (0);
        for (int i = 0; i < 4; ++i) runBlock (r.p, r.host, buf, midi);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (peakOf (buf)) && r.p.getSamples().getInfo (0).status == tb::SampleManager::Status::empty,
               "CLEAR empties the pad");

        // Both samples are now unreferenced: the manager frees them on the
        // message thread (never the audio thread, see the safety checks).
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (tb::SampleBuffer::liveInstances() == 0, "swapped-out and cleared samples are freed (no leaks)");
    }

    void testRealtimeSafety()
    {
        std::cout << "\n== Real-time safety ==" << std::endl;
        check (gAudioAllocs.load() == 0, "zero heap allocations inside processBlock across all tests ("
                                             + juce::String ((int) gAudioAllocs.load()) + ")");
        check (gAudioFrees.load() == 0, "zero heap frees inside processBlock across all tests ("
                                            + juce::String ((int) gAudioFrees.load()) + ")");
    }

    void testState (const juce::File& tone)
    {
        std::cout << "\n== State save / restore (Ableton Live Set) ==" << std::endl;
        juce::MemoryBlock blob;
        const auto gone = writeAudio ("will-be-deleted", makeTone (4800, 1), 48000.0, Fmt::wav16);
        {
            Rig r;
            setParam (r.p, tb::params::masterGain, -6.0f);
            setParam (r.p, tb::params::masterLimiter, 0.0f);
            r.p.getEngine().setPadNote (0, 48);
            r.p.getEngine().setPadNote (15, 70);
            r.load (2, tone);
            r.load (9, gone);
            setPadParam (r.p, 2, tb::params::padPitch, -3.5f);
            setPadParam (r.p, 2, tb::params::padStart, 0.125f);
            setPadParam (r.p, 2, tb::params::padReverse, 1.0f);
            r.p.getStateInformation (blob);
        }
        gone.deleteFile();

        Rig q;
        q.p.setStateInformation (blob.getData(), (int) blob.getSize());
        waitForLoads (q.p);

        const float g = q.p.getParameters().getRawParameterValue (tb::params::masterGain)->load();
        check (std::abs (g + 6.0f) < 0.02f, "master_gain restored (-6 dB): " + juce::String (g, 2));
        check (q.p.getParameters().getRawParameterValue (tb::params::masterLimiter)->load() < 0.5f, "limiter off restored");
        check (q.p.getEngine().getPadNote (0) == 48 && q.p.getEngine().getPadNote (15) == 70, "MIDI note map restored");
        check (q.p.getEngine().getPadNote (1) == 37, "untouched pad keeps default note");

        const auto info = q.p.getSamples().getInfo (2);
        check (info.status == tb::SampleManager::Status::ready && info.file == tone, "sample path restored and reloaded");
        auto raw = [&] (int pad, const char* n) { return q.p.getParameters().getRawParameterValue (tb::params::padId (pad, n))->load(); };
        check (std::abs (raw (2, tb::params::padPitch) + 3.5f) < 0.01f && std::abs (raw (2, tb::params::padStart) - 0.125f) < 1e-4f
                 && raw (2, tb::params::padReverse) > 0.5f, "pad pitch / start / reverse restored");

        const auto missing = q.p.getSamples().getInfo (9);
        check (missing.status == tb::SampleManager::Status::missing && missing.file == gone,
               "sample deleted since saving -> pad shows MISSING, keeps its path, set still opens");

        juce::MemoryBlock again;
        q.p.getStateInformation (again);
        const auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
        check (xml != nullptr && xml->toString().contains ("will-be-deleted"), "missing sample's path is kept when re-saving");

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        runBlock (q.p, q.host, buf, midi);
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, (juce::uint8) 127), 0);
        runBlock (q.p, q.host, buf, midi);
        check (peakOf (buf) > 0.01f, "restored pad plays");

        const char junk[] = "not a valid state";
        q.p.setStateInformation (junk, (int) sizeof (junk));
        check (q.p.getEngine().getPadNote (0) == 48, "garbage state blob ignored without crashing");
    }

    //==========================================================================
    int renderSnapshot (const juce::File& out)
    {
        std::cout << "\n== Rendering editor snapshot ==" << std::endl;
        TwistedBreaksProcessor p;
        FakeHost host;
        host.playing = true; host.bpm = 170.0; host.ppq = 4.0 * 11 + 2.5;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);

        const auto brk = writeAudio ("AMEN STYLE BREAK 170", makeBreak (44100.0), 44100.0, Fmt::wav16);
        p.getSamples().loadFile (0, brk);
        const char* names[] = { "KICK HARD", "SNARE CRACK", "HAT CLOSED", "RIDE", "REESE STAB" };
        for (int i = 0; i < 5; ++i)
            p.getSamples().loadFile (i + 1, writeAudio (names[i], makeTone (12000 + i * 3000, i % 2 + 1), 48000.0, Fmt::wav16));
        p.getSamples().loadFile (7, testDir().getChildFile ("THINK BREAK.wav"));   // shows MISSING
        waitForLoads (p);

        setPadParam (p, 0, tb::params::padStart, 0.06f);
        setPadParam (p, 0, tb::params::padEnd, 0.94f);

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (tb::ui::MainView::kWidth, tb::ui::MainView::kHeight);

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        const int pattern[] = { 1, 2, 3, 2, 1, 3, 2, 4 };
        for (int i = 0; i < 40; ++i)
        {
            if (i % 5 == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + pattern[(i / 5) % 8], (juce::uint8) 120), 0);
            if (i == 22) midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            for (int k = 0; k < 3; ++k) runBlock (p, host, buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (25);
        }
        midi.addEvent (juce::MidiMessage::noteOn (1, 38, (juce::uint8) 110), 0);
        runBlock (p, host, buf, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (40);

        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat png;
        const bool ok = os.openedOk() && png.writeImageToStream (img, os);
        std::cout << (ok ? "  wrote " : "  FAILED to write ") << out.getFullPathName() << std::endl;
        ed.reset();
        return ok ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    int result = 0;
    if (argc >= 3 && juce::String (argv[1]) == "--snapshot")
    {
        result = renderSnapshot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    }
    else
    {
        std::cout << "TWISTED BREAKS test runner" << std::endl;

        const auto toneAudio = makeTone (12000, 1);
        const auto tone  = writeAudio ("tone", toneAudio, 48000.0, Fmt::wavFloat);
        const auto other = writeAudio ("other", makeTone (9000, 2), 48000.0, Fmt::wavFloat);

        testBasics();
        testMidiRouting (tone);
        testTransport();
        testPanicAndLimiter (tone);
        testLoading (tone);
        testPlayback (toneAudio, tone);
        testRateConversionAndStereo();
        testLiveEditsAndLifetime (tone, other);
        testState (tone);
        testRealtimeSafety();

        std::cout << "\n" << (checks - failures) << " / " << checks << " checks passed" << std::endl;
        result = failures == 0 ? 0 : 1;
    }

    testDir().deleteRecursively();
    return result;
}
