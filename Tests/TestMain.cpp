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
#include "Engine/TransientDetector.h"
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

    const char* const kBreakKick  = "x.........x.....x.x.......x.....";
    const char* const kBreakSnare = "....x.......x..x....x.......x...";
    const char* const kBreakHats  = "x.x.x.xxx.x.x.x.x.x.x.xxx.x.x.xx";
    int breakStepLength (double rate) { return (int) (rate * 60.0 / 170.0 / 4.0); }

    /** A synthetic two-bar break (kick / snare / hats) at 170 BPM. */
    juce::AudioBuffer<float> makeBreak (double rate)
    {
        const double bpm = 170.0;
        const int    stepLen = breakStepLength (rate);
        juce::ignoreUnused (bpm);
        const int    steps = 32;
        juce::AudioBuffer<float> b (2, stepLen * steps);
        b.clear();
        juce::Random rnd (99);

        const char* kick  = kBreakKick;
        const char* snare = kBreakSnare;
        const char* hats  = kBreakHats;

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
        for (int pad = 0; pad < tb::kNumPads; ++pad)
            for (auto* n : { tb::params::padMode, tb::params::padSync, tb::params::padRate, tb::params::padChoke,
                             tb::params::padVelSens, tb::params::padMute, tb::params::padSolo, tb::params::padSlice })
                allPadParams &= p.getParameters().getParameter (tb::params::padId (pad, n)) != nullptr;
        for (int pad = 0; pad < tb::kNumPads; ++pad)
            for (auto* n : { tb::params::padSwap, tb::params::padSwapAt })
                allPadParams &= p.getParameters().getParameter (tb::params::padId (pad, n)) != nullptr;
        for (int pad = 0; pad < tb::kNumPads; ++pad)
            allPadParams &= p.getParameters().getParameter (tb::params::padId (pad, tb::params::padGroup)) != nullptr;
        check (allPadParams, "272 per-pad parameters exist (pad01_gain .. pad16_group)");
        check (tb::params::padId (6, tb::params::padPitch) == "pad07_pitch", "stable per-pad parameter IDs");
        check (p.getParameters().processor.getParameters().size() == 295, "295 automatable parameters in total (3 master + 17 x 16 pads + 5 x 4 groups)");

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


    //==========================================================================
    //  PHASE 3
    //==========================================================================
    juce::AudioBuffer<float> renderHeld (Rig& r, int note, int blocks, int releaseAfterBlock = -1)
    {
        juce::AudioBuffer<float> out (2, blocks * kBlock), buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 127), 0);
        for (int b = 0; b < blocks; ++b)
        {
            if (b == releaseAfterBlock) midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            runBlock (r.p, r.host, buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
        }
        return out;
    }

    float maxStepIn (const juce::AudioBuffer<float>& b, int ch, int from, int to)
    {
        float m = 0.0f;
        for (int i = juce::jmax (1, from); i < juce::jmin (to, b.getNumSamples()); ++i)
            m = juce::jmax (m, std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
        return m;
    }

    void testLoops (const juce::AudioBuffer<float>& src, const juce::File& tone)
    {
        std::cout << "\n== Phase 3: loops ==" << std::endl;
        Rig r;
        r.load (0, tone);
        const int len = src.getNumSamples();
        setPadParam (r.p, 0, tb::params::padMode, (float) tb::params::modeLoop);
        setPadParam (r.p, 0, tb::params::padSync, 0.0f);
        r.settle();

        const int blocks = (len * 4) / kBlock;
        auto out = renderHeld (r, 36, blocks);
        check (peakOf (out, out.getNumSamples() - kBlock) > 0.05f, "LOOP keeps playing past the end of the sample while held");
        float maxErr = 0.0f;
        for (int k = 0; k < 400; ++k)
            maxErr = juce::jmax (maxErr, std::abs (out.getSample (0, len + k) - src.getSample (0, k)),
                                         std::abs (out.getSample (0, 2 * len + 100 + k) - src.getSample (0, 100 + k)));
        check (maxErr < 1.0e-5f, "region starting at the top of the file loops sample-exactly (err " + juce::String (maxErr, 8)
                                    + ", out[len]=" + juce::String (out.getSample (0, len), 6) + " src[0]=" + juce::String (src.getSample (0, 0), 6)
                                    + ", out[len-1]=" + juce::String (out.getSample (0, len - 1), 6) + " src[len-1]=" + juce::String (src.getSample (0, len - 1), 6) + ")");

        out = renderHeld (r, 36, 6, 3);
        check (isSilent (peakOf (out, 4 * kBlock)), "note-off stops the loop (with a short fade)");
        check (maxStepIn (out, 0, 3 * kBlock - 2, 3 * kBlock + 300) < 0.05f, "...without a click");

        // Seam crossfade: a mid-file region whose ends don't match.
        setPadParam (r.p, 0, tb::params::padStart, 0.2f);
        setPadParam (r.p, 0, tb::params::padEnd, 0.55f);
        r.settle();
        out = renderHeld (r, 36, (len * 2) / kBlock);
        const float natural = maxStepIn (src.getNumChannels() > 0 ? out : out, 0, 300, (int) (len * 0.3));
        const int seam = (int) std::lround (len * 0.35);
        check (maxStepIn (out, 0, seam - 300, seam + 300) <= natural * 1.25f,
               "loop seam is crossfaded (no click where the end joins the start)");

        setPadParam (r.p, 0, tb::params::padStart, 0.0f);
        setPadParam (r.p, 0, tb::params::padEnd, 1.0f);
        setPadParam (r.p, 0, tb::params::padRate, 3.0f);   // 2x
        r.settle();
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            for (int b = 0; b < 10; ++b) runBlock (r.p, r.host, buf, midi);
            const float head = r.p.getBridge().padPlayhead[0].load();
            const float expected = std::fmod (10.0f * kBlock * 2.0f, (float) len) / (float) len;
            check (std::abs (head - expected) < 0.002f, "free loop at 2x runs twice as fast");
        }
        setPadParam (r.p, 0, tb::params::padRate, 2.0f);
        r.settle();

        // One-shots ignore note-off; the mouse release stops loops.
        setPadParam (r.p, 0, tb::params::padMode, (float) tb::params::modeOneShot);
        r.settle();
        out = renderHeld (r, 36, 6, 1);
        check (peakOf (out, 3 * kBlock) > 0.05f, "ONE-SHOT ignores note-off");

        setPadParam (r.p, 0, tb::params::padMode, (float) tb::params::modeLoop);
        r.settle();
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            r.p.triggerPadFromUi (0, 1.0f, true);
            runBlock (r.p, r.host, buf, midi);
            r.p.triggerPadFromUi (0, 0.0f, false);
            runBlock (r.p, r.host, buf, midi);
            runBlock (r.p, r.host, buf, midi);
            check (isSilent (peakOf (buf)), "releasing the mouse stops a LOOP pad");
        }
    }

    void testSync()
    {
        std::cout << "\n== Phase 3: tempo-synced loops ==" << std::endl;
        check (std::abs (tb::PadVoice::loopBeatsFor (4.0 * 60.0 / 140.0) - 4.0) < 1e-9, "a 4-beat break at 140 BPM is read as 4 beats");
        check (std::abs (tb::PadVoice::loopBeatsFor (4.0 * 60.0 / 85.0) - 8.0) < 1e-9, "an 85 BPM break is read in double time (8 beats, 170)");
        check (std::abs (tb::PadVoice::loopBeatsFor (1.0 * 60.0 / 165.0) - 1.0) < 1e-9, "a one-beat hit at 165 BPM is 1 beat");

        Rig r;
        const int len = (int) std::lround (4.0 * 60.0 / 140.0 * 48000.0);   // 4 beats at 140
        r.load (0, writeAudio ("break140", makeTone (len, 1), 48000.0, Fmt::wavFloat));
        setPadParam (r.p, 0, tb::params::padMode, (float) tb::params::modeLoop);
        setPadParam (r.p, 0, tb::params::padSync, 1.0f);
        r.settle();

        r.host.bpm = 170.0; r.host.ppq = 1.0; r.host.playing = true;   // start mid-bar, beat 2
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        auto phaseError = [&]
        {
            const double expected = std::fmod (r.host.ppq, 4.0) / 4.0;
            double err = std::abs (r.p.getBridge().padPlayhead[0].load() - expected);
            return juce::jmin (err, 1.0 - err);
        };
        check (phaseError() < 0.003, "triggered on beat 2, a synced loop enters at beat 2 of the break (on the grid)");

        double worst = 0.0;
        for (int b = 0; b < 600; ++b) { runBlock (r.p, r.host, buf, midi); worst = juce::jmax (worst, phaseError()); }
        check (worst < 0.002, "after ~6 s (4+ loops at 170 BPM) it is still locked to Ableton's bar grid (max err "
                                  + juce::String (worst, 5) + ")");

        r.host.ppq = 0.0;     // loop brace wraps back
        runBlock (r.p, r.host, buf, midi);
        check (phaseError() < 0.003, "re-locks instantly after a loop-brace jump");

        r.host.bpm = 120.0;   // tempo change
        for (int b = 0; b < 20; ++b) runBlock (r.p, r.host, buf, midi);
        check (phaseError() < 0.003, "follows a tempo change");

        setPadParam (r.p, 0, tb::params::padRate, 3.0f);   // 2x -> one cycle per 2 beats
        for (int b = 0; b < 20; ++b) runBlock (r.p, r.host, buf, midi);
        const double exp2 = std::fmod (r.host.ppq, 2.0) / 2.0;
        double err2 = std::abs (r.p.getBridge().padPlayhead[0].load() - exp2);
        err2 = juce::jmin (err2, 1.0 - err2);
        check (err2 < 0.003, "rate 2x: the break cycles every 2 beats, still on the grid");
        r.host.playing = false;
    }

    void testChokeMuteSolo (const juce::File& tone)
    {
        std::cout << "\n== Phase 3: choke, mute, solo, velocity, MIDI learn ==" << std::endl;
        Rig r;
        r.load (0, tone);
        r.load (1, tone);
        setPadParam (r.p, 0, tb::params::padPan, -1.0f);   // pad 1 left, pad 2 right
        setPadParam (r.p, 1, tb::params::padPan,  1.0f);
        setPadParam (r.p, 0, tb::params::padChoke, 1.0f);
        setPadParam (r.p, 1, tb::params::padChoke, 1.0f);
        r.settle();

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        midi.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (buf.getMagnitude (0, 200, kBlock - 200)) && buf.getMagnitude (1, 0, kBlock) > 0.05f,
               "pads in the same choke group cut each other (within 3 ms) (L " + juce::String (buf.getMagnitude (0, 200, kBlock - 200), 10)
                 + " R " + juce::String (buf.getMagnitude (1, 0, kBlock), 4) + ")");
        check (maxStepIn (buf, 0, 0, 200) < 0.05f, "...with a click-free fade");
        r.settle();

        setPadParam (r.p, 1, tb::params::padChoke, 2.0f);
        r.settle();
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        midi.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        check (buf.getMagnitude (0, 0, kBlock) > 0.05f && buf.getMagnitude (1, 0, kBlock) > 0.05f,
               "different choke groups play together");
        r.settle();

        setPadParam (r.p, 0, tb::params::padMute, 1.0f);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (buf.getMagnitude (0, 0, kBlock)) && buf.getMagnitude (1, 0, kBlock) > 0.05f, "MUTE silences the pad (L "
                 + juce::String (buf.getMagnitude (0, 0, kBlock), 10) + " R " + juce::String (buf.getMagnitude (1, 0, kBlock), 4) + ")");
        setPadParam (r.p, 0, tb::params::padMute, 0.0f);
        r.settle();

        setPadParam (r.p, 1, tb::params::padSolo, 1.0f);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127), 0);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (buf.getMagnitude (0, 0, kBlock)) && buf.getMagnitude (1, 0, kBlock) > 0.05f, "SOLO on pad 2 silences pad 1 (L "
                 + juce::String (buf.getMagnitude (0, 0, kBlock), 10) + " R " + juce::String (buf.getMagnitude (1, 0, kBlock), 4) + ")");
        setPadParam (r.p, 1, tb::params::padSolo, 0.0f);
        r.settle();

        setPadParam (r.p, 0, tb::params::padVelSens, 0.0f);
        r.settle();
        auto out = renderNote (r.p, r.host, 36, 20, 2);
        check (std::abs (out.getSample (0, 500) - out.getSample (0, 500) / (20.0f / 127.0f) * (20.0f / 127.0f)) < 1e-6f
                 && out.getMagnitude (0, 0, kBlock * 2) > 0.5f, "velocity sensitivity 0%: a soft hit plays at full level");
        setPadParam (r.p, 0, tb::params::padVelSens, 1.0f);
        r.settle();

        auto& e = r.p.getEngine();
        const auto v0 = e.getNoteMapVersion();
        e.startNoteLearn (5);
        midi.addEvent (juce::MidiMessage::noteOn (1, 70, (juce::uint8) 100), 0);
        runBlock (r.p, r.host, buf, midi);
        check (e.getPadNote (5) == 70 && e.getNoteLearnPad() == -1, "MIDI learn: next note played (70) is assigned to pad 6");
        check (r.p.getBridge().padTriggerCount[5].load() == 1, "...and the learning note plays the pad");
        check (e.getNoteMapVersion() != v0, "...and the change is flagged so the host saves it");

        e.startNoteLearn (2);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        runBlock (r.p, r.host, buf, midi);
        check (e.getPadNote (2) == 36 && e.getPadNote (0) == 38, "learning a note another pad uses swaps their notes (no duplicates)");

        juce::MemoryBlock blob;
        setPadParam (r.p, 3, tb::params::padMode, (float) tb::params::modeLoop);
        setPadParam (r.p, 3, tb::params::padChoke, 4.0f);
        r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        auto raw = [&] (int pad, const char* n) { return q.getParameters().getRawParameterValue (tb::params::padId (pad, n))->load(); };
        check (juce::roundToInt (raw (3, tb::params::padMode)) == tb::params::modeLoop && juce::roundToInt (raw (3, tb::params::padChoke)) == 4
                 && q.getEngine().getPadNote (5) == 70, "mode, choke and learned notes are saved with the set");
    }


    //==========================================================================
    //  0.5.1: controller pad lights (iRig Pads)
    //==========================================================================
    struct RecordingSink final : tb::PadLights::Sink
    {
        std::vector<juce::MidiMessage> msgs;
        void send (const juce::MidiMessage& m) override { msgs.push_back (m); }
        int last (int cc) const
        {
            for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
                if (it->isController() && it->getControllerNumber() == cc) return it->getControllerValue();
            return -1;
        }
    };

    void testPadLights (const juce::File& tone)
    {
        std::cout << "\n== 0.5.1: iRig Pads lights ==" << std::endl;
        using tb::PadLights;
        check (PadLights::ccFor (0, true, false) == 94 && PadLights::ccFor (0, false, false) == 95
                 && PadLights::ccFor (12, true, false) == 70 && PadLights::ccFor (3, false, false) == 101,
               "iRig Pads LED CCs: pad 1 = 94 / 95, pad 4 = 100 / 101, pad 13 = 70 / 71 (manual)");
        check (PadLights::ccFor (0, true, true) == 95, "SWAP COLOURS flips red and green");

        Rig r;
        auto& lights = r.p.getLights();
        RecordingSink rec;
        lights.setSinkForTesting (&rec);

        // Plugin pad 1 = note 36 = C1 = iRig default-scene pad 2; plugin pad 3 = D1 = iRig pad 3.
        check (lights.hardwarePadFor (0) == 1 && lights.hardwarePadFor (2) == 2 && lights.hardwarePadFor (6) == 6,
               "default scene: each plugin pad lights the iRig pad that plays its note");

        r.load (0, tone);
        lights.tick (0.0);
        check (rec.last (97) == 6 && rec.last (96) == 0, "a loaded, idle pad glows dim green");
        check (rec.msgs.front().getChannel() == 1, "...sent on MIDI channel 1");
        const auto before = rec.msgs.size();
        lights.tick (33.0);
        check (rec.msgs.size() == before, "nothing changes, nothing is sent (no MIDI flood)");

        renderNote (r.p, r.host, 36, 127, 1);
        lights.tick (66.0);
        check (rec.last (97) > 100, "while it plays: bright green (" + juce::String (rec.last (97)) + ")");
        check (rec.last (99) == 0 && rec.last (98) == 0, "...and only that pad lights (empty pad 3 stays off)");
        for (int b = 0; b < 150; ++b) { juce::AudioBuffer<float> buf (2, kBlock); juce::MidiBuffer m; runBlock (r.p, r.host, buf, m); }
        lights.tick (100.0);
        check (rec.last (97) == 6, "...and back to dim when the sound has died away");

        // A queued SWAP flashes red on the incoming pad.
        r.load (1, tone);
        for (int p = 0; p < 2; ++p)
        {
            setPadParam (r.p, p, tb::params::padMode, (float) tb::params::modeLoop);
            setPadParam (r.p, p, tb::params::padSwap, 1.0f);
        }
        r.settle();
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            runBlock (r.p, r.host, buf, m);
            m.addEvent (juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127), 100);
            runBlock (r.p, r.host, buf, m);
        }
        // Plugin pad 2 = C#1 = iRig pad 5 (CC 86 red / 87 green).
        lights.tick (4000.0);
        const int on = rec.last (86);
        lights.tick (4125.0);
        const int off = rec.last (86);
        check (on == 127 && off == 0, "a queued SWAP flashes the incoming pad red");
        r.p.requestPanic();
        r.settle();

        // LEARN a custom layout: iRig pad k plays plugin pad 15 - k.
        lights.startLearn();
        lights.tick (5000.0);
        check (rec.last (94) == 127, "LEARN lights the first iRig pad red");
        for (int k = 0; k < 16; ++k)
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 36 + 15 - k, (juce::uint8) 100), 0);
            runBlock (r.p, r.host, buf, m);
            lights.tick (5100.0 + k * 40.0);
            if (k == 0) check (rec.last (94) == 0 && rec.last (96) == 127, "...hit it, and the next one lights up");
        }
        bool ok = ! lights.isLearning();
        for (int p = 0; p < 16; ++p) ok &= lights.hardwarePadFor (p) == 15 - p;
        check (ok, "after 16 hits the layout is learned (any iRig scene works)");

        bool changed = false;
        lights.onSettingsChanged = [&] { changed = true; };
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (changed, "...and the host is told to save it");

        lights.setSwapColours (true);
        juce::MemoryBlock blob;
        r.p.getStateInformation (blob);
        lights.setSinkForTesting (nullptr);
        {
            TwistedBreaksProcessor q;
            q.setStateInformation (blob.getData(), (int) blob.getSize());
            check (q.getLights().getSwapColours() && q.getLights().hardwarePadFor (3) == 12,
                   "learned layout and colour setting are saved with the set");
        }

        {
            RecordingSink rec2;
            {
                tb::PadLights l2 (r.p.getBridge(), r.p.getEngine());
                l2.setSinkForTesting (&rec2);
                l2.tick (0.0);
            }
            bool allOff = rec2.msgs.size() >= 32;
            for (size_t i = rec2.msgs.size() - 32; allOff && i < rec2.msgs.size(); ++i) allOff &= rec2.msgs[i].getControllerValue() == 0;
            check (allOff, "closing the plugin turns every pad light off");
        }
    }

    //==========================================================================
    //  PHASE 5: group buses
    //==========================================================================
    void testGroups (const juce::File& tone)
    {
        std::cout << "\n== Phase 5: group buses A-D ==" << std::endl;
        using namespace tb::params;
        TwistedBreaksProcessor probe;
        bool all = true;
        for (int g = 0; g < tb::kNumGroups; ++g)
            for (auto* n : { grpLevel, grpPan, grpMute, grpSolo, grpBypass })
                all &= probe.getParameters().getParameter (groupId (g, n)) != nullptr;
        check (all && groupId (2, grpLevel) == "grp_c_level", "4 groups x level / pan / mute / solo / FX bypass, stable IDs (grp_c_level)");
        check (juce::roundToInt (probe.getParameters().getRawParameterValue (padId (9, padGroup))->load()) == 0,
               "pads start in group A (older sets sound the same)");

        Rig r;
        r.load (0, tone);
        r.load (1, tone);
        setPadParam (r.p, 1, padGroup, 1.0f);              // pad 2 -> group B
        r.settle();

        auto grp = [&] (int g, const char* n, float v) { setParam (r.p, groupId (g, n), v); };
        auto& br = r.p.getBridge();
        for (auto& gp : br.groupPeak) gp.exchange (0.0f);

        const auto ref = renderNote (r.p, r.host, 37, 127, 2);
        check (br.groupPeak[1].exchange (0.0f) > 0.1f && br.groupPeak[0].exchange (0.0f) == 0.0f,
               "pad 2 routed to group B: B's meter moves, A's doesn't");

        grp (1, grpLevel, -6.0f);
        r.settle();
        auto out = renderNote (r.p, r.host, 37, 127, 2);
        const float ratio = out.getSample (0, 700) / ref.getSample (0, 700);
        check (std::abs (ratio - juce::Decibels::decibelsToGain (-6.0f)) < 1.0e-4f,
               "group B LEVEL -6 dB scales pad 2 by exactly -6 dB (x" + juce::String (ratio, 5) + ")");

        r.settle();
        out = renderNote (r.p, r.host, 36, 127, 2);
        check (std::abs (out.getSample (0, 700) - ref.getSample (0, 700)) < 1.0e-6f, "...while pad 1 (group A) is untouched ("
                 + juce::String (out.getSample (0, 700), 6) + " vs " + juce::String (ref.getSample (0, 700), 6) + ")");

        grp (1, grpLevel, 0.0f);
        grp (1, grpPan, 1.0f);
        r.settle();
        out = renderNote (r.p, r.host, 37, 127, 2);
        check (isSilent (out.getMagnitude (0, 0, out.getNumSamples())) && out.getMagnitude (1, 0, out.getNumSamples()) > 0.1f,
               "group PAN hard right: group B only on the right");
        grp (1, grpPan, 0.0f);

        grp (0, grpMute, 1.0f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, 2);
        r.settle();
        auto out2 = renderNote (r.p, r.host, 37, 127, 2);
        check (isSilent (peakOf (out)) && peakOf (out2) > 0.1f, "group MUTE silences group A only");
        grp (0, grpMute, 0.0f);

        grp (1, grpSolo, 1.0f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, 2);
        r.settle();
        out2 = renderNote (r.p, r.host, 37, 127, 2);
        check (isSilent (peakOf (out)) && peakOf (out2) > 0.1f, "group SOLO on B silences group A");
        grp (1, grpSolo, 0.0f);

        // Re-routing a pad: it now follows its new group's fader.
        grp (1, grpLevel, -12.0f);
        setPadParam (r.p, 0, padGroup, 1.0f);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, 2);
        check (std::abs (out.getSample (0, 700) / ref.getSample (0, 700) - juce::Decibels::decibelsToGain (-12.0f)) < 1.0e-4f,
               "moving pad 1 to group B: it follows B's level");
        setPadParam (r.p, 0, padGroup, 0.0f);
        grp (1, grpLevel, 0.0f);
        r.settle();

        // Muting a group while a loop plays: a 20 ms glide, no click.
        setPadParam (r.p, 0, padMode, (float) modeLoop);
        setPadParam (r.p, 0, padSync, 0.0f);
        r.settle();
        {
            juce::AudioBuffer<float> all (2, 12 * kBlock), buf (2, kBlock);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            for (int b = 0; b < 12; ++b)
            {
                if (b == 4) grp (0, grpMute, 1.0f);
                runBlock (r.p, r.host, buf, midi);
                for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            }
            const float natural = maxStepIn (all, 0, 100, 4 * kBlock);
            check (maxStepIn (all, 0, 4 * kBlock - 2, 6 * kBlock) <= natural * 1.05f && isSilent (all.getMagnitude (0, 7 * kBlock, 5 * kBlock)),
                   "muting a group mid-loop fades it out (no click) and it goes silent");
        }
        grp (0, grpMute, 0.0f);
        setPadParam (r.p, 0, padMode, (float) modeOneShot);
        r.settle();

        // A host block bigger than announced still plays through the buses correctly.
        {
            const auto blocks = renderNote (r.p, r.host, 36, 127, 6);
            r.settle();
            juce::AudioBuffer<float> big (2, 6 * kBlock);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            runBlock (r.p, r.host, big, midi);
            float diff = 0.0f;
            for (int i = 0; i < big.getNumSamples(); ++i) diff = juce::jmax (diff, std::abs (big.getSample (0, i) - blocks.getSample (0, i)));
            check (diff < 1.0e-6f, "a 3072-sample host block (prepared for 512) renders identically (diff " + juce::String (diff, 8) + ")");
        }

        // State.
        grp (2, grpLevel, -9.0f); grp (2, grpPan, -0.4f); grp (3, grpMute, 1.0f); grp (1, grpBypass, 1.0f);
        setPadParam (r.p, 7, padGroup, 3.0f);
        juce::MemoryBlock blob;
        r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        auto raw = [&] (const juce::String& id) { return q.getParameters().getRawParameterValue (id)->load(); };
        check (std::abs (raw (groupId (2, grpLevel)) + 9.0f) < 0.01f && std::abs (raw (groupId (2, grpPan)) + 0.4f) < 0.01f
                 && raw (groupId (3, grpMute)) > 0.5f && raw (groupId (1, grpBypass)) > 0.5f
                 && juce::roundToInt (raw (padId (7, padGroup))) == 3,
               "group levels, pans, mutes, bypass and pad routing are saved with the set");
    }

    void testMixerView()
    {
        std::cout << "\n== Phase 5: MIXER view ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);

        tb::ui::MainView* view = nullptr;
        tb::ui::MixerPanel* mixer = nullptr;
        tb::ui::WaveformEditor* crt = nullptr;
        juce::Button* mixerKey = nullptr;
        juce::Button* groupKeyD = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            if (auto* m = dynamic_cast<tb::ui::MixerPanel*> (&c)) mixer = m;
            if (auto* w = dynamic_cast<tb::ui::WaveformEditor*> (&c)) crt = w;
            if (auto* sk = dynamic_cast<tb::ui::SelectorKey*> (&c)) if (sk->getName() == "MIXER") mixerKey = sk;
            if (auto* b = dynamic_cast<juce::Button*> (&c)) if (b->getTooltip() == "Send this pad to group bus D") groupKeyD = b;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (view && mixer && crt && mixerKey && groupKeyD, "found the MIXER key, mixer, CRT and GROUP keys");
        if (! (view && mixer && crt && mixerKey && groupKeyD)) return;

        check (mixerKey->isEnabled(), "MIXER key is online");
        mixerKey->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (mixer->isVisible() && ! crt->isVisible() && mixerKey->getToggleState(), "MIXER shows the four strips in place of the CRT");

        // Drag group B's fader down with the mouse.
        juce::Slider* faderB = nullptr;
        for (auto* c : mixer->getChildren())
            if (auto* sl = dynamic_cast<juce::Slider*> (c)) if (sl->getTooltip().startsWith ("GROUP B level")) faderB = sl;
        check (faderB != nullptr, "found group B's fader");
        if (faderB != nullptr)
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto t0 = juce::Time::getCurrentTime();
            const auto start = faderB->getLocalBounds().getCentre().toFloat();
            auto ev = [&] (juce::Point<float> pos, int ms, bool down)
            {
                return juce::MouseEvent (src, pos, down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                         1.0f, 0, 0, 0, 0, faderB, faderB, t0 + juce::RelativeTime::milliseconds (ms), start, t0, 1, false);
            };
            faderB->mouseDown (ev (start, 0, true));
            for (int i = 1; i <= 5; ++i) faderB->mouseDrag (ev (start.translated (0.0f, 8.0f * (float) i), i * 10, true));
            faderB->mouseUp (ev (start.translated (0.0f, 40.0f), 70, false));
            const float lvl = r.p.getParameters().getRawParameterValue (tb::params::groupId (1, tb::params::grpLevel))->load();
            check (lvl < -3.0f, "dragging group B's fader down lowers grp_b_level (" + juce::String (lvl, 1) + " dB)");
        }

        // Click GROUP C's header: the selected pad (1) is routed to C.
        {
            const auto hb = mixer->headerBounds (2).getCentre().toFloat();
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto t0 = juce::Time::getCurrentTime();
            juce::MouseEvent e (src, hb, juce::ModifierKeys(), 1.0f, 0, 0, 0, 0, mixer, mixer, t0, hb, t0, 1, false);
            mixer->mouseUp (e);
            check (juce::roundToInt (r.p.getParameters().getRawParameterValue (tb::params::padId (0, tb::params::padGroup))->load()) == 2,
                   "clicking GROUP C's plate routes the selected pad to group C");
        }

        groupKeyD->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (juce::roundToInt (r.p.getParameters().getRawParameterValue (tb::params::padId (0, tb::params::padGroup))->load()) == 3
                 && groupKeyD->getToggleState(), "SOUND page GROUP key D routes the pad to group D");

        view->showMixer (false);
        check (! mixer->isVisible() && crt->isVisible(), "PADS / SLICER bring the CRT back");
    }

    //==========================================================================
    //  0.4.1: SWAP groups
    //==========================================================================
    struct TimedEvent { int at; juce::MidiMessage msg; };

    /** Renders `total` samples from the current host position, injecting events at absolute sample positions. */
    juce::AudioBuffer<float> renderTimeline (Rig& r, int total, const std::vector<TimedEvent>& events,
                                             std::vector<bool>* queued0 = nullptr)
    {
        const int blocks = (total + kBlock - 1) / kBlock;
        juce::AudioBuffer<float> out (2, blocks * kBlock), buf (2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            for (const auto& e : events)
                if (e.at >= b * kBlock && e.at < (b + 1) * kBlock)
                    midi.addEvent (e.msg, e.at - b * kBlock);
            runBlock (r.p, r.host, buf, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            if (queued0 != nullptr) queued0->push_back (r.p.getBridge().padQueued[0].load());
        }
        return out;
    }

    int firstSoundIn (const juce::AudioBuffer<float>& b, int ch, int from, int to)
    {
        for (int i = juce::jmax (0, from); i < juce::jmin (to, b.getNumSamples()); ++i)
            if (std::abs (b.getSample (ch, i)) > 1.0e-7f) return i;
        return -1;
    }

    void testSwapGroups()
    {
        std::cout << "\n== 0.4.1: SWAP groups (hand-off between breaks at slice points) ==" << std::endl;

        // Break A on pad 1 (panned hard left); break B on pad 2 (hard right) is the
        // same groove played 100 samples LATE (laid back), so its own markers sit
        // 100 samples after A's. Both two bars, equal 16 slices.
        const auto aAudio = makeBreak (48000.0);
        const int  len    = aAudio.getNumSamples();
        juce::AudioBuffer<float> bAudio (2, len);
        bAudio.clear();
        for (int ch = 0; ch < 2; ++ch) bAudio.copyFrom (ch, 100, aAudio, ch, 0, len - 100);
        const auto fileA = writeAudio ("swapA", aAudio, 48000.0, Fmt::wavFloat);
        const auto fileB = writeAudio ("swapB", bAudio, 48000.0, Fmt::wavFloat);

        Rig r;
        auto& sm = r.p.getSamples();
        r.load (0, fileA);
        r.load (1, fileB);
        sm.setSliceCount (0, 16); sm.equalSlice (0);
        auto tb16 = tb::SliceTable::equal (len, 16);
        for (int i = 1; i < tb16.count; ++i) tb16.starts[(size_t) i] += 100;
        sm.setSlices (1, tb16);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        for (int p = 0; p < 2; ++p)
        {
            setPadParam (r.p, p, tb::params::padMode, (float) tb::params::modeLoop);
            setPadParam (r.p, p, tb::params::padSync, 1.0f);
            setPadParam (r.p, p, tb::params::padSwap, 1.0f);
            setPadParam (r.p, p, tb::params::padPan, p == 0 ? -1.0f : 1.0f);
        }
        r.settle();

        const auto aTable = sm.getInfo (0).slices;
        check (aTable.count == 16 && sm.getInfo (1).slices.starts[3] == aTable.starts[3] + 100, "two sliced breaks in SWAP group 1");

        // Tempo at which a 2-bar (8-beat) break plays at exactly its own speed.
        r.host.bpm = 60.0 * 8.0 * 48000.0 / (double) len;
        r.host.ppq = 0.0;
        r.host.playing = true;

        auto on  = [] (int note) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) 127); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };

        const int hitB  = 21000;                       // mid slice 3 of A
        const int S     = aTable.starts[3];            // A's next marker: the hand-off point
        const int stopB = S + 30000;                   // hit B again: stop at B's next marker
        int nextB = -1;
        for (int i = 0; i < 16 && nextB < 0; ++i)
            if (tb16.starts[(size_t) i] > stopB - S + tb16.starts[3]) nextB = tb16.starts[(size_t) i];
        const int stopAt = S + (nextB - tb16.starts[3]);

        std::vector<bool> q0;
        auto out = renderTimeline (r, stopAt + 6000,
                                   { { 0, on (36) }, { 600, off (36) },        // latched: note-off ignored
                                     { hitB, on (37) }, { hitB + 300, off (37) },
                                     { stopB, on (37) } });

        check (firstSoundIn (out, 0, 0, 2000) >= 0 && firstSoundIn (out, 0, 1000, 2000) >= 0,
               "a pad in a SWAP group latches: it keeps playing after note-off");
        check (firstSoundIn (out, 1, 0, S) < 0, "hitting pad 2 mid-slice does NOT cut in straight away (queued)");
        const int firstB = firstSoundIn (out, 1, S - 2000, S + 2000);
        check (std::abs (firstB - S) <= 2, "pad 2 takes over exactly on break A's next slice marker (at "
                                             + juce::String (firstB) + ", marker " + juce::String (S) + ")");
        check (isSilent (out.getMagnitude (0, S + 120, 3000)), "break A stops right there (1 ms fade, its next hit doesn't leak)");
        bool faded = true;
        for (int k = 0; k < 120; ++k)
            faded &= std::abs (out.getSample (0, S + k)) <= std::abs (aAudio.getSample (0, S + k)) * juce::jmax (0.0f, 1.0f - (float) k / 48.0f) + 1.0e-4f;
        check (faded, "...faded out over 1 ms, not hard-cut (no click)");

        const int mB = tb16.starts[3];
        float err = 0.0f;
        for (int k = 48; k < 448; ++k)
            err = juce::jmax (err, std::abs (out.getSample (1, S + k) - bAudio.getSample (1, mB + k)));
        check (err < 5.0e-3f, "break B continues from the same point, starting ON its own slice marker (its hit isn't cut) (err "
                                 + juce::String (err, 6) + ")");

        const int stopFirstSilent = stopAt + 200;
        check (! isSilent (out.getMagnitude (1, stopAt - 1500, 1400)) && isSilent (out.getMagnitude (1, stopFirstSilent, 3000)),
               "hitting the playing pad again stops it at its next slice point");

        // B stays on the grid (with its 100-sample lead) while it plays: check the
        // playhead over a long run.
        r.settle();
        r.host.ppq = 0.0;
        renderTimeline (r, 30000, { { 0, on (36) }, { 21000, on (37) } });
        double worst = 0.0;
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < 400; ++b)
        {
            runBlock (r.p, r.host, buf, midi);
            const double grid = std::fmod (r.host.ppq, 8.0) / 8.0 * len;
            double e = std::abs (r.p.getBridge().padPlayhead[1].load() * len - (grid + 100.0));
            e = juce::jmin (e, len - e);
            worst = juce::jmax (worst, e);
        }
        check (worst < 3.0, "after the swap, break B stays locked to Live's grid for 4+ s (max err "
                               + juce::String (worst, 2) + " samples)");
        check (buf.getMagnitude (0, 0, kBlock) < 1.0e-9f, "...and break A stays stopped");

        // SWAP AT = BAR on pad 1: hit it while pad 2 plays; it lands on the next bar line.
        r.settle();
        r.host.ppq = 0.0;
        setPadParam (r.p, 0, tb::params::padSwapAt, (float) tb::params::swapAtBar);
        const double samplesPerBar = 4.0 * 60.0 / r.host.bpm * 48000.0;
        q0.clear();
        out = renderTimeline (r, 150000, { { 1000, on (37) }, { 80000, on (36) } }, &q0);
        const int bar = (int) std::lround (std::ceil (80000.0 / samplesPerBar) * samplesPerBar);
        const int firstA = firstSoundIn (out, 0, 0, 150000);
        check (std::abs (firstA - bar) <= 2, "SWAP AT BAR: lands on the next bar line (at " + juce::String (firstA)
                                               + ", bar " + juce::String (bar) + ")");
        check (q0[(size_t) (82000 / kBlock)] && ! q0.back(), "the queued pad flashes until the swap lands");
        setPadParam (r.p, 0, tb::params::padSwapAt, (float) tb::params::swapAtSlice);

        // Pressing the playing pad while a swap is queued cancels it.
        r.settle();
        r.host.ppq = 0.0;
        out = renderTimeline (r, 40000, { { 0, on (36) }, { 21000, on (37) }, { 22000, on (36) } });
        check (firstSoundIn (out, 1, 0, 40000) < 0 && ! isSilent (out.getMagnitude (0, 30000, 5000)),
               "hitting the playing pad while a swap is queued cancels the swap");

        // Live stopped: same fraction of the way through, still on A's slice point.
        r.settle();
        r.host.playing = false;
        out = renderTimeline (r, 40000, { { 0, on (36) }, { hitB, on (37) } });
        const int firstStopped = firstSoundIn (out, 1, 0, 40000);
        err = 0.0f;
        for (int k = 48; k < 448; ++k)
            err = juce::jmax (err, std::abs (out.getSample (1, S + k) - bAudio.getSample (1, mB + k)));
        check (std::abs (firstStopped - S) <= 2 && err < 1.0e-2f,
               "with Live stopped it still swaps on A's slice point, onto B's matching slice (at " + juce::String (firstStopped)
                 + ", err " + juce::String (err, 5) + ")");

        // Panic clears everything; pads not in a group behave as before.
        r.p.requestPanic();
        runBlock (r.p, r.host, buf, midi);
        runBlock (r.p, r.host, buf, midi);
        check (isSilent (peakOf (buf)), "PANIC stops a latched SWAP pad");
        setPadParam (r.p, 0, tb::params::padSwap, 0.0f);
        r.settle();
        auto held = renderHeld (r, 36, 8, 3);
        check (isSilent (peakOf (held, 5 * kBlock)), "SWAP off: a LOOP pad stops on note-off again");

        juce::MemoryBlock blob;
        setPadParam (r.p, 4, tb::params::padSwap, 3.0f);
        setPadParam (r.p, 4, tb::params::padSwapAt, 2.0f);
        r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        auto raw = [&] (int pad, const char* n) { return q.getParameters().getRawParameterValue (tb::params::padId (pad, n))->load(); };
        check (juce::roundToInt (raw (4, tb::params::padSwap)) == 3 && juce::roundToInt (raw (4, tb::params::padSwapAt)) == 2,
               "SWAP group and AT are saved with the set");
    }

    //==========================================================================
    //  PHASE 4
    //==========================================================================
    void testTransientDetection()
    {
        std::cout << "\n== Phase 4: transient detection ==" << std::endl;
        const double rate = 44100.0;
        tb::SampleBuffer buf;
        buf.audio = makeBreak (rate);
        buf.sourceSampleRate = rate;
        const int step = breakStepLength (rate);

        std::vector<int> hits, loud;
        for (int i = 0; i < 32; ++i)
        {
            const bool k = kBreakKick[i] == 'x', sn = kBreakSnare[i] == 'x', h = kBreakHats[i] == 'x';
            if (k || sn || h) hits.push_back (i * step);
            if (k || sn)      loud.push_back (i * step);
        }

        const auto t = tb::TransientDetector::detect (buf, 0.5f, 16);
        check (t.count == 16 && t.starts[0] == 0, "AUTO SLICE on a 2-bar break: 16 slices, the first at the start ("
                                                      + juce::String (t.count) + ")");

        auto nearestHit = [&] (int pos)
        {
            int best = 1 << 30;
            for (int h : hits) best = juce::jmin (best, std::abs (pos - h));
            return best;
        };
        int worst = 0;
        for (int i = 0; i < t.count; ++i) worst = juce::jmax (worst, nearestHit (t.starts[(size_t) i]));
        check (worst <= (int) (rate * 0.004), "every slice starts on a real hit (worst " + juce::String (worst * 1000.0 / rate, 2) + " ms off)");

        int missed = 0, late = 0;
        for (int h : loud)
        {
            bool found = false;
            for (int i = 0; i < t.count; ++i)
            {
                const int d = t.starts[(size_t) i] - h;
                if (d >= -(int) (rate * 0.004) && d <= (int) (rate * 0.002)) { found = true; break; }
                if (d > (int) (rate * 0.002) && d < (int) (rate * 0.01)) ++late;
            }
            if (! found) ++missed;
        }
        check (missed == 0, "every kick and snare gets its own slice (" + juce::String ((int) loud.size() - missed) + " / " + juce::String ((int) loud.size()) + ")");
        check (late == 0, "no slice starts late (after the attack)");

        int tooEarly = 0;
        for (int i = 1; i < t.count; ++i)
            if (nearestHit (t.starts[(size_t) i]) > (int) (rate * 0.0025)) ++tooEarly;
        check (tooEarly == 0, "slices start just before the attack (<= 2.5 ms), so the hit keeps its punch");

        const auto few  = tb::TransientDetector::findOnsets (buf, 0.0f).size();
        const auto many = tb::TransientDetector::findOnsets (buf, 1.0f).size();
        check (many >= few && many >= hits.size() - 2, "higher SENSITIVITY finds more hits (" + juce::String ((int) few) + " -> " + juce::String ((int) many) + ")");

        const auto eight = tb::TransientDetector::detect (buf, 0.5f, 8);
        int eightOnLoud = 0;
        for (int i = 1; i < eight.count; ++i)
            for (int h : loud) if (std::abs (eight.starts[(size_t) i] - h) < (int) (rate * 0.004)) { ++eightOnLoud; break; }
        check (eight.count == 8 && eightOnLoud == 7, "COUNT 8 keeps the 8 strongest hits (all kicks / snares)");

        tb::SampleBuffer flat;
        flat.audio = makeTone (48000, 1);
        flat.sourceSampleRate = 48000.0;
        const auto none = tb::TransientDetector::detect (flat, 0.5f, 16);
        check (none.count <= 2, "a sustained tone gives no false slices (" + juce::String (none.count) + ")");
    }

    void testSlicing (const juce::AudioBuffer<float>& src, const juce::File& tone)
    {
        std::cout << "\n== Phase 4: slices, edits, playback, SPREAD ==" << std::endl;
        Rig r;
        auto& sm = r.p.getSamples();
        r.load (0, tone);
        const int len = src.getNumSamples();

        sm.setSliceCount (0, 4);
        sm.equalSlice (0);
        auto t = sm.getInfo (0).slices;
        check (t.count == 4 && t.starts[1] == len / 4 && t.starts[3] == 3 * len / 4, "EQUAL makes 4 equal slices");

        sm.moveMarker (0, 1, 2000);
        check (sm.getInfo (0).slices.starts[1] == 2000, "drag a marker");
        sm.moveMarker (0, 1, 999999);
        check (sm.getInfo (0).slices.starts[1] == len / 2 - tb::SliceTable::kMinGap, "a marker can't be dragged past its neighbour");
        sm.addMarker (0, 9500);
        check (sm.getInfo (0).slices.count == 5 && sm.getInfo (0).slices.starts[4] == 9500, "add a marker (kept in order)");
        sm.deleteMarker (0, 4);
        check (sm.getInfo (0).slices.count == 4, "delete a marker");
        sm.resetSlices (0);
        check (sm.getInfo (0).slices == tb::SliceTable::equal (len, 4), "RESET goes back to the last AUTO / EQUAL result");
        bool full = true;
        sm.setSliceCount (0, 16); sm.equalSlice (0);
        full &= ! sm.addMarker (0, 123);
        check (full && sm.getInfo (0).slices.count == 16, "never more than 16 slices");
        sm.setSliceCount (0, 4); sm.equalSlice (0);

        // SLICE mode plays exactly the chosen slice.
        setPadParam (r.p, 0, tb::params::padMode, (float) tb::params::modeSlice);
        setPadParam (r.p, 0, tb::params::padSlice, 3.0f);
        r.settle();
        auto out = renderNote (r.p, r.host, 36, 127, (len / kBlock) + 4);
        check (std::abs ((lastSound (out) + 1) - len / 4) <= 2, "SLICE mode plays only the chosen slice (slice 3 = a quarter of the file)");
        check (std::abs (out.getSample (0, 300) - src.getSample (0, len / 2 + 300)) < 1.0e-5f, "...from the right place");
        r.settle();

        // Live edit while the pad plays: moving the next marker shortens it.
        sm.moveMarker (0, 3, len / 2 + 1000);
        r.settle();
        out = renderNote (r.p, r.host, 36, 127, (len / kBlock) + 4);
        check (std::abs ((lastSound (out) + 1) - 1000) <= 2, "moving a marker changes the slice immediately (non-destructive)");
        sm.resetSlices (0);
        r.settle();

        // AUTO SLICE in the background on a real break file.
        const auto brk = writeAudio ("autobreak", makeBreak (44100.0), 44100.0, Fmt::wav16);
        r.load (1, brk);
        sm.autoSlice (1);
        check (sm.getInfo (1).slicing, "AUTO SLICE runs in the background (pad shows it's working)");
        waitForLoads (r.p);
        check (sm.getInfo (1).slices.count == 16 && ! sm.getInfo (1).slicing, "...and lands 16 markers on the break");

        // SPREAD: slices onto pads, sharing one decoded copy.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        const int before = tb::SampleBuffer::liveInstances();
        setPadParam (r.p, 1, tb::params::padChoke, 2.0f);
        const int filled = r.p.spreadSlices (1);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (filled == 15, "SPREAD from pad 2 fills pads 2-16 with slices 1-15");
        check (tb::SampleBuffer::liveInstances() == before, "...all sharing one decoded copy (no extra memory)");
        auto raw = [&] (int pad, const char* n) { return r.p.getParameters().getRawParameterValue (tb::params::padId (pad, n))->load(); };
        check (juce::roundToInt (raw (5, tb::params::padMode)) == tb::params::modeSlice && juce::roundToInt (raw (5, tb::params::padSlice)) == 5
                 && juce::roundToInt (raw (5, tb::params::padChoke)) == 2, "...each pad in SLICE mode on its own slice, choke copied");

        const auto table = sm.getInfo (1).slices;
        r.settle();
        {
            // pad 5 = slice 4: after one block the read head must be inside slice 4.
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 127), 0);
            runBlock (r.p, r.host, buf, midi);
            const int brkLen = sm.getInfo (4).buffer->getNumSamples();
            const double expected = (table.starts[3] + kBlock * 44100.0 / 48000.0) / brkLen;
            check (std::abs (r.p.getBridge().padPlayhead[4].load() - expected) < 2.0 / brkLen,
                   "pad 5 plays slice 4 of the break, from its first sample");
        }
        r.settle();

        // Save / reload: markers, detected (RESET), sensitivity, count; one decode for 15 pads.
        sm.setSensitivity (1, 0.8f);
        sm.moveMarker (1, 2, table.starts[2] + 500);
        juce::MemoryBlock blob;
        r.p.getStateInformation (blob);
        const auto savedSlices = sm.getInfo (1).slices;

        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        {
            Rig q;
            const int base = tb::SampleBuffer::liveInstances();
            q.p.setStateInformation (blob.getData(), (int) blob.getSize());
            waitForLoads (q.p);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            const auto info = q.p.getSamples().getInfo (1);
            check (info.slices == savedSlices && info.detected == table && std::abs (info.sensitivity - 0.8f) < 1e-6f,
                   "slice markers, RESET point and SENS are saved with the set");
            check (tb::SampleBuffer::liveInstances() - base == 2, "reopening 15 sliced pads decodes the break once ("
                     + juce::String (tb::SampleBuffer::liveInstances() - base - 1) + " copy) + the tone");
            check (q.p.getSamples().getInfo (15).buffer == info.buffer, "...and they share it");
        }
    }


    void testSliceEditingOnScreen (const juce::File& tone)
    {
        std::cout << "\n== Phase 4: editing slices on the screen ==" << std::endl;
        Rig r;
        r.load (0, tone);
        auto& sm = r.p.getSamples();
        const int len = sm.getInfo (0).buffer->getNumSamples();

        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        tb::ui::WaveformEditor* screen = nullptr;
        tb::ui::PadEditorPanel* panel = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* w = dynamic_cast<tb::ui::WaveformEditor*> (&c)) screen = w;
            if (auto* pe = dynamic_cast<tb::ui::PadEditorPanel*> (&c)) panel = pe;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (screen != nullptr && panel != nullptr, "found the screen and the pad editor");
        if (screen == nullptr || panel == nullptr) return;

        panel->setPage (tb::ui::PadEditorPanel::Page::slice);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        // Same geometry the screen uses: glass = bounds.reduced (20, 18); plot = glass.reduced (14, 0).
        const auto plot = screen->getLocalBounds().toFloat().reduced (20.0f, 18.0f).reduced (14.0f, 0.0f);
        auto xFor = [&] (int sample) { return plot.getX() + (float) sample / (float) len * plot.getWidth(); };
        const float y = plot.getCentreY();

        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto t0 = juce::Time::getCurrentTime();
        int ms = 0;
        auto ev = [&] (float x, bool down, int clicks = 1)
        {
            ms += 400;
            return juce::MouseEvent (src, { x, y }, down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                     1.0f, 0, 0, 0, 0, screen, screen, t0 + juce::RelativeTime::milliseconds (ms),
                                     { x, y }, t0 + juce::RelativeTime::milliseconds (ms), clicks, false);
        };

        screen->mouseDoubleClick (ev (xFor (len / 2), false, 2));
        auto tbl = sm.getInfo (0).slices;
        check (tbl.count == 2 && tbl.starts[0] == 0 && std::abs (tbl.starts[1] - len / 2) < len / 200,
               "double-click on the screen adds a marker (pad gets slices 1 and 2)");

        screen->mouseDown (ev (xFor (tbl.starts[1]), true));
        screen->mouseDrag (ev (xFor (len * 3 / 4), true));
        screen->mouseUp (ev (xFor (len * 3 / 4), false));
        tbl = sm.getInfo (0).slices;
        check (std::abs (tbl.starts[1] - len * 3 / 4) < len / 200, "drag a marker on the screen");

        screen->mouseUp (ev (xFor (len * 7 / 8), false));      // plain click inside slice 2
        check (juce::roundToInt (r.p.getParameters().getRawParameterValue (tb::params::padId (0, tb::params::padSlice))->load()) == 2,
               "click a slice to select it (pad's SLICE becomes 2)");

        screen->mouseDoubleClick (ev (xFor (tbl.starts[1]), false, 2));
        check (sm.getInfo (0).slices.count == 1, "double-click a marker deletes it");
        ed.reset();
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
    void testEditorDrivesState()
    {
        std::cout << "\n== Plugin window -> saved state ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        juce::Slider* knob = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* sl = dynamic_cast<juce::Slider*> (&c))
                if (sl->getTooltip().startsWith ("Master level")) knob = sl;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (knob != nullptr, "found the MASTER LEVEL knob");
        if (knob == nullptr) return;

        struct Spy : juce::AudioProcessorListener
        {
            int changes = 0, gestures = 0, dirty = 0;
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override { if (d.nonParameterStateChanged) ++dirty; }
            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++gestures; }
        } spy;
        r.p.addListener (&spy);

        // A real mouse drag on the knob: press, drag up 60 px, release.
        auto centre = knob->getLocalBounds().getCentre().toFloat();
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto t0 = juce::Time::getCurrentTime();
        auto ev = [&] (juce::Point<float> pos, int ms, bool down)
        {
            return juce::MouseEvent (src, pos, down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                     1.0f, 0, 0, 0, 0, knob, knob, t0 + juce::RelativeTime::milliseconds (ms),
                                     centre, t0, 1, false);
        };
        const double before = knob->getValue();
        knob->mouseDown (ev (centre, 0, true));
        for (int i = 1; i <= 6; ++i) knob->mouseDrag (ev (centre.translated (0.0f, -10.0f * (float) i), i * 10, true));
        knob->mouseUp (ev (centre.translated (0.0f, -60.0f), 80, false));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        const float raw = r.p.getParameters().getRawParameterValue (tb::params::masterGain)->load();
        std::cout << "  knob " << before << " -> " << knob->getValue() << " dB, parameter now " << raw
                  << " dB, host saw " << spy.changes << " changes / " << spy.gestures << " gestures" << std::endl;
        check (std::abs (knob->getValue() - before) > 0.5, "dragging the knob moves it");
        check (std::abs (raw - (float) knob->getValue()) < 0.05f, "the parameter follows the knob");
        check (spy.changes > 0 && spy.gestures > 0, "the host is told (value change + gesture)");
        check (spy.dirty == 1, "after the drag the host is told to re-fetch the plugin state (Ableton AU save / duplicate / undo)");



        juce::MemoryBlock blob;
        r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        const float restored = q.getParameters().getRawParameterValue (tb::params::masterGain)->load();
        check (std::abs (restored - raw) < 0.05f, "knob-set value survives save / restore (" + juce::String (restored, 2) + " dB)");

        spy.dirty = 0;
        for (int d = 0; d < 5; ++d)    // five quick drags
        {
            knob->mouseDown (ev (centre, 0, true));
            knob->mouseDrag (ev (centre.translated (0.0f, 8.0f), 10, true));
            knob->mouseUp (ev (centre.translated (0.0f, 8.0f), 20, false));
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (spy.dirty == 1, "a burst of edits sends one coalesced notification (" + juce::String (spy.dirty) + ")");

        spy.dirty = 0;
        const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tb-dirty-test.wav");
        {
            juce::WavAudioFormat wav;
            auto* os = new juce::FileOutputStream (f);
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os, 48000.0, 1, 16, {}, 0));
            juce::AudioBuffer<float> b (1, 480); b.clear();
            w->writeFromAudioSampleBuffer (b, 0, 480);
        }
        r.p.getSamples().loadFile (4, f);
        waitForLoads (r.p);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (spy.dirty >= 1, "loading a sample also tells the host the state changed");
        f.deleteFile();

        // Host automation playback must NOT spam state notifications.
        spy.dirty = 0;
        auto* gainParam = r.p.getParameters().getParameter (tb::params::masterGain);
        for (int i = 0; i < 20; ++i) gainParam->setValueNotifyingHost ((float) i / 20.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (spy.dirty == 0, "host automation does not trigger state notifications");
        r.p.removeListener (&spy);
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
        setPadParam (p, 0, tb::params::padMode, (float) tb::params::modeLoop);
        setPadParam (p, 1, tb::params::padChoke, 1.0f);
        setPadParam (p, 2, tb::params::padChoke, 1.0f);
        setPadParam (p, 3, tb::params::padChoke, 1.0f);
        setPadParam (p, 4, tb::params::padMute, 1.0f);
        const bool playPage  = out.getFileName().contains ("play");
        const bool slicePage = out.getFileName().contains ("slice");
        const bool swapPage  = out.getFileName().contains ("swap");
        const bool mixerView = out.getFileName().contains ("mixer");
        if (mixerView)
        {
            // Kick + snare in A, hats in B, stab in C; a mix with a muted D.
            const int groupOf[] = { 0, 0, 0, 1, 1, 2, 2, 3 };
            for (int pad = 0; pad < 8; ++pad) setPadParam (p, pad, tb::params::padGroup, (float) groupOf[pad]);
            setParam (p, tb::params::groupId (1, tb::params::grpLevel), -6.0f);
            setParam (p, tb::params::groupId (1, tb::params::grpPan), 0.35f);
            setParam (p, tb::params::groupId (2, tb::params::grpLevel), -14.0f);
            setParam (p, tb::params::groupId (2, tb::params::grpPan), -0.5f);
            setParam (p, tb::params::groupId (3, tb::params::grpMute), 1.0f);
            setParam (p, tb::params::groupId (2, tb::params::grpBypass), 1.0f);
            setParam (p, tb::params::groupId (0, tb::params::grpSolo), 0.0f);
        }
        if (swapPage)
        {
            p.getSamples().autoSlice (0);
            waitForLoads (p);
            for (int pad = 0; pad < 4; ++pad)
            {
                setPadParam (p, pad, tb::params::padMode, (float) tb::params::modeLoop);
                setPadParam (p, pad, tb::params::padSwap, 1.0f);
                setPadParam (p, pad, tb::params::padChoke, 0.0f);
            }
        }
        if (slicePage)
        {
            p.getSamples().autoSlice (0);
            waitForLoads (p);
            setPadParam (p, 0, tb::params::padMode, (float) tb::params::modeSlice);
            p.spreadSlices (0);
            setPadParam (p, 0, tb::params::padSlice, 5.0f);
            setPadParam (p, 4, tb::params::padMute, 0.0f);
        }

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (tb::ui::MainView::kWidth, tb::ui::MainView::kHeight);
        if (mixerView)
        {
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* mv = dynamic_cast<tb::ui::MainView*> (&c)) mv->showMixer (true);
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*ed);
        }
        if (playPage || slicePage || swapPage)
        {
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* pe = dynamic_cast<tb::ui::PadEditorPanel*> (&c))
                    pe->setPage (slicePage ? tb::ui::PadEditorPanel::Page::slice
                                 : swapPage ? tb::ui::PadEditorPanel::Page::swap : tb::ui::PadEditorPanel::Page::play);
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*ed);
        }

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        const int pattern[] = { 1, 2, 3, 2, 1, 3, 2, 4 };
        for (int i = 0; i < 40; ++i)
        {
            if (i % 5 == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + pattern[(i / 5) % 8], (juce::uint8) 120), 0);
            if (i == 22) midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);   // loop, held
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
        testLoops (toneAudio, tone);
        testSync();
        testChokeMuteSolo (tone);
        testTransientDetection();
        testSlicing (toneAudio, tone);
        testSliceEditingOnScreen (tone);
        testSwapGroups();
        testGroups (tone);
        testMixerView();
        testPadLights (tone);
        testEditorDrivesState();
        testRealtimeSafety();

        std::cout << "\n" << (checks - failures) << " / " << checks << " checks passed" << std::endl;
        result = failures == 0 ? 0 : 1;
    }

    testDir().deleteRecursively();
    return result;
}
