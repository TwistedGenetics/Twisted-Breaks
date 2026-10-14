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
#include "Core/FxDefs.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>

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
            for (auto* n : { tb::params::padGroup, tb::params::padFixVel })
                allPadParams &= p.getParameters().getParameter (tb::params::padId (pad, n)) != nullptr;
        check (allPadParams, "288 per-pad parameters exist (pad01_gain .. pad16_fixvel)");
        check (tb::params::padId (6, tb::params::padPitch) == "pad07_pitch", "stable per-pad parameter IDs");
        {
            std::set<juce::String> ids;
            bool unique = true;
            for (auto* prm : p.getParameters().processor.getParameters())
                if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm)) unique &= ids.insert (wid->getParameterID()).second;
            check (unique, "every parameter ID is unique (" + juce::String ((int) ids.size()) + ")");
        }
        check (p.getParameters().processor.getParameters().size() == 1065, "1065 automatable parameters in total (3 master + 5 sequencer + 13 mutate + 11 macro + 6 resample + 3 scene + 4 snapshot / bank + 25 x 32 pads + 5 x 4 groups + 50 x 4 group FX)");

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
    //  PHASE 6: performance FX
    //==========================================================================
    juce::AudioBuffer<float> makeSine (double hz, int len, float amp = 0.5f)
    {
        juce::AudioBuffer<float> b (1, len);
        for (int i = 0; i < len; ++i)
            b.setSample (0, i, amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / 48000.0));
        return b;
    }

    float rmsOf (const juce::AudioBuffer<float>& b, int ch, int from, int num)
    {
        double s = 0.0;
        for (int i = from; i < from + num; ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return (float) std::sqrt (s / juce::jmax (1, num));
    }

    float cutoffNorm (float hz) { return std::log (hz / 20.0f) / std::log (1000.0f); }

    struct FxRig
    {
        Rig r;
        FxRig()
        {
            r.load (0, writeAudio ("fx_sine440",  makeSine (440.0,  4 * 48000), 48000.0, Fmt::wavFloat));
            r.load (1, writeAudio ("fx_sine100",  makeSine (100.0,  4 * 48000), 48000.0, Fmt::wavFloat));
            r.load (2, writeAudio ("fx_sine5k",   makeSine (5000.0, 4 * 48000), 48000.0, Fmt::wavFloat));
            juce::AudioBuffer<float> burst (1, 48000);
            burst.clear();
            for (int i = 0; i < 400; ++i) burst.setSample (0, i, 0.8f * (float) std::sin (i * 0.05) * (float) std::sin (juce::MathConstants<double>::pi * i / 400.0));
            r.load (3, writeAudio ("fx_burst", burst, 48000.0, Fmt::wavFloat));
            // A rising chirp never repeats, so loops / reversals can be checked exactly.
            juce::AudioBuffer<float> chirp (1, 4 * 48000);
            double ph = 0.0;
            for (int i = 0; i < chirp.getNumSamples(); ++i)
            {
                ph += juce::MathConstants<double>::twoPi * (200.0 + 0.02 * i) / 48000.0;
                chirp.setSample (0, i, 0.5f * (float) std::sin (ph));
            }
            r.load (4, writeAudio ("fx_chirp", chirp, 48000.0, Fmt::wavFloat));
            r.host.bpm = 120.0;              // 1 beat = 24000 samples
        }
        void fx (int fxId, bool on, int group = 0)            { setParam (r.p, tb::fx::paramId (group, fxId), on ? 1.0f : 0.0f); }
        void set (int fxId, int k, float v, int group = 0)    { setParam (r.p, tb::fx::paramId (group, fxId, k), v); }
        void allOff()
        {
            for (int g = 0; g < 4; ++g)
                for (int f = 0; f < tb::fx::kNumFx; ++f) fx (f, false, g);
            r.p.requestPanic();
            r.settle();
        }

        /** Plays `note` from the top of the timeline (Live playing from bar 1); hook runs before each block. */
        juce::AudioBuffer<float> play (int note, int blocks, std::function<void (int)> hook = {})
        {
            r.host.ppq = 0.0;
            r.host.playing = true;
            juce::AudioBuffer<float> out (2, blocks * kBlock), buf (2, kBlock);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 127), 0);
            for (int b = 0; b < blocks; ++b)
            {
                if (hook) hook (b);
                runBlock (r.p, r.host, buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            }
            r.host.playing = false;
            return out;
        }
    };

    void testFxFilters()
    {
        std::cout << "\n== Phase 6: FX - mute, filters, crush, rate, drive, tape ==" << std::endl;
        using namespace tb::fx;
        FxRig x;
        const auto dry440 = x.play (36, 40);
        x.allOff();

        // MUTE
        auto out = x.play (36, 40, [&] (int b) { if (b == 10) x.fx (mute, true); if (b == 25) x.fx (mute, false); });
        check (isSilent (out.getMagnitude (0, 12 * kBlock, 12 * kBlock)), "MUTE silences the group (10 ms fade)");
        check (maxStepIn (out, 0, 10 * kBlock - 4, 12 * kBlock) <= maxStepIn (dry440, 0, 1000, 10 * kBlock) * 1.05f, "...without a click");
        float diff = 0.0f;
        for (int i = 28 * kBlock; i < 40 * kBlock; ++i) diff = juce::jmax (diff, std::abs (out.getSample (0, i) - dry440.getSample (0, i)));
        check (diff < 1.0e-6f, "...and off again it's exactly the dry signal (" + juce::String (diff, 8) + ")");
        x.allOff();

        // Filters: measured attenuation.
        auto level = [&] (int note, int fxId, float hz, float res = 0.0f)
        {
            x.set (fxId, 0, cutoffNorm (hz)); x.set (fxId, 1, res); x.set (fxId, 2, 0.0f);
            x.fx (fxId, true);
            x.r.settle();
            const auto o = x.play (note, 30);
            x.allOff();
            const auto d = x.play (note, 30);
            x.allOff();
            return juce::Decibels::gainToDecibels (rmsOf (o, 0, 15 * kBlock, 14 * kBlock) / rmsOf (d, 0, 15 * kBlock, 14 * kBlock));
        };
        const float lp5k = level (38, lpf, 500.0f), lp100 = level (37, lpf, 500.0f);
        check (lp5k < -20.0f && lp100 > -1.5f, "LPF at 500 Hz: 5 kHz down " + juce::String (-lp5k, 1) + " dB, 100 Hz passes ("
                                                 + juce::String (lp100, 1) + " dB)");
        const float hp100 = level (37, hpf, 2000.0f), hp5k = level (38, hpf, 2000.0f);
        check (hp100 < -20.0f && hp5k > -1.5f, "HPF at 2 kHz: 100 Hz down " + juce::String (-hp100, 1) + " dB, 5 kHz passes ("
                                                 + juce::String (hp5k, 1) + " dB)");
        const float bpC = level (36, bpf, 440.0f, 0.5f), bpLo = level (37, bpf, 440.0f, 0.5f), bpHi = level (38, bpf, 440.0f, 0.5f);
        check (std::abs (bpC) < 1.5f && bpLo < -9.0f && bpHi < -12.0f, "BPF at 440 Hz: centre passes (" + juce::String (bpC, 1)
                 + " dB), 100 Hz " + juce::String (bpLo, 1) + " dB, 5 kHz " + juce::String (bpHi, 1) + " dB");

        // CRUSH to 2 bits: at most 4 output levels.
        x.set (crush, 0, 1.0f / 15.0f); x.set (crush, 1, 1.0f); x.fx (crush, true);
        x.r.settle();
        out = x.play (36, 20);
        std::set<int> levels;
        for (int i = 10 * kBlock; i < 20 * kBlock; ++i) levels.insert (juce::roundToInt (out.getSample (0, i) * 1.0e5f));
        check (levels.size() <= 4, "BIT CRUSH at 2 bits: the signal has " + juce::String ((int) levels.size()) + " levels (max 4)");
        x.allOff();

        // RATE: 1 kHz, stepped -> runs of exactly 48 identical samples.
        x.set (rate, 0, std::log (1000.0f / 200.0f) / std::log (220.5f)); x.set (rate, 1, 1.0f); x.set (rate, 2, 1.0f);
        x.fx (rate, true);
        x.r.settle();
        out = x.play (36, 20);
        int changes = 0;
        for (int i = 10 * kBlock + 1; i < 20 * kBlock; ++i) if (out.getSample (0, i) != out.getSample (0, i - 1)) ++changes;
        const float expected = (10.0f * kBlock) / 48.0f;
        check (std::abs ((float) changes - expected) <= 2.0f, "SAMPLE RATE 1 kHz (stepped): the output holds each value for 48 samples ("
                                                                 + juce::String (changes) + " changes, expected ~" + juce::String (expected, 0) + ")");
        x.allOff();

        // DRIVE hard +36 dB: a clipped square, never above full scale.
        x.set (drive, 0, 1.0f); x.set (drive, 1, 1.0f); x.set (drive, 2, 1.0f); x.fx (drive, true);
        x.r.settle();
        out = x.play (36, 20);
        int atRail = 0;
        for (int i = 10 * kBlock; i < 20 * kBlock; ++i) if (std::abs (out.getSample (0, i)) > 0.95f) ++atRail;
        check (out.getMagnitude (0, 10 * kBlock, 10 * kBlock) <= 1.0001f && atRail > 4 * kBlock,
               "DRIVE (hard, +36 dB) clips the sine flat (" + juce::String (100 * atRail / (10 * kBlock)) + " % of samples at the rail; master limiter keeps it under 0 dBFS)");
        x.allOff();

        // TAPE with the tone down: highs roll off.
        x.set (tape, 0, 0.5f); x.set (tape, 1, cutoffNorm (1000.0f)); x.fx (tape, true);
        x.r.settle();
        out = x.play (38, 20);
        x.allOff();
        const auto d5k = x.play (38, 20);
        x.allOff();
        const float tapeDb = juce::Decibels::gainToDecibels (rmsOf (out, 0, 10 * kBlock, 9 * kBlock) / rmsOf (d5k, 0, 10 * kBlock, 9 * kBlock));
        check (tapeDb < -10.0f, "TAPE (tone 1 kHz) rolls off a 5 kHz tone (" + juce::String (tapeDb, 1) + " dB)");
    }

    void testFxTime()
    {
        std::cout << "\n== Phase 6: FX - stutter, repeat, gate, pitch drop, reverse ==" << std::endl;
        using namespace tb::fx;
        FxRig x;
        const int beat = 24000, n16 = beat / 4, n8 = beat / 2;
        const auto dry = x.play (36, 120);
        x.allOff();

        // Engaged at sample 1024 (mid 1/16), on a chirp that never repeats.
        const auto dryC = x.play (40, 160);
        x.allOff();
        auto engageAt2 = [&] (int fxId) { return [&x, fxId] (int b) { if (b == 2) x.fx (fxId, true); }; };

        // STUTTER 1/16: waits for 6000, lets [6000, 12000) through, then loops it.
        x.set (stutter, 0, 3.0f);
        auto out = x.play (40, 60, engageAt2 (stutter));
        float pass = 0.0f, d1 = 0.0f;
        for (int k = 0; k < n16; ++k) pass = juce::jmax (pass, std::abs (out.getSample (0, k) - dryC.getSample (0, k)), std::abs (out.getSample (0, n16 + k) - dryC.getSample (0, n16 + k)));
        for (int k = 300; k < n16 - 300; ++k)
            d1 = juce::jmax (d1, std::abs (out.getSample (0, 2 * n16 + k) - dryC.getSample (0, n16 + k)),
                                 std::abs (out.getSample (0, 4 * n16 + k) - dryC.getSample (0, n16 + k)));
        check (pass < 1.0e-6f, "STUTTER waits for the next 1/16 and lets that 1/16 through");
        check (d1 < 1.0e-4f, "...then repeats exactly that 1/16, again and again (err " + juce::String (d1, 6) + ")");
        check (maxStepIn (out, 0, 2 * n16 - 300, 2 * n16 + 300) < maxStepIn (dryC, 0, 1000, n16 * 2) * 1.5f, "...seamlessly");
        x.allOff();

        // BEAT REPEAT 1/8: at 12000 it repeats [0, 12000).
        x.set (repeat, 0, 2.0f);
        out = x.play (40, 70, engageAt2 (repeat));
        d1 = 0.0f;
        float before = 0.0f;
        for (int k = 300; k < n8 - 300; ++k)
        {
            d1 = juce::jmax (d1, std::abs (out.getSample (0, n8 + k) - dryC.getSample (0, k)), std::abs (out.getSample (0, 2 * n8 + k) - dryC.getSample (0, k)));
            before = juce::jmax (before, std::abs (out.getSample (0, k) - dryC.getSample (0, k)));
        }
        check (before < 1.0e-6f && d1 < 1.0e-4f, "BEAT REPEAT 1/8: on the next 1/8 it repeats the 1/8 just played (err " + juce::String (d1, 6) + ")");
        check (maxStepIn (out, 0, n8 - 200, n8 + 400) < maxStepIn (dryC, 0, 1000, n8) * 1.5f, "...landing without a click");
        x.allOff();

        // GATE 1/16 at 50 %: second half of every 1/16 silent.
        x.set (gate, 0, 1.0f); x.set (gate, 1, 0.5f); x.fx (gate, true);
        x.r.settle();
        out = x.play (36, 60);
        bool chopped = true;
        for (int st = 2; st < 5; ++st)
        {
            chopped &= rmsOf (out, 0, st * n16 + n16 / 2 + 100, n16 / 2 - 200) < 1.0e-6f;
            chopped &= rmsOf (out, 0, st * n16 + 100, n16 / 2 - 200) > 0.3f;
        }
        check (chopped, "GATE 1/16 at 50 %: each 1/16 plays for its first half, silent for the second, on Live's grid");
        x.allOff();

        // PITCH DROP over 1/4: the tone slows (fewer zero crossings), then stops.
        x.set (pitch, 0, 2.0f);
        out = x.play (36, 80, [&] (int b) { if (b == 10) x.fx (pitch, true); });
        auto crossings = [&] (int from, int num)
        {
            int c = 0;
            for (int i = from + 1; i < from + num; ++i) if ((out.getSample (0, i - 1) < 0.0f) != (out.getSample (0, i) < 0.0f)) ++c;
            return c;
        };
        const int s0 = 10 * kBlock;
        const int c1 = crossings (s0 + 500, 4000), c2 = crossings (s0 + 10000, 4000), c3 = crossings (s0 + 18000, 4000);
        check (c1 > c2 && c2 > c3, "PITCH DROP: the pitch falls (zero crossings " + juce::String (c1) + " > " + juce::String (c2) + " > " + juce::String (c3) + ")");
        check (rmsOf (out, 0, s0 + beat + 500, 8000) < 1.0e-3f, "...and after 1/4 it has stopped dead");
        x.allOff();

        // REVERSE 1/4: from the next beat (24000), the beat before plays backwards.
        x.set (reverse, 0, 1.0f);
        out = x.play (40, 150, engageAt2 (reverse));
        d1 = 0.0f;
        for (int k = 500; k < beat - 500; ++k)
            d1 = juce::jmax (d1, std::abs (out.getSample (0, beat + k) - dryC.getSample (0, beat - 1 - k)),
                                 std::abs (out.getSample (0, 2 * beat + k) - dryC.getSample (0, 2 * beat - 1 - k)));
        check (d1 < 1.0e-4f, "REVERSE 1/4: on the next beat the last beat plays backwards, then the next... (err " + juce::String (d1, 6) + ")");
        check (maxStepIn (out, 0, 2 * beat - 300, 2 * beat + 300) < maxStepIn (dryC, 0, 2 * beat - 300, 2 * beat + 300) * 1.2f,
               "...chunk joins are click-free (2 ms raised-cosine edges)");
        x.allOff();
    }

    void testFxSpace()
    {
        std::cout << "\n== Phase 6: FX - delay, reverb, comb; groups, bypass, MIDI ==" << std::endl;
        using namespace tb::fx;
        FxRig x;

        // DELAY 1/8, no feedback, mix 100 %: the burst comes back exactly 1/8 later.
        x.set (delay, 0, 1.0f); x.set (delay, 1, 0.0f); x.set (delay, 2, 1.0f); x.fx (delay, true);
        x.r.settle();
        auto out = x.play (39, 60);
        const int n8 = 12000;
        float e = 0.0f;
        for (int k = 0; k < 400; ++k) e = juce::jmax (e, std::abs (out.getSample (0, n8 + k) - out.getSample (0, k)));
        check (e < 0.02f && out.getMagnitude (0, n8, 400) > 0.3f, "DELAY 1/8: the echo lands exactly 1/8 later (err " + juce::String (e, 4) + ")");
        x.allOff();

        // Throw: with feedback the tail keeps ringing after DELAY is switched off.
        x.set (delay, 1, 0.6f);
        out = x.play (39, 80, [&] (int b) { if (b == 0) x.fx (delay, true); if (b == 4) x.fx (delay, false); });
        check (out.getMagnitude (0, 3 * n8, 400) > 0.05f, "...it's a throw: switched off, the echoes keep repeating and fade");
        x.allOff();

        // REVERB tail.
        x.set (reverb, 2, 1.0f); x.fx (reverb, true);
        x.r.settle();
        out = x.play (39, 60, [&] (int b) { if (b == 6) x.fx (reverb, false); });
        check (rmsOf (out, 0, 8000, 4000) > 1.0e-3f && rmsOf (out, 0, 20000, 4000) > 1.0e-4f, "REVERB: the burst rings on as a tail (also after switching it off)");
        x.allOff();

        // COMB at C3: the burst rings at 130.8 Hz (period 367 samples).
        x.set (comb, 0, 0.5f); x.set (comb, 1, 0.9f); x.set (comb, 2, 0.0f); x.fx (comb, true);
        x.r.settle();
        out = x.play (39, 20);
        const int period = juce::roundToInt (48000.0 / 130.81);
        float best = 0.0f; int bestLag = 0;
        for (int lag = 200; lag < 600; ++lag)
        {
            double c = 0.0;
            for (int i = 2000; i < 6000; ++i) c += out.getSample (0, i) * out.getSample (0, i + lag);
            if (c > best) { best = (float) c; bestLag = lag; }
        }
        check (std::abs (bestLag - period) <= 2, "COMB at C3 resonates with a period of " + juce::String (bestLag) + " samples (130.8 Hz = " + juce::String (period) + ")");
        x.allOff();

        // Each group has its own chain: LPF on group A doesn't touch a pad in group B.
        setPadParam (x.r.p, 2, tb::params::padGroup, 1.0f);
        x.set (lpf, 0, cutoffNorm (300.0f)); x.fx (lpf, true, 0);
        x.r.settle();
        out = x.play (38, 20);
        x.allOff();
        const auto dB = x.play (38, 20);
        x.allOff();
        float diff = 0.0f;
        for (int i = 0; i < 20 * kBlock; ++i) diff = juce::jmax (diff, std::abs (out.getSample (0, i) - dB.getSample (0, i)));
        check (diff < 1.0e-6f, "groups are independent: LPF on group A leaves a group B pad untouched");
        setPadParam (x.r.p, 2, tb::params::padGroup, 0.0f);

        // FX BYPASS on the group: effects skipped.
        x.fx (lpf, true, 0);
        setParam (x.r.p, tb::params::groupId (0, tb::params::grpBypass), 1.0f);
        x.r.settle();
        out = x.play (38, 20);
        setParam (x.r.p, tb::params::groupId (0, tb::params::grpBypass), 0.0f);
        x.allOff();
        const auto d5k = x.play (38, 20);
        x.allOff();
        diff = 0.0f;
        for (int i = 0; i < 20 * kBlock; ++i) diff = juce::jmax (diff, std::abs (out.getSample (0, i) - d5k.getSample (0, i)));
        check (diff < 1.0e-6f, "group FX BYPASS: the LPF is skipped, dry signal out");

        // MIDI: note C#3 (61) = FX 2 (LPF) on the FX panel's group.
        auto& eng = x.r.p.getEngine();
        auto& br  = x.r.p.getBridge();
        eng.setFxTargetGroup (2);
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOff (1, 61), 200);
        runBlock (x.r.p, x.r.host, buf, midi);
        runBlock (x.r.p, x.r.host, buf, midi);
        check ((br.fxEngaged[2].load() >> lpf) & 1u, "MIDI note C#3 (LOCKED): LPF on group C stays on after the note ends");
        check ((br.fxEngaged[0].load() >> lpf) & 1u ? false : true, "...only on the selected group");
        midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 100), 0);
        runBlock (x.r.p, x.r.host, buf, midi);
        for (int b = 0; b < 4; ++b) runBlock (x.r.p, x.r.host, buf, midi);
        check (((br.fxEngaged[2].load() >> lpf) & 1u) == 0, "...a second press turns it off");

        eng.setFxLockedMask (~(1u << hpf));     // HPF momentary
        midi.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100), 0);
        runBlock (x.r.p, x.r.host, buf, midi);
        const bool held = (br.fxEngaged[2].load() >> hpf) & 1u;
        midi.addEvent (juce::MidiMessage::noteOff (1, 62), 0);
        for (int b = 0; b < 4; ++b) runBlock (x.r.p, x.r.host, buf, midi);
        check (held && ((br.fxEngaged[2].load() >> hpf) & 1u) == 0, "MOMENTARY from MIDI: HPF on while the note is held, off on release");

        midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 100), 0);
        runBlock (x.r.p, x.r.host, buf, midi);
        x.r.p.requestPanic();
        for (int b = 0; b < 4; ++b) runBlock (x.r.p, x.r.host, buf, midi);
        check (br.fxEngaged[2].load() == 0, "PANIC turns MIDI-latched effects off and kills tails");
        eng.setFxLockedMask (0xffffffffu);
    }

    void testFxUi()
    {
        std::cout << "\n== Phase 6: FX keys and FX editor ==" << std::endl;
        using namespace tb::fx;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);

        tb::ui::MainView* view = nullptr; tb::ui::FXGrid* grid = nullptr; tb::ui::FxEditorPanel* editor = nullptr;
        juce::Button* fxKey = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            if (auto* g = dynamic_cast<tb::ui::FXGrid*> (&c)) grid = g;
            if (auto* e = dynamic_cast<tb::ui::FxEditorPanel*> (&c)) editor = e;
            if (auto* sk = dynamic_cast<tb::ui::SelectorKey*> (&c)) if (sk->getName() == "FX") fxKey = sk;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (view && grid && editor && fxKey, "found the FX grid, FX editor and FX key");
        if (! (view && grid && editor && fxKey)) return;

        auto raw = [&] (const juce::String& id) { return r.p.getParameters().getRawParameterValue (id)->load(); };
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto mouse = [&] (juce::Component* comp, juce::Point<float> pos, bool down, bool right = false)
        {
            const auto t = juce::Time::getCurrentTime();
            auto mods = down ? (right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys();
            juce::MouseEvent e (src, pos, mods, 1.0f, 0, 0, 0, 0, comp, comp, t, pos, t, 1, false);
            if (down) comp->mouseDown (e); else comp->mouseUp (e);
        };

        check (fxKey->isEnabled(), "FX key is online");
        fxKey->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (editor->isVisible() && view->getView() == tb::ui::MainView::View::fx, "FX shows the FX editor in place of the CRT");

        // LPF (LOCKED by default): click on, click off.
        const auto lpfKey = grid->keyBounds (lpf).getCentre();
        mouse (grid, lpfKey, true); mouse (grid, lpfKey, false);
        check (raw (paramId (0, lpf)) > 0.5f, "clicking LPF (LOCKED) switches it on for group A and it stays on");
        check (editor->getFx() == lpf, "...and the FX editor now shows the LPF");
        mouse (grid, lpfKey, true); mouse (grid, lpfKey, false);
        check (raw (paramId (0, lpf)) < 0.5f, "...click again: off");

        // STUTTER (MOMENTARY by default): on only while held.
        const auto stKey = grid->keyBounds (stutter).getCentre();
        mouse (grid, stKey, true);
        const bool whileHeld = raw (paramId (0, stutter)) > 0.5f;
        mouse (grid, stKey, false);
        check (whileHeld && raw (paramId (0, stutter)) < 0.5f, "STUTTER (MOMENTARY): on while the mouse is held, off on release");

        // Right-click: edit without switching.
        mouse (grid, grid->keyBounds (delay).getCentre(), true, true); mouse (grid, grid->keyBounds (delay).getCentre(), false);
        check (editor->getFx() == delay && raw (paramId (0, delay)) < 0.5f, "right-click picks DELAY for editing without switching it on");

        // Group C.
        mouse (grid, grid->groupBounds (2).getCentre(), true); mouse (grid, grid->groupBounds (2).getCentre(), false);
        check (r.p.getEngine().getFxTargetGroup() == 2 && editor->getGroup() == 2, "GRP C: the keys, the editor and the FX MIDI notes now act on group C");
        mouse (grid, lpfKey, true); mouse (grid, lpfKey, false);
        check (raw (paramId (2, lpf)) > 0.5f && raw (paramId (0, lpf)) < 0.5f, "...LPF now switches group C's LPF, not A's");

        // Editor knob drives group C's cutoff.
        juce::Slider* knob = nullptr;
        for (auto* c : editor->getChildren())
            if (auto* sl = dynamic_cast<juce::Slider*> (c)) if (sl->isVisible() && sl->getTooltip().contains ("CUTOFF")) knob = sl;
        check (knob != nullptr, "the editor shows LPF's CUTOFF knob");
        if (knob != nullptr)
        {
            knob->setValue (0.2, juce::sendNotificationSync);
            check (std::abs (raw (paramId (2, lpf, 0)) - 0.2f) < 0.01f, "...turning it sets grp_c_lpf_cutoff (automatable)");
        }

        // LOCKED / MOMENTARY setting, saved with the set.
        juce::Button* momentary = nullptr; juce::Button* allOff = nullptr;
        for (auto* c : editor->getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (c))
            {
                if (b->getName() == "MOMENTARY") momentary = b;
                if (b->getName() == "ALL OFF") allOff = b;
            }
        check (momentary != nullptr && allOff != nullptr, "found MOMENTARY and ALL OFF");
        if (momentary == nullptr || allOff == nullptr) return;
        momentary->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (! r.p.isFxLocked (lpf), "MOMENTARY sets LPF to momentary");

        allOff->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (raw (paramId (2, lpf)) < 0.5f, "ALL OFF switches group C's effects off");

        juce::MemoryBlock blob;
        r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        check (! q.isFxLocked (lpf) && q.isFxLocked (hpf) && ! q.isFxLocked (stutter) && q.getEngine().getFxTargetGroup() == 2,
               "key modes and the FX group are saved with the set");

        // The PANIC button also switches effects off (so a LOCKED stutter doesn't come back).
        setParam (r.p, paramId (1, delay), 1.0f);
        setParam (r.p, paramId (3, repeat), 1.0f);
        juce::Button* panic = nullptr;
        std::function<void (juce::Component&)> findPanic = [&] (juce::Component& c)
        {
            if (auto* b = dynamic_cast<tb::ui::PanicButton*> (&c)) panic = b;
            for (auto* child : c.getChildren()) findPanic (*child);
        };
        findPanic (*ed);
        if (panic != nullptr) { panic->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (30); }
        check (panic != nullptr && raw (paramId (1, delay)) < 0.5f && raw (paramId (3, repeat)) < 0.5f, "the PANIC button switches every effect off too");
    }

    void testFxCpu()
    {
        std::cout << "\n== Phase 6: CPU with every effect on ==" << std::endl;
        FxRig x;
        for (int g = 0; g < 4; ++g)
            for (int f = 0; f < tb::fx::kNumFx; ++f)
                if (f != tb::fx::mute) x.fx (f, true, g);
        for (int p = 0; p < 4; ++p) setPadParam (x.r.p, p, tb::params::padGroup, (float) p);
        x.r.host.playing = true;
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        for (int p = 0; p < 4; ++p) midi.addEvent (juce::MidiMessage::noteOn (1, 36 + p, (juce::uint8) 120), 0);
        const int blocks = 400;   // ~4.3 s of audio
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b) runBlock (x.r.p, x.r.host, buf, midi);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const double audioMs = blocks * kBlock * 1000.0 / kSr;
        const double load = ms / audioMs * 100.0;
        std::cout << "  all 16 FX x 4 groups, 4 pads: " << juce::String (load, 1) << " % of one core" << std::endl;
        check (load < 60.0, "all 16 effects on all 4 groups run well inside real time (" + juce::String (load, 1) + " % of one core)");
        check (peakOf (buf) <= 1.0f && ! std::isnan (buf.getSample (0, 100)), "...and the output stays sane (no NaN, limiter holds)");
    }

    //==========================================================================
    //  PHASE 7: 32-step sequencer + live recording
    //==========================================================================
    /** Sample positions where sound starts after silence (block sample = 50 samples of 0.5). */
    std::vector<int> onsets (const juce::AudioBuffer<float>& b, int ch = 0)
    {
        std::vector<int> o;
        int quiet = 1000;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const bool loud = std::abs (b.getSample (ch, i)) > 1.0e-4f;
            if (loud && quiet >= 200) o.push_back (i);
            quiet = loud ? 0 : quiet + 1;
        }
        return o;
    }

    juce::String listOf (const std::vector<int>& v)
    {
        juce::StringArray a;
        for (int x : v) a.add (juce::String (x));
        return a.joinIntoString (" ");
    }

    struct SeqRig
    {
        Rig r;
        tb::SequencerData& seq;
        SeqRig() : seq (r.p.getBridge().seq)
        {
            juce::AudioBuffer<float> blip (1, 4000);
            blip.clear();
            for (int i = 0; i < 50; ++i) blip.setSample (0, i, 0.5f);
            r.load (0, writeAudio ("seq_blip", blip, 48000.0, Fmt::wavFloat));
            r.load (1, writeAudio ("seq_blip2", blip, 48000.0, Fmt::wavFloat));
            r.host.bpm = 120.0;                               // 1/16 = 6000 samples
            setPadParam (r.p, 1, tb::params::padPan, 1.0f);   // pad 2 on the right only
            setPadParam (r.p, 0, tb::params::padPan, -1.0f);  // pad 1 on the left only
            r.settle();
        }
        void step (int pad, int s, int vel = 127, int micro = 0)
        {
            tb::SeqStep st; st.on = true; st.velocity = vel; st.micro = micro;
            seq.set (pad, s, st);
        }
        juce::AudioBuffer<float> run (int samples, const std::vector<TimedEvent>& events = {}, bool playing = true)
        {
            r.host.ppq = 0.0;
            r.host.playing = playing;
            auto out = renderTimeline (r, samples, events);
            r.host.playing = false;
            r.p.requestPanic();
            r.settle();
            return out;
        }
    };

    void testSequencer()
    {
        std::cout << "\n== Phase 7: 32-step sequencer ==" << std::endl;
        {
            tb::SeqStep st; st.on = true; st.velocity = 77; st.micro = -64; st.probability = 55; st.ratchet = 3; st.pitch = -12;
            check (tb::SeqStep::unpack (st.pack()) == st, "a step packs into its 64-bit word and back (velocity, micro-timing, chance, ratchet, pitch)");
        }

        SeqRig x;
        x.step (0, 0); x.step (0, 4); x.step (0, 8); x.step (0, 12);
        auto out = x.run (100000);
        auto o = onsets (out);
        check (o == std::vector<int> { 0, 24000, 48000, 72000, 96000 }, "steps 1, 5, 9, 13 at 120 BPM fire at exactly 0 / 24000 / 48000 / 72000 samples, and loop after 16 steps ("
                                                                           + listOf (o) + ")");
        check (onsets (out, 1).empty(), "...only pad 1's lane plays");

        x.seq.clearAll();
        x.step (0, 1, 127); x.step (0, 2, 64);
        out = x.run (20000);
        o = onsets (out);
        const float ratio = out.getSample (0, 12010) / out.getSample (0, 6010);
        check (o == std::vector<int> { 6000, 12000 } && std::abs (ratio - 64.0f / 127.0f) < 1.0e-4f,
               "step 2 lands mid-block at sample 6000, and step velocity 64 plays at 64/127 (x" + juce::String (ratio, 4) + ")");

        x.seq.clearAll();
        x.step (0, 0);
        setParam (x.r.p, tb::params::seqLength, 3.0f);
        o = onsets (x.run (40000));
        check (o == std::vector<int> { 0, 18000, 36000 }, "LENGTH 3: the pattern repeats every 3 steps (" + listOf (o) + ")");
        setParam (x.r.p, tb::params::seqLength, 16.0f);

        x.seq.clearAll();
        x.step (0, 1);
        setParam (x.r.p, tb::params::seqRes, 0.0f);       // 1/8
        o = onsets (x.run (20000));
        check (o == std::vector<int> { 12000 }, "STEP 1/8: step 2 is an 1/8 in (sample 12000)");
        setParam (x.r.p, tb::params::seqRes, 1.0f);

        x.seq.clearAll();
        x.step (0, 2, 127, -64);
        o = onsets (x.run (20000));
        check (o == std::vector<int> { 9000 }, "micro-timing: a step pulled half a step early plays at 9000, not 12000 (" + listOf (o) + ")");

        x.seq.clearAll();
        x.step (0, 0); x.step (0, 4);
        check (onsets (x.run (30000, {}, false)).empty(), "Live stopped: the sequencer is silent");
        setParam (x.r.p, tb::params::seqRun, 0.0f);
        check (onsets (x.run (30000)).empty(), "RUN off: the pattern doesn't play");
        setParam (x.r.p, tb::params::seqRun, 1.0f);

        // Relocate / loop-brace jump back to bar 1 mid-pattern: step 1 plays again at the jump.
        {
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            juce::AudioBuffer<float> all (2, 80 * kBlock), buf (2, kBlock);
            juce::MidiBuffer midi;
            for (int b = 0; b < 80; ++b)
            {
                if (b == 30) x.r.host.ppq = 0.0;              // jump back at sample 15360
                runBlock (x.r.p, x.r.host, buf, midi);
                for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            }
            x.r.host.playing = false;
            o = onsets (all);
            const bool ok = o.size() >= 2 && o[0] == 0 && std::find (o.begin(), o.end(), 30 * kBlock) != o.end()
                         && std::find (o.begin(), o.end(), 30 * kBlock + 24000) != o.end();
            check (ok, "a jump back to bar 1 (loop brace / relocate) restarts the pattern on the spot (" + listOf (o) + ")");
            x.r.p.requestPanic(); x.r.settle();
        }

        // Transport restart: stop, back to 1, play: step 1 fires right at the start again.
        o = onsets (x.run (10000));
        check (! o.empty() && o[0] == 0, "stop and play again from bar 1: the first step fires at sample 0");
    }

    void testLiveRecord()
    {
        std::cout << "\n== Phase 7: live recording ==" << std::endl;
        SeqRig x;
        auto on = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };

        // Not armed: nothing recorded.
        x.run (20000, { { 7000, on (36) } });
        check (! x.seq.laneHasSteps (0), "an un-armed pad isn't recorded");

        x.seq.setArmed (0, true);
        x.run (20000, { { 7000, on (36) } }, false);
        check (! x.seq.laneHasSteps (0), "nothing is recorded while Live is stopped");

        // Hit just after step 2 (7000): quantised back onto step 2, plays from the next pass.
        auto out = x.run (110000, { { 7000, on (36, 90) } });
        const auto st = x.seq.get (0, 1);
        check (st.on && st.micro == 0 && st.velocity == 90, "armed: a hit at 7000 is recorded on step 2 (1/16), velocity 90");
        auto o = onsets (out);
        check (o == std::vector<int> { 7000, 96000 + 6000 }, "...you hear the hit, then the sequencer plays it in time next pass (" + listOf (o) + ")");

        // Hit early (10000): quantised forward to step 3 (12000): no double hit this pass.
        x.seq.clearLane (0);
        out = x.run (110000, { { 10000, on (36) } });
        o = onsets (out);
        check (x.seq.get (0, 2).on && o == std::vector<int> { 10000, 96000 + 12000 },
               "a hit early for step 3 records step 3 without a double hit straight after (" + listOf (o) + ")");

        // 1/32 quantise on the 1/16 grid: kept as micro-timing.
        x.seq.clearLane (0);
        setParam (x.r.p, tb::params::seqQuant, 3.0f);
        out = x.run (110000, { { 9100, on (36) } });
        const auto st32 = x.seq.get (0, 2);
        o = onsets (out);
        check (st32.on && st32.micro == -64 && std::find (o.begin(), o.end(), 96000 + 9000) != o.end(),
               "REC Q 1/32: a hit at 9100 plays back at 9000 (step 3 pulled half a step early) (" + listOf (o) + ")");
        setParam (x.r.p, tb::params::seqQuant, 2.0f);

        // Quantise 1/4: snapped to the beat.
        x.seq.clearLane (0);
        setParam (x.r.p, tb::params::seqQuant, 0.0f);
        x.run (30000, { { 20000, on (36) } });
        check (x.seq.get (0, 4).on && ! x.seq.get (0, 3).on, "REC Q 1/4: a hit at 20000 lands on the beat (step 5)");
        setParam (x.r.p, tb::params::seqQuant, 2.0f);

        // Mouse presses record too.
        x.seq.clearLane (0);
        x.r.host.ppq = 0.0; x.r.host.playing = true;
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            for (int b = 0; b < 35; ++b) runBlock (x.r.p, x.r.host, buf, midi);    // to ~17920
            x.r.p.triggerPadFromUi (0, 0.8f, true);
            runBlock (x.r.p, x.r.host, buf, midi);
        }
        x.r.host.playing = false;
        check (x.seq.get (0, 3).on, "mouse presses on an armed pad are recorded too (17920 -> step 4)");

        x.seq.setArmed (0, false);
        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        bool same = true;
        for (int s = 0; s < 32; ++s) same &= q.getBridge().seq.get (0, s) == x.seq.get (0, s);
        check (same && q.getBridge().seq.laneHasSteps (0), "the pattern is saved with the set");
    }

    void testSequencerView()
    {
        std::cout << "\n== Phase 7: SEQUENCER view ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::MainView* view = nullptr; tb::ui::SequencerPanel* panel = nullptr; juce::Button* seqKey = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            if (auto* sp = dynamic_cast<tb::ui::SequencerPanel*> (&c)) panel = sp;
            if (auto* sk = dynamic_cast<tb::ui::SelectorKey*> (&c)) if (sk->getName() == "SEQUENCER") seqKey = sk;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (view && panel && seqKey, "found the SEQUENCER key and view");
        if (! (view && panel && seqKey)) return;
        check (seqKey->isEnabled(), "SEQUENCER key is online");
        seqKey->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (panel->isVisible() && view->getView() == tb::ui::MainView::View::sequencer, "it shows the 16 x 32 grid in place of the CRT");

        auto& seq = r.p.getBridge().seq;
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&] (juce::Point<float> pos, juce::Point<float> down, bool pressed)
        {
            const auto t = juce::Time::getCurrentTime();
            return juce::MouseEvent (src, pos, pressed ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                     1.0f, 0, 0, 0, 0, panel, panel, t, down, t, 1, false);
        };
        const auto c = panel->cellBounds (2, 5).getCentre();
        panel->mouseDown (ev (c, c, true)); panel->mouseUp (ev (c, c, false));
        check (seq.get (2, 5).on, "clicking a cell switches the step on");
        panel->mouseDown (ev (c, c, true));
        panel->mouseDrag (ev (c.translated (0.0f, -20.0f), c, true));
        panel->mouseUp (ev (c.translated (0.0f, -20.0f), c, false));
        check (seq.get (2, 5).on && seq.get (2, 5).velocity == 120, "dragging up on a step raises its velocity (100 -> " + juce::String (seq.get (2, 5).velocity) + ")");
        panel->mouseDown (ev (c, c, true)); panel->mouseUp (ev (c, c, false));
        check (! seq.get (2, 5).on, "clicking a lit step switches it off");

        const auto lbl = panel->laneLabelBounds (6).getCentre();
        panel->mouseDown (ev (lbl, lbl, true)); panel->mouseUp (ev (lbl, lbl, false));
        juce::Button* arm = nullptr; juce::Button* clear = nullptr;
        for (auto* ch : panel->getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (ch))
            {
                if (b->getName().startsWith ("ARM")) arm = b;
                if (b->getName() == "CLEAR") clear = b;
            }
        check (arm != nullptr && arm->getButtonText() == "ARM PAD 07", "clicking lane 07 selects pad 7 (ARM follows)");
        if (arm == nullptr || clear == nullptr) return;
        arm->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (seq.isArmed (6), "ARM arms pad 7 for recording");
        seq.set (6, 0, tb::SeqStep { true });
        clear->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (! seq.laneHasSteps (6), "CLEAR empties pad 7's lane");
    }


    //==========================================================================
    //  PHASE 9: per-step features of the main sequencer
    //==========================================================================
    /** Loud run lengths: (onset, samples until it goes quiet) on a channel. */
    std::vector<std::pair<int, int>> bursts (const juce::AudioBuffer<float>& b, int ch = 0, float thresh = 1.0e-4f)
    {
        std::vector<std::pair<int, int>> o;
        int start = -1, quiet = 1000;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const bool loud = std::abs (b.getSample (ch, i)) > thresh;
            if (loud && quiet >= 200) { if (start >= 0) o.back().second = i - quiet - start; start = i; o.push_back ({ i, 0 }); }
            if (loud) o.back().second = i - start + 1;
            quiet = loud ? 0 : quiet + 1;
        }
        return o;
    }

    void testStepFeatures()
    {
        std::cout << "\n== Phase 9: step features (chance, ratchet, pitch, reverse, slice, FX, conditions) ==" << std::endl;
        {
            tb::SeqStep st; st.on = true; st.velocity = 90; st.micro = 17; st.probability = 35; st.ratchet = 4; st.pitch = -24;
            st.rev = true; st.slice = 16; st.fx = 16; st.cond = tb::condNotFirst; st.hit = false;
            check (tb::SeqStep::unpack (st.pack()) == st && tb::SeqStep::unpack (st.pack()).fx == 16 && ! tb::SeqStep::unpack (st.pack()).hit,
                   "every step field packs into the 64-bit word and back");

            // A 0.7 / 0.8 set: 32-bit words (on, velocity 100, chance 100).
            const uint64_t old = 1u | (100u << 1) | (100u << 16) | (32u << 25);
            const auto o = tb::SeqStep::unpack (old);
            check (o.on && o.velocity == 100 && o.probability == 100 && o.ratchet == 1 && o.pitch == 0 && ! o.rev && o.slice == 0
                     && o.fx == 0 && o.cond == tb::condAlways && o.hit,
                   "a step saved by 0.7 / 0.8 loads as the same plain step (new fields off)");
            juce::ValueTree t ("SEQ"), lane ("LANE");
            lane.setProperty ("pad", 3, nullptr);
            lane.setProperty ("steps", juce::String::toHexString ((juce::int64) old) + ",0", nullptr);
            t.appendChild (lane, nullptr);
            tb::SequencerData d;
            d.fromValueTree (t);
            check (d.get (3, 0) == o && ! d.get (3, 1).on, "...and an old saved SEQ tree reads straight in");
            tb::SequencerData e;
            d.set (3, 5, st);
            e.fromValueTree (d.toValueTree());
            check (e.get (3, 5) == st, "the new fields are saved with the set and read back");
        }

        SeqRig x;
        auto stepWith = [&x] (int pad, int s, std::function<void (tb::SeqStep&)> f)
        {
            tb::SeqStep st; st.on = true; st.velocity = 127; f (st); x.seq.set (pad, s, st);
        };

        // ---- Ratchets: evenly across the step, sample-exact (1/16 = 6000).
        x.seq.clearAll();
        stepWith (0, 0, [] (auto& s) { s.ratchet = 3; });
        stepWith (0, 1, [] (auto& s) { s.ratchet = 4; });
        auto o = onsets (x.run (14000));
        check (o == std::vector<int> { 0, 2000, 4000, 6000, 7500, 9000, 10500 }, "RATCHET x3 / x4: hits every 1/3 and 1/4 of the step (" + listOf (o) + ")");

        // ---- Chance.
        x.seq.clearAll();
        stepWith (0, 0, [] (auto& s) { s.probability = 0; s.ratchet = 2; });
        check (onsets (x.run (12000)).empty(), "CHANCE 0 %: the step (and its ratchets) never play");
        x.seq.clearAll();
        setParam (x.r.p, tb::params::seqLength, 1.0f);           // one step = one pass: 64 rolls in 384000 samples
        stepWith (0, 0, [] (auto& s) { s.probability = 50; s.ratchet = 2; });
        o = onsets (x.run (64 * 6000));
        int plays = 0; bool whole = true;
        for (int pass = 0; pass < 64; ++pass)
        {
            int n = 0;
            for (int t : o) n += (t >= pass * 6000 && t < (pass + 1) * 6000) ? 1 : 0;
            plays += n > 0 ? 1 : 0;
            whole = whole && (n == 0 || n == 2);
        }
        check (plays >= 20 && plays <= 44 && whole,
               "CHANCE 50 %: " + juce::String (plays) + " of 64 passes play, and a step's ratchets play all-or-nothing");
        setParam (x.r.p, tb::params::seqLength, 4.0f);           // 1 pass = 24000 samples

        // ---- Conditions (pass counted on Live's timeline).
        auto condOnsets = [&] (int cond, double startPpq = 0.0, int samples = 95000)   // 4 passes (blocks round up)
        {
            x.seq.clearAll();
            stepWith (0, 0, [cond] (auto& s) { s.cond = cond; });
            x.r.host.ppq = startPpq;
            x.r.host.playing = true;
            auto out = renderTimeline (x.r, samples, {});
            x.r.host.playing = false;
            x.r.p.requestPanic();
            x.r.settle();
            return onsets (out);
        };
        { const auto co = condOnsets (tb::cond1of2); check (co == std::vector<int> { 0, 48000 }, "1:2 plays on passes 1 and 3 (" + listOf (co) + ")"); }
        { const auto co = condOnsets (tb::cond2of2); check (co == std::vector<int> { 24000, 72000 }, "2:2 plays on passes 2 and 4 (" + listOf (co) + ")"); }
        { const auto co = condOnsets (tb::cond3of4); check (co == std::vector<int> { 48000 }, "3:4 plays on pass 3 of 4 only (" + listOf (co) + ")"); }
        { const auto co = condOnsets (tb::cond1of3); check (co == std::vector<int> { 0, 72000 }, "1:3 plays on passes 1 and 4 (" + listOf (co) + ")"); }
        { const auto co = condOnsets (tb::condFirst); check (co == std::vector<int> { 0 }, "FIRST plays only on the pass Live started in (" + listOf (co) + ")"); }
        { const auto co = condOnsets (tb::condNotFirst); check (co == std::vector<int> { 24000, 48000, 72000 }, "!FIRST skips that pass (" + listOf (co) + ")"); }
        o = condOnsets (tb::condFirst, 8.0, 71000);                                       // start at pass 9 (1 pass = 1 beat)
        check (o == std::vector<int> { 0 }, "FIRST follows where Live starts (from beat 9: plays there, then not) (" + listOf (o) + ")");
        o = condOnsets (tb::cond1of2, 1.0, 71000);                                        // start at pass 2
        check (o == std::vector<int> { 24000 }, "A:B is on the song position, so a restart mid-song stays in step (" + listOf (o) + ")");
        check (condOnsets (tb::condFill).empty(), "FILL steps rest while FILL isn't held");
        setParam (x.r.p, tb::params::seqFill, 1.0f);
        check (condOnsets (tb::condFill).size() == 4 && condOnsets (tb::condNotFill).empty(), "holding FILL (the Sequencer Fill parameter) plays FILL steps and rests !FILL ones");
        setParam (x.r.p, tb::params::seqFill, 0.0f);
        check (condOnsets (tb::condNotFill).size() == 4, "...and !FILL steps play again when it's let go");
        setParam (x.r.p, tb::params::seqLength, 16.0f);

        // ---- Pitch / reverse / slice: a 4000-sample ramp (0 -> 0.8).
        {
            Rig r;
            juce::AudioBuffer<float> ramp (1, 4000);
            for (int i = 0; i < 4000; ++i) ramp.setSample (0, i, 0.05f + 0.75f * (float) i / 3999.0f);
            r.load (0, writeAudio ("step_ramp", ramp, 48000.0, Fmt::wavFloat));
            r.host.bpm = 120.0;
            setParam (r.p, tb::params::masterLimiter, 0.0f);
            r.settle();
            auto& seq = r.p.getBridge().seq;
            auto runIt = [&r] (int n)
            {
                r.host.ppq = 0.0; r.host.playing = true;
                auto out = renderTimeline (r, n, {});
                r.host.playing = false; r.p.requestPanic(); r.settle();
                return out;
            };
            auto set = [&seq] (int s, std::function<void (tb::SeqStep&)> f) { tb::SeqStep st; st.on = true; st.velocity = 127; f (st); seq.set (0, s, st); };
            set (0, [] (auto& s) { s.pitch = 12; });
            set (1, [] (auto&) {});
            set (2, [] (auto& s) { s.pitch = -12; });
            auto out = runIt (30000);
            auto b = bursts (out);
            const bool pitchOk = b.size() == 3 && std::abs (b[0].second - 2000) <= 2 && std::abs (b[1].second - 4000) <= 2
                                 && std::abs (b[2].second - 8000) <= 2 && b[0].first == 0 && b[1].first == 6000 && b[2].first == 12000;
            juce::String desc;
            for (auto& [s0, n] : b) desc << s0 << ":" << n << " ";
            check (pitchOk, "PITCH +12 plays the hit at double speed (2000 samples), the next plain step at the pad's own (4000), -12 at half (8000) ("
                              + desc.trim() + ")");
            check (std::abs (out.getSample (0, 1000) - (0.05f + 0.75f * 2000.0f / 3999.0f)) < 0.01f,
                   "...and the pitched hit holds its speed across blocks (halfway through the ramp at 1000 samples)");

            seq.clearAll();
            set (0, [] (auto& s) { s.rev = true; });
            set (1, [] (auto&) {});
            out = runIt (12000);
            check (out.getSample (0, 60) > 0.75f && out.getSample (0, 3800) < 0.1f && out.getSample (0, 6060) < 0.07f,
                   "REV plays that hit backwards (top of the ramp first), the next step forwards again ("
                     + juce::String (out.getSample (0, 60), 3) + " " + juce::String (out.getSample (0, 3800), 3) + " " + juce::String (out.getSample (0, 6060), 3) + ")");

            // SLICE: no markers = 16 equal slices of 250 samples.
            seq.clearAll();
            set (0, [] (auto& s) { s.slice = 9; });
            out = runIt (6000);
            b = bursts (out);
            check (b.size() == 1 && std::abs (b[0].second - 250) <= 2 && std::abs (out.getSample (0, 60) - (0.05f + 0.75f * 2060.0f / 3999.0f)) < 0.01f,
                   "SLICE 9 plays just that slice of the pad (samples 2000-2250) (" + juce::String ((int) b.size()) + " " + (b.empty() ? juce::String() : juce::String (b[0].second)) + " " + juce::String (out.getSample (0, 60), 3) + ")");

            seq.clearAll();
            set (0, [] (auto& s) { s.slice = 3; s.rev = true; s.pitch = 12; });
            out = runIt (6000);
            b = bursts (out);
            check (b.size() == 1 && std::abs (b[0].second - 125) <= 2 && out.getSample (0, 50) > out.getSample (0, 110) + 0.05f,
                   "...combined: slice 3, reversed, an octave up (125 samples, falling)");
        }

        // ---- FX steps: a looping tone on group A; MUTE thrown on steps 3-4, FX only.
        {
            SeqRig y;
            juce::AudioBuffer<float> tone (1, 4800);
            for (int i = 0; i < 4800; ++i) tone.setSample (0, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * (float) i / 48.0f));
            y.r.load (2, writeAudio ("step_tone", tone, 48000.0, Fmt::wavFloat));
            setPadParam (y.r.p, 2, tb::params::padMode, (float) tb::params::modeLoop);
            setPadParam (y.r.p, 2, tb::params::padSync, 0.0f);
            y.r.settle();
            auto& seq = y.seq;
            tb::SeqStep hit; hit.on = true; hit.velocity = 127;
            seq.clearAll();
            seq.set (2, 0, hit);
            tb::SeqStep m; m.on = true; m.fx = tb::fx::mute + 1; m.hit = false;
            seq.set (2, 2, m); seq.set (2, 3, m);
            y.r.host.ppq = 0.0; y.r.host.playing = true;
            auto out = renderTimeline (y.r, 30000, {});
            auto peakIn = [&out] (int a, int b) { float p = 0; for (int i = a; i < b; ++i) p = juce::jmax (p, std::abs (out.getSample (0, i))); return p; };
            // MUTE's own 10 ms crossfade follows each edge.
            check (peakIn (8000, 11990) > 0.2f && peakIn (12600, 24000) < 1.0e-4f && peakIn (24600, 30000) > 0.2f && peakIn (24000, 24100) > 0.0f,
                   "FX steps hold MUTE on the pad's group from step 3 to the end of step 4 (12000-24000): back-to-back steps without a gap, then it lets go ("
                     + juce::String (peakIn (8000, 11990), 3) + " " + juce::String (peakIn (12600, 24000), 5) + " " + juce::String (peakIn (24600, 30000), 3) + ")");
            // The loop was never retriggered (HIT off): its phase carries straight on.
            const float expect = 0.3f * std::sin (juce::MathConstants<float>::twoPi * (float) (26000 % 48) / 48.0f);
            check (std::abs (out.getSample (0, 26000) - expect) < 0.02f, "...FX-only steps don't retrigger the pad (the loop runs on underneath)");

            seq.clearAll();
            seq.set (2, 0, hit);
            seq.set (2, 1, m);
            y.r.host.ppq = 0.0;
            renderTimeline (y.r, 8000, {});                               // MUTE held (step 2) ...
            const bool heldNow = (y.r.p.getBridge().fxEngaged[0].load() & 1u) != 0;
            y.r.host.playing = false;                                     // ... and Live stops mid-step
            renderTimeline (y.r, 4096, {});
            check (heldNow && (y.r.p.getBridge().fxEngaged[0].load() & 1u) == 0, "stopping Live lets go of a step-held effect");
            y.r.p.requestPanic(); y.r.settle();

            // Chance applies to the FX too.
            seq.clearAll();
            seq.set (2, 0, hit);
            m.probability = 0;
            seq.set (2, 2, m);
            y.r.host.ppq = 0.0; y.r.host.playing = true;
            out = renderTimeline (y.r, 20000, {});
            y.r.host.playing = false; y.r.p.requestPanic(); y.r.settle();
            check (peakIn (12600, 17900) > 0.2f, "...and a step that doesn't play (CHANCE 0) doesn't throw its FX");
        }

        // ---- Live recording keeps a step's settings.
        {
            SeqRig z;
            z.seq.clearAll();
            tb::SeqStep st; st.on = true; st.ratchet = 3; st.fx = 5; st.cond = tb::cond1of2; st.velocity = 20;
            z.seq.set (0, 2, st);
            z.seq.setArmed (0, true);
            z.run (16000, { { 12000, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100) } });
            const auto got = z.seq.get (0, 2);
            check (got.on && got.velocity == 100 && got.ratchet == 3 && got.fx == 5 && got.cond == tb::cond1of2,
                   "recording a hit onto a step keeps its ratchet / FX / condition (new velocity " + juce::String (got.velocity) + ")");
            z.seq.setArmed (0, false);
        }
    }

    void testStepEditorView()
    {
        std::cout << "\n== Phase 9: SEQUENCER step row ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::MainView* view = nullptr; tb::ui::SequencerPanel* panel = nullptr; tb::ui::TransportDisplay* trans = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            if (auto* sp = dynamic_cast<tb::ui::SequencerPanel*> (&c)) panel = sp;
            if (auto* td = dynamic_cast<tb::ui::TransportDisplay*> (&c)) trans = td;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (view && panel && trans, "found the sequencer panel");
        if (! (view && panel && trans)) return;
        view->showView (tb::ui::MainView::View::sequencer);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (panel->getHeight() > 380 && ! trans->isVisible(), "the SEQUENCER view takes the transport box too (" + juce::String (panel->getHeight()) + " px), for the STEP row");
        view->showView (tb::ui::MainView::View::crt);
        check (trans->isVisible(), "...and the transport box is back in the other views");
        view->showView (tb::ui::MainView::View::sequencer);

        auto& seq = r.p.getBridge().seq;
        seq.clearAll();
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&] (juce::Component* comp, juce::Point<float> pos, juce::ModifierKeys mods)
        {
            const auto t = juce::Time::getCurrentTime();
            return juce::MouseEvent (src, pos, mods, 1.0f, 0, 0, 0, 0, comp, comp, t, pos, t, 1, false);
        };
        const auto c = panel->cellBounds (4, 9).getCentre();
        panel->mouseDown (ev (panel, c, juce::ModifierKeys::leftButtonModifier)); panel->mouseUp (ev (panel, c, {}));
        check (seq.get (4, 9).on && panel->getSelectedPad() == 4 && panel->getSelectedStep() == 9,
               "clicking a cell switches it on and selects it for the STEP row (pad 5, step 10)");
        panel->mouseDown (ev (panel, c, juce::ModifierKeys::rightButtonModifier)); panel->mouseUp (ev (panel, c, {}));
        check (seq.get (4, 9).on, "right-click selects a lit step without switching it off");

        // Drive the readouts like a user: scroll / click.
        auto wheel = [&] (juce::Component& comp, int notches)
        {
            juce::MouseWheelDetails w { 0.0f, notches > 0 ? 0.1f : -0.1f, false, false, false };
            for (int i = 0; i < std::abs (notches); ++i)
                comp.mouseWheelMove (ev (&comp, comp.getLocalBounds().getCentre().toFloat(), {}), w);
        };
        auto click = [&] (juce::Component& comp)
        {
            const auto p = comp.getLocalBounds().getCentre().toFloat();
            comp.mouseDown (ev (&comp, p, juce::ModifierKeys::leftButtonModifier));
            comp.mouseUp (ev (&comp, p, {}));
        };
        click (panel->readout ("RAT")); click (panel->readout ("RAT"));
        wheel (panel->readout ("CHANCE"), -5);
        wheel (panel->readout ("PITCH"), 7);
        wheel (panel->readout ("SLICE"), 3);
        wheel (panel->readout ("FX"), tb::fx::stutter + 1);
        wheel (panel->readout ("COND"), tb::cond3of4);
        wheel (panel->readout ("MICRO"), -10);
        panel->revButton().triggerClick();
        panel->hitButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        const auto st = seq.get (4, 9);
        check (st.ratchet == 3 && st.probability == 75 && st.pitch == 7 && st.slice == 3 && st.fx == tb::fx::stutter + 1
                 && st.cond == tb::cond3of4 && st.micro == -10 && st.rev && ! st.hit,
               "the STEP row edits RATCHET, CHANCE, PITCH, SLICE, FX, COND, MICRO, REV and HIT of the selected step");

        // Editing an empty step switches it on.
        const auto e = panel->cellBounds (7, 3).getCentre();
        panel->mouseDown (ev (panel, e, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier)));
        panel->mouseUp (ev (panel, e, {}));
        check (! seq.get (7, 3).on && panel->getSelectedStep() == 3, "shift-click selects an empty step without switching it on");
        wheel (panel->readout ("PITCH"), -5);
        check (seq.get (7, 3).on && seq.get (7, 3).pitch == -5, "editing an empty step's setting switches it on");

        // FILL (HOLD) drives the automatable parameter.
        auto* fillParam = r.p.getParameters().getRawParameterValue (tb::params::seqFill);
        juce::Component& fill = panel->fillButton();
        const auto fp = fill.getLocalBounds().getCentre().toFloat();
        fill.mouseDown (ev (&fill, fp, juce::ModifierKeys::leftButtonModifier));
        const bool down = fillParam->load() > 0.5f;
        fill.mouseUp (ev (&fill, fp, {}));
        check (down && fillParam->load() < 0.5f, "FILL (HOLD) sets Sequencer Fill while it's held, and lets go");
    }


    //==========================================================================
    //  PHASE 7.1: per-pad slice sequencer
    //==========================================================================
    struct SliceRig
    {
        Rig r;
        juce::AudioBuffer<float> brk;
        tb::SliceSeqData& data;
        int step = 0;      // 1/16 in samples
        SliceRig() : brk (makeBreak (48000.0)), data (r.p.getBridge().sliceSeq)
        {
            r.load (0, writeAudio ("sseq_break", brk, 48000.0, Fmt::wavFloat));
            step = breakStepLength (48000.0);
            r.host.bpm = 60.0 * 8.0 * 48000.0 / (double) brk.getNumSamples();   // the break's own tempo: 1/16 = `step` samples
            setPadParam (r.p, 0, tb::params::padSliceSeq, 1.0f);
            setParam (r.p, tb::params::masterLimiter, 0.0f);   // the synthetic break peaks above the limiter's ceiling
            r.settle();
        }
        void equal16() { r.p.getSamples().setSliceCount (0, 16); r.p.getSamples().equalSlice (0); juce::MessageManager::getInstance()->runDispatchLoopUntil (20); r.settle(); }
        /** Write a slot from a list of steps (rest of the 32 = TIE). */
        void write (int slot, std::vector<tb::SliceStep> steps, int length, int rate = 1, int swing = 0)
        {
            data.resetSlot (0, slot);
            for (int k = 0; k < (int) steps.size(); ++k) data.setStep (0, slot, k, steps[(size_t) k]);
            tb::SliceSlot ss; ss.length = length; ss.rate = rate; ss.swing = swing; ss.written = true;
            data.setSlot (0, slot, ss);
        }
        /** Live playing from bar 1; the pad is hit at `hitAt`. */
        juce::AudioBuffer<float> run (int samples, std::vector<TimedEvent> extra = {}, int hitAt = 0, bool playing = true)
        {
            r.host.ppq = 0.0;
            r.host.playing = playing;
            extra.insert (extra.begin(), TimedEvent { hitAt, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127) });
            auto out = renderTimeline (r, samples, extra);
            r.host.playing = false;
            r.p.requestPanic();
            r.settle();
            return out;
        }
        /** Max difference, skipping the first 40 samples (a hit's 0.5 ms fade-in). */
        float diffFromBreak (const juce::AudioBuffer<float>& out, int outStart, int brkStart, int num, int ch = 0) const
        {
            float d = 0.0f;
            for (int i = 40; i < num; ++i)
                d = juce::jmax (d, std::abs (out.getSample (ch, outStart + i) - brk.getSample (ch, brkStart + i)));
            return d;
        }
    };

    void testSliceSequencer()
    {
        std::cout << "\n== Phase 7.1: slice sequencer ==" << std::endl;
        using SS = tb::SliceStep;
        {
            SS st; st.kind = SS::play; st.slice = 13; st.rev = true; st.pitch = -7; st.velocity = 90; st.length = 1; st.ratchet = 3; st.chance = 42; st.micro = -32;
            check (SS::unpack (st.pack()) == st, "a slice step packs into 64 bits and back (slice, rev, pitch, vel, length, ratchet, chance, micro)");
        }

        SliceRig x;
        const int L = x.brk.getNumSamples();
        auto play = [] (int slice) { return SS::playing (slice); };
        auto rest = [] { SS s; s.kind = SS::rest; return s; };

        // Untouched pattern on an AUTO-sliced break: replays the break exactly.
        x.r.p.getSamples().autoSlice (0);
        waitForLoads (x.r.p);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        x.r.settle();
        auto out = x.run (L + 4000);
        check (x.diffFromBreak (out, 0, 0, L - 64) < 1.0e-5f, "untouched pattern (AUTO slices): the break plays back exactly as recorded (err "
                                                               + juce::String (x.diffFromBreak (out, 0, 0, L - 64), 7) + ")");
        check (x.diffFromBreak (out, L, 0, 3000) < 1.0e-3f, "...and loops round in time");

        x.equal16();
        const int s2 = 2 * x.step;          // EQUAL 16 on 32 steps: a slice every 2 steps

        // Rearranged: slice 5 first, then a rest, then slice 1 reversed.
        SS rev1 = play (0); rev1.rev = true;
        x.write (0, { play (4), {}, rest(), {}, rev1 }, 8);
        out = x.run (4 * x.step + s2 + 500);
        check (x.diffFromBreak (out, 0, 4 * s2, s2 - 300) < 1.0e-5f, "step 1 plays slice 5 (rearranged), from its first sample");
        check (isSilent (out.getMagnitude (0, s2 + 200, s2 - 400)), "REST cuts it on step 3 (3 ms fade)");
        float d = 0.0f;
        for (int i = 300; i < s2 - 300; ++i) d = juce::jmax (d, std::abs (out.getSample (0, 4 * x.step + i) - x.brk.getSample (0, s2 - 1 - i)));
        check (d < 1.0e-4f, "REV: step 5 plays slice 1 backwards from its end (err " + juce::String (d, 6) + ")");

        // Ratchet x2: the slice restarts half way through the step.
        SS r2 = play (2); r2.ratchet = 2;
        x.write (0, { r2, rest() }, 2);
        out = x.run (x.step + 100);
        const int half = x.step / 2;
        check (x.diffFromBreak (out, 0, 2 * s2, half - 300) < 1.0e-5f && x.diffFromBreak (out, half + 300, 2 * s2 + 300, half - 600) < 1.0e-3f,
               "RATCHET x2: the slice plays twice inside the step");

        // Gate 1/2: silent for the second half of the step.
        SS g = play (3); g.length = 1;
        x.write (0, { g, {} }, 2);
        out = x.run (2 * x.step);
        check (x.diffFromBreak (out, 0, 3 * s2, half - 300) < 1.0e-5f && isSilent (out.getMagnitude (0, half + 200, x.step - 400)),
               "LENGTH 1/2: the hit is gated to half a step");

        // Pitch +12: twice the speed.
        SS p12 = play (1); p12.pitch = 12;
        x.write (0, { p12 }, 1);
        out = x.run (x.step);
        d = 0.0f;
        for (int i = 100; i < 1500; ++i) d = juce::jmax (d, std::abs (out.getSample (0, i) - x.brk.getSample (0, s2 + 2 * i)));
        check (d < 2.0e-3f, "PITCH +12: the slice plays an octave up (twice as fast)");

        // Chance 0 never plays; micro -32 = half a step early; swing delays odd steps.
        SS never = play (1); never.chance = 0;
        x.write (0, { rest(), {}, never, {} }, 4);
        check (isSilent (peakOf (x.run (4 * x.step))), "CHANCE 0 %: never plays");

        SS early = play (6); early.micro = -32;
        x.write (0, { rest(), {}, early, rest() }, 4);
        out = x.run (4 * x.step);
        auto o = onsets (out);
        check (! o.empty() && std::abs (o[0] - (2 * x.step - x.step / 2)) <= 2, "MICRO -1/2: the hit lands half a step early (at "
                                                                                + juce::String (o.empty() ? -1 : o[0]) + ")");

        x.write (0, { rest(), play (6), rest(), rest() }, 4, 1, 50);
        out = x.run (4 * x.step);
        o = onsets (out);
        check (! o.empty() && std::abs (o[0] - (x.step + x.step / 4)) <= 2, "SWING 50 %: the off-beat step comes a quarter step late (at "
                                                                            + juce::String (o.empty() ? -1 : o[0]) + ")");

        // Rate 1/8: steps twice as long.
        x.write (0, { rest(), play (6) }, 2, 0);
        o = onsets (x.run (4 * x.step));
        check (! o.empty() && std::abs (o[0] - 2 * x.step) <= 1, "RATE 1/8: step 2 is an 1/8 in");

        // A second press stops it; Live stopped: a press starts it right away.
        x.write (0, { play (0), {}, {}, {} }, 4);
        out = x.run (20 * x.step, { { 5 * x.step, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100) } });
        check (! isSilent (out.getMagnitude (0, x.step, x.step)) && isSilent (out.getMagnitude (0, 5 * x.step + 300, 10 * x.step)),
               "a second hit on the pad stops its sequence");
        out = x.run (4 * x.step, {}, 3000, false);
        o = onsets (out);
        check (! o.empty() && std::abs (o[0] - 3000) <= 1 && x.diffFromBreak (out, 3000, 0, 2000) < 1.0e-5f,
               "Live stopped: hitting the pad starts the sequence immediately (free clock)");

        // Slots: switching lands on the next pattern start.
        x.write (1, { play (8) }, 1);       // slot 2: slice 9 on every step
        x.write (0, { play (0), {} }, 2);   // slot 1: slice 1 every 2 steps
        {
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            juce::AudioBuffer<float> all (2, 30 * x.step / kBlock * kBlock + kBlock), buf (2, kBlock);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
            const int blocks = all.getNumSamples() / kBlock;
            for (int b = 0; b < blocks; ++b)
            {
                if (b * kBlock >= x.step && b * kBlock < x.step + kBlock) setPadParam (x.r.p, 0, tb::params::padSliceSlot, 2.0f);
                runBlock (x.r.p, x.r.host, buf, midi);
                for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            }
            x.r.host.playing = false;
            check (x.diffFromBreak (all, 2 * x.step + 300, 8 * s2 + 300, x.step - 600) < 1.0e-5f && x.diffFromBreak (all, 0, 0, x.step) < 1.0e-5f,
                   "SLOT change mid-pattern lands on the next pattern start (step 3 plays slot 2)");
            x.r.p.requestPanic(); x.r.settle();
            setPadParam (x.r.p, 0, tb::params::padSliceSlot, 1.0f);
        }

        // FILL: rolls (x4) while held.
        x.write (0, { play (2), {}, {}, {} }, 4);
        x.r.p.getBridge().sliceFill[0].store (true);
        out = x.run (x.step);
        x.r.p.getBridge().sliceFill[0].store (false);
        const int q = (int) std::lround (x.step / 4.0);     // 1058.75 -> 1059
        check (x.diffFromBreak (out, q + 300, 2 * s2 + 300, q - 600) < 1.0e-3f, "FILL held: each hit rolls x4");

        // The main sequencer starts it (but never stops it).
        x.write (0, { play (0), {}, {}, {} }, 4);
        {
            tb::SeqStep st; st.on = true;
            auto& seq = x.r.p.getBridge().seq;
            seq.set (0, 0, st); seq.set (0, 4, st);
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            auto all = renderTimeline (x.r, 10 * x.step, {});
            x.r.host.playing = false;
            check (x.diffFromBreak (all, 0, 0, 2000) < 1.0e-5f && ! isSilent (all.getMagnitude (0, 8 * x.step, x.step)),
                   "a main-sequencer step starts the slice sequence, and the next one doesn't stop it");
            seq.clearAll();
            x.r.p.requestPanic(); x.r.settle();
        }

        // State.
        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        TwistedBreaksProcessor qp;
        qp.setStateInformation (blob.getData(), (int) blob.getSize());
        bool same = qp.getBridge().sliceSeq.getSlot (0, 1).written && qp.getBridge().sliceSeq.getSlot (0, 0).length == 4;
        for (int k = 0; k < 32; ++k) same &= qp.getBridge().sliceSeq.getStep (0, 0, k) == x.data.getStep (0, 0, k);
        check (same, "slice sequences (all slots and their settings) are saved with the set");
    }

    void testSliceSeqEditing()
    {
        std::cout << "\n== Phase 7.1: slice sequence editing ==" << std::endl;
        using SS = tb::SliceStep;
        using Op = TwistedBreaksProcessor::SliceOp;
        SliceRig x;
        x.equal16();
        auto& d = x.data;
        juce::Random rnd (7);

        check (! d.getSlot (0, 0).written, "a new slot plays the break as recorded (nothing written yet)");
        x.r.p.materializeSliceSlot (0, 0);
        check (d.getSlot (0, 0).written && d.getSlot (0, 0).length == 32 && d.getStep (0, 0, 2).kind == SS::play && d.getStep (0, 0, 2).slice == 1
                 && d.getStep (0, 0, 1).kind == SS::tie, "editing turns it into steps: EQUAL 16 on a 2-bar break = a slice every other 1/16, ties between");

        auto snapshot = [&] { std::array<SS, 32> a; for (int k = 0; k < 32; ++k) a[(size_t) k] = d.getStep (0, 0, k); return a; };
        const auto orig = snapshot();

        x.r.p.sliceSeqOp (0, Op::shuffle, rnd);
        auto sh = snapshot();
        bool beatsKept = true, changed = false;
        std::multiset<int> a, b;
        for (int k = 0; k < 32; ++k)
        {
            if (k % 4 == 0) beatsKept &= sh[(size_t) k] == orig[(size_t) k];
            changed |= ! (sh[(size_t) k] == orig[(size_t) k]);
            if (orig[(size_t) k].kind == SS::play) a.insert (orig[(size_t) k].slice);
            if (sh[(size_t) k].kind == SS::play) b.insert (sh[(size_t) k].slice);
        }
        check (changed && beatsKept && a == b, "SHUFFLE moves the hits around, keeps every beat in place, uses the same slices");

        x.r.p.sliceSeqOp (0, Op::reset, rnd);
        check (! d.getSlot (0, 0).written, "RESET: back to the break as recorded");

        x.r.p.materializeSliceSlot (0, 0);
        x.r.p.sliceSeqOp (0, Op::reverseOrder, rnd);
        check (d.getStep (0, 0, 31) == orig[0] && d.getStep (0, 0, 0) == orig[31], "REVERSE flips the step order");
        x.r.p.sliceSeqOp (0, Op::reset, rnd);
        x.r.p.materializeSliceSlot (0, 0);
        x.r.p.sliceSeqOp (0, Op::rotateRight, rnd);
        check (d.getStep (0, 0, 1) == orig[0] && d.getStep (0, 0, 0) == orig[31], "ROT > moves everything one step later");
        x.r.p.sliceSeqOp (0, Op::reset, rnd);
        x.r.p.materializeSliceSlot (0, 0);
        x.r.p.sliceSeqOp (0, Op::half, rnd);
        check (d.getStep (0, 0, 16) == orig[0] && d.getStep (0, 0, 18) == orig[2], "HALF repeats the first half");
        x.r.p.sliceSeqOp (0, Op::reset, rnd);
        x.r.p.materializeSliceSlot (0, 0);
        x.r.p.sliceSeqOp (0, Op::random, rnd);
        bool beats = true;
        for (int k = 0; k < 32; k += 4) beats &= d.getStep (0, 0, k) == orig[(size_t) k];
        check (beats, "RANDOM re-picks the off-beat slices only");
        x.r.p.sliceSeqOp (0, Op::reset, rnd);

        // The screen.
        std::unique_ptr<juce::AudioProcessorEditor> ed (x.r.p.createEditor());
        tb::ui::PadEditorPanel* pe = nullptr; tb::ui::SliceSeqPanel* panel = nullptr; tb::ui::MainView* view = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* p1 = dynamic_cast<tb::ui::PadEditorPanel*> (&c)) pe = p1;
            if (auto* p2 = dynamic_cast<tb::ui::SliceSeqPanel*> (&c)) panel = p2;
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (pe && panel && view, "found the SEQ page and the slice sequence screen");
        if (! (pe && panel && view)) return;
        pe->setPage (tb::ui::PadEditorPanel::Page::seq);
        check (panel->isVisible() && view->getView() == tb::ui::MainView::View::sliceSeq, "the SEQ tab puts the steps on the screen");

        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&] (juce::Point<float> pos, juce::Point<float> down, bool pressed, int clicks = 1)
        {
            const auto t = juce::Time::getCurrentTime();
            return juce::MouseEvent (src, pos, pressed ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                                     1.0f, 0, 0, 0, 0, panel, panel, t, down, t, clicks, false);
        };
        const auto c3 = panel->cellBounds (3).getCentre();
        panel->mouseDown (ev (c3, c3, true));
        panel->mouseDrag (ev (c3.translated (0.0f, -40.0f), c3, true));     // up 5 from TIE (-2) -> slice index 3
        panel->mouseUp (ev (c3.translated (0.0f, -40.0f), c3, false));
        check (panel->getSelectedStep() == 3 && d.getStep (0, 0, 3).kind == SS::play && d.getStep (0, 0, 3).slice == 3,
               "dragging a step up picks its slice (TIE -> slice 4)");
        panel->mouseDoubleClick (ev (c3, c3, false, 2));
        check (d.getStep (0, 0, 3).kind == SS::tie, "double-click makes it a TIE again");

        juce::Button* rev = nullptr; juce::Button* stepRec = nullptr;
        for (auto* ch : panel->getChildren())
            if (auto* bt = dynamic_cast<juce::Button*> (ch))
            {
                if (bt->getName() == "REV") rev = bt;
                if (bt->getName() == "STEP REC") stepRec = bt;
            }
        panel->selectStep (2);
        if (rev != nullptr) { rev->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (20); }
        check (rev != nullptr && d.getStep (0, 0, 2).rev, "REV reverses the selected step");

        // STEP REC: SPREAD pads write their slices into the steps in turn.
        x.r.p.spreadSlices (0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        setPadParam (x.r.p, 0, tb::params::padSliceSeq, 1.0f);
        x.r.settle();
        if (stepRec != nullptr) { stepRec->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (20); }
        panel->selectStep (0);
        panel->refresh();
        for (int pad : { 5, 9 })
        {
            x.r.p.triggerPadFromUi (pad, 1.0f, true);
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            runBlock (x.r.p, x.r.host, buf, midi);
            panel->refresh();
        }
        check (d.getStep (0, 0, 0).slice == 5 && d.getStep (0, 0, 1).slice == 9 && d.getStep (0, 0, 1).kind == SS::play && panel->getSelectedStep() == 2,
               "STEP REC: tapping SPREAD pads 6 and 10 writes slices 6 and 10 into steps 1 and 2");
        pe->setPage (tb::ui::PadEditorPanel::Page::sound);
        check (view->getView() == tb::ui::MainView::View::crt, "leaving the SEQ page brings the CRT back");
    }

    void testSliceSeqSwap()
    {
        std::cout << "\n== Phase 7.1: slice sequences + SWAP ==" << std::endl;
        SliceRig x;
        x.equal16();
        x.r.load (1, writeAudio ("sseq_break", x.brk, 48000.0, Fmt::wavFloat));     // same file: shared audio
        x.r.p.getSamples().equalSlice (1);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        for (int p = 0; p < 2; ++p)
        {
            setPadParam (x.r.p, p, tb::params::padSliceSeq, 1.0f);
            setPadParam (x.r.p, p, tb::params::padSwap, 1.0f);
            setPadParam (x.r.p, p, tb::params::padPan, p == 0 ? -1.0f : 1.0f);
        }
        x.r.settle();
        const int hit2 = 3 * x.step + 1000;      // mid step 4
        auto out = x.run (8 * x.step, { { hit2, juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127) } });
        const int first = firstSoundIn (out, 1, 0, out.getNumSamples());
        check (std::abs (first - 4 * x.step) <= 2, "hitting pad 2 mid-step: its sequence takes over on pad 1's next step (at "
                                                    + juce::String (first) + ", step " + juce::String (4 * x.step) + ")");
        check (isSilent (out.getMagnitude (0, 4 * x.step + 300, 3 * x.step)), "...and pad 1's sequence stops there");
        check (x.diffFromBreak (out, 4 * x.step + 300, 4 * x.step + 300, x.step - 600, 1) < 1.0e-4f,
               "...continuing from the same point in the bar (step 5 of its own pattern)");
    }

    //==========================================================================
    //  PHASE 8: snapshots, break bank, missing samples
    //==========================================================================
    void testSnapshots (const juce::File& tone)
    {
        std::cout << "\n== Phase 8: snapshots ==" << std::endl;
        using namespace tb::params;
        SeqRig x;                                // blips on pads 1 (left) and 2 (right), 120 BPM
        auto& snaps = x.r.p.getSnapshots();
        auto raw = [&] (const juce::String& id) { return x.r.p.getParameters().getRawParameterValue (id)->load(); };
        auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };

        // Snapshot 1: pad 1 at -6 dB, LPF on group A, steps on pad 1; master at -3 dB.
        setPadParam (x.r.p, 0, padGain, -6.0f);
        setParam (x.r.p, tb::fx::paramId (0, tb::fx::lpf), 1.0f);
        setParam (x.r.p, masterGain, -3.0f);
        x.step (0, 0); x.step (0, 4);
        snaps.store (0);
        check (snaps.getState (0) == tb::SnapshotManager::SlotState::active, "STORE: slot 1 holds the current state");

        // Change everything, store snapshot 2.
        setPadParam (x.r.p, 0, padGain, 0.0f);
        setParam (x.r.p, tb::fx::paramId (0, tb::fx::lpf), 0.0f);
        x.seq.clearAll();
        x.step (1, 2);
        x.r.load (0, tone);                      // a different sample on pad 1
        snaps.store (1);
        setParam (x.r.p, masterGain, 0.0f);

        // Recall 1, NOW (Live stopped).
        snaps.recall (0, switchNow);
        pump();
        {
            juce::AudioBuffer<float> buf (2, kBlock); juce::MidiBuffer midi;
            runBlock (x.r.p, x.r.host, buf, midi);
        }
        snaps.poll();
        pump();
        check (std::abs (raw (padId (0, padGain)) + 6.0f) < 0.01f && raw (tb::fx::paramId (0, tb::fx::lpf)) > 0.5f,
               "RECALL 1: pad gain and group A's LPF come back (parameters, so Live sees them too)");
        check (x.seq.get (0, 0).on && x.seq.get (0, 4).on && ! x.seq.get (1, 2).on, "...the sequencer pattern comes back");
        check (x.r.p.getSamples().getInfo (0).name == "seq_blip", "...and pad 1's sample comes back");
        check (std::abs (raw (masterGain)) < 0.01f, "...the master section is left alone");
        check (snaps.getState (0) == tb::SnapshotManager::SlotState::active, "slot 1 shows ACTIVE");

        // Exactness: NEXT BAR, armed mid bar 1. Old snapshot: pad 1 on its own
        // step 1 pulled 94 samples early (95906); new: pad 2 on step 1 (96000).
        setPadParam (x.r.p, 0, padGain, 0.0f);
        x.seq.clearAll();
        x.step (0, 0, 127, -2);                  // -2/128 of a step = -94 samples
        snaps.store (2);
        x.seq.clearAll();
        x.step (1, 0);
        snaps.store (3);
        snaps.recall (2, switchNow);
        { juce::AudioBuffer<float> buf (2, kBlock); juce::MidiBuffer midi; runBlock (x.r.p, x.r.host, buf, midi); }
        snaps.poll(); pump();
        x.r.settle();

        x.r.host.ppq = 0.0; x.r.host.playing = true;
        const int blocks = 220;
        juce::AudioBuffer<float> all (2, blocks * kBlock), buf (2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            if (b == 60) { snaps.recall (3, switchBar); }        // ~30700: mid bar 1
            runBlock (x.r.p, x.r.host, buf, midi);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
            if (b > 60) snaps.poll();
        }
        x.r.host.playing = false;
        const auto left = onsets (all, 0), right = onsets (all, 1);
        check (std::find (left.begin(), left.end(), 96000 - 94) != left.end() && std::find (right.begin(), right.end(), 96000) != right.end()
                 && std::find (right.begin(), right.end(), 0) == right.end(),
               "NEXT BAR: the switch lands on the bar line sample (96000, mid-block): old step at 95906 plays, new step at 96000 plays ("
                 + listOf (left) + " | " + listOf (right) + ")");
        check (std::find (left.begin(), left.end(), 192000 - 94) == left.end(), "...and the old pattern is gone after it");
        check (snaps.getState (3) == tb::SnapshotManager::SlotState::active && x.seq.get (1, 0).on && ! x.seq.get (0, 0).on,
               "slot 4 is active and its pattern is now the one you see");
        x.r.p.requestPanic(); x.r.settle();

        // MIDI Program Change 2 -> snapshot 2; automating "Snapshot" = 3 -> snapshot 3.
        {
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::programChange (1, 1), 0);
            juce::AudioBuffer<float> b2 (2, kBlock);
            runBlock (x.r.p, x.r.host, b2, m);
            snaps.poll(); pump();
            runBlock (x.r.p, x.r.host, b2, m);
            snaps.poll(); pump();
            check (snaps.getActive() == 1, "MIDI Program Change 2 recalls snapshot 2");
            setParam (x.r.p, snapRecall, 3.0f);
            snaps.poll(); pump();
            runBlock (x.r.p, x.r.host, b2, m);
            snaps.poll(); pump();
            check (snaps.getActive() == 2, "automating the Snapshot parameter to 3 recalls snapshot 3");
        }

        // Saved with the set.
        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        {
            TwistedBreaksProcessor q;
            q.setStateInformation (blob.getData(), (int) blob.getSize());
            pump();
            check (q.getSnapshots().getState (0) != tb::SnapshotManager::SlotState::empty
                     && q.getSnapshots().getState (3) != tb::SnapshotManager::SlotState::empty
                     && q.getSnapshots().getState (5) == tb::SnapshotManager::SlotState::empty,
                   "snapshots are saved with the set");
        }
    }

    void testBreakBank()
    {
        std::cout << "\n== Phase 8: break bank ==" << std::endl;
        SliceRig x;
        x.equal16();
        auto& bank = x.r.p.getBank();
        auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (50); };

        juce::AudioBuffer<float> inv (x.brk);
        inv.applyGain (-1.0f);                    // break B: the same groove, upside down
        const auto fileA = writeAudio ("bank_a", x.brk, 48000.0, Fmt::wavFloat);
        const auto fileB = writeAudio ("bank_b", inv, 48000.0, Fmt::wavFloat);
        const auto slices = tb::SliceTable::equal (x.brk.getNumSamples(), 16);
        bank.loadFile (0, fileA, &slices);
        bank.loadFile (1, fileB, &slices);
        for (int i = 0; i < 20 && (bank.get (0).status != tb::BreakBank::Status::ready || bank.get (1).status != tb::BreakBank::Status::ready); ++i) pump();
        x.r.settle();
        check (bank.get (0).status == tb::BreakBank::Status::ready && bank.get (1).slices.count == 16, "two breaks loaded into bank slots 1 and 2");

        // Pad 1 = SLICE SEQ, following the bank. Untouched pattern: replays the active break.
        setPadParam (x.r.p, 0, tb::params::padFollowBank, 1.0f);
        x.r.settle();
        const int beat = 4 * x.step;
        juce::AudioBuffer<float> all (2, 6 * beat / kBlock * kBlock + kBlock), buf (2, kBlock);
        x.r.host.ppq = 0.0; x.r.host.playing = true;
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        const int blocks = all.getNumSamples() / kBlock;
        for (int b = 0; b < blocks; ++b)
        {
            if (b == 10) { setParam (x.r.p, tb::params::bankAt, (float) tb::params::switchBeat); setParam (x.r.p, tb::params::bankActive, 2.0f); }
            runBlock (x.r.p, x.r.host, buf, midi);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, b * kBlock, buf, ch, 0, kBlock);
        }
        x.r.host.playing = false;
        check (x.diffFromBreak (all, 0, 0, beat - 50) < 1.0e-5f, "FOLLOW BANK: the pad plays bank break 1");
        float d = 0.0f;
        for (int i = 300; i < beat - 300; ++i) d = juce::jmax (d, std::abs (all.getSample (0, beat + i) - inv.getSample (0, beat + i)));
        check (d < 1.0e-4f, "Break Bank 2 (NEXT BEAT) set mid-beat: from the next beat the same pattern plays break 2 (err " + juce::String (d, 6) + ")");
        check (x.r.p.getEngine().getActiveBank() == 1, "...and the bank shows slot 2 active");
        x.r.p.requestPanic(); x.r.settle();

        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        {
            TwistedBreaksProcessor q;
            q.setStateInformation (blob.getData(), (int) blob.getSize());
            for (int i = 0; i < 20 && q.getBank().get (1).status != tb::BreakBank::Status::ready; ++i) pump();
            check (q.getBank().get (1).status == tb::BreakBank::Status::ready && q.getBank().get (1).slices == slices,
                   "the bank (files + markers) is saved with the set");
        }
    }

    void testFindMissing()
    {
        std::cout << "\n== Phase 8: find missing samples ==" << std::endl;
        const auto oldDir = testDir().getChildFile ("old_place");
        const auto newDir = testDir().getChildFile ("new_place").getChildFile ("deep");
        oldDir.createDirectory(); newDir.createDirectory();
        const auto orig = oldDir.getChildFile ("lost_break.wav");
        { const auto f = writeAudio ("lost_break", makeBreak (44100.0), 44100.0, Fmt::wav16); f.moveFileTo (orig); }

        juce::MemoryBlock blob;
        {
            Rig r;
            r.load (2, orig);
            r.p.getSamples().equalSlice (2);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
            r.p.getStateInformation (blob);
        }
        orig.moveFileTo (newDir.getChildFile ("lost_break.wav"));     // the folder was moved

        Rig q;
        q.p.setStateInformation (blob.getData(), (int) blob.getSize());
        waitForLoads (q.p);
        check (q.p.getSamples().countMissing() == 1 && q.p.getSamples().getInfo (2).status == tb::SampleManager::Status::missing,
               "the moved sample shows MISSING (the set still opens)");
        const int found = q.p.getSamples().relocateMissing (testDir().getChildFile ("new_place"));
        waitForLoads (q.p);
        const auto info = q.p.getSamples().getInfo (2);
        check (found == 1 && info.status == tb::SampleManager::Status::ready && info.slices.count == 16,
               "FIND MISSING: found in the new folder (searching sub-folders), reloaded with its slices kept");
    }

    void testSnapshotView()
    {
        std::cout << "\n== Phase 8: SNAPSHOTS view ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        tb::ui::SnapshotPanel* panel = nullptr; juce::Button* key = nullptr; juce::Button* store = nullptr; juce::Button* follow = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* sp = dynamic_cast<tb::ui::SnapshotPanel*> (&c)) panel = sp;
            if (auto* sk = dynamic_cast<tb::ui::SelectorKey*> (&c)) if (sk->getName() == "SNAPSHOTS") key = sk;
            if (auto* b = dynamic_cast<juce::Button*> (&c))
            {
                if (b->getName() == "STORE") store = b;
                if (b->getName() == "FOLLOW BANK") follow = b;
            }
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (panel && key && store && follow, "found the SNAPSHOTS key and view, STORE and FOLLOW BANK");
        if (! (panel && key && store && follow)) return;
        check (key->isEnabled(), "SNAPSHOTS key is online (every view now live)");
        key->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (panel->isVisible(), "it shows the snapshot slots and the break bank");

        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto click = [&] (juce::Point<float> pos)
        {
            const auto t = juce::Time::getCurrentTime();
            panel->mouseDown (juce::MouseEvent (src, pos, juce::ModifierKeys::leftButtonModifier, 1.0f, 0, 0, 0, 0, panel, panel, t, pos, t, 1, false));
        };
        store->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        click (panel->snapBounds (2).getCentre());
        check (r.p.getSnapshots().getState (2) == tb::SnapshotManager::SlotState::active && ! store->getToggleState(),
               "STORE, then click slot 3: stored (and STORE switches off)");
        click (panel->bankBounds (4).getCentre());
        check (juce::roundToInt (r.p.getParameters().getRawParameterValue (tb::params::bankActive)->load()) == 5,
               "clicking bank slot 5 makes it the active break (automatable)");
        follow->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        check (r.p.getParameters().getRawParameterValue (tb::params::padId (0, tb::params::padFollowBank))->load() > 0.5f,
               "FOLLOW BANK on the SWAP page sets the pad to follow the bank");
    }

    void testSnapshotStress (const juce::File& tone)
    {
        std::cout << "\n== Phase 8: snapshot switching every beat for 2 minutes ==" << std::endl;
        SeqRig x;
        auto& snaps = x.r.p.getSnapshots();
        x.r.load (2, tone);
        x.step (0, 0); x.step (0, 8);
        setParam (x.r.p, tb::fx::paramId (0, tb::fx::delay), 1.0f);
        snaps.store (0);
        x.seq.clearAll();
        x.step (1, 4); x.step (1, 12);
        setParam (x.r.p, tb::fx::paramId (0, tb::fx::delay), 0.0f);
        setParam (x.r.p, tb::fx::paramId (0, tb::fx::stutter), 1.0f);
        snaps.store (1);

        x.r.host.ppq = 0.0; x.r.host.playing = true;
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        const int beats = 240;                                   // 2 minutes at 120 BPM
        const int blocksPerBeat = 24000 / kBlock + 1;
        double worstMs = 0.0;
        std::vector<double> times;
        times.reserve ((size_t) (beats * blocksPerBeat));
        int switches = 0;
        const int buffersBefore = tb::SampleBuffer::liveInstances();
        bool sane = true;
        for (int beat = 0; beat < beats; ++beat)
        {
            snaps.recall (beat % 2, tb::params::switchBeat);
            for (int b = 0; b < blocksPerBeat; ++b)
            {
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                runBlock (x.r.p, x.r.host, buf, midi);
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                worstMs = juce::jmax (worstMs, ms);
                times.push_back (ms);
                for (int i = 0; i < kBlock; i += 64) sane &= std::isfinite (buf.getSample (0, i));
                snaps.poll();
            }
            if (snaps.getActive() == beat % 2) ++switches;
        }
        x.r.host.playing = false;
        const double blockMs = kBlock * 1000.0 / kSr;
        check (switches >= beats - 2, "240 switches, one per beat: every one landed (" + juce::String (switches) + ")");
        // Judge the 99.9th percentile: on a shared CI machine one block in ~11,000 can be
        // stalled by the OS (a preemption, not our code). A real cost shows up here too.
        std::sort (times.begin(), times.end());
        const double p999 = times.empty() ? 0.0 : times[(size_t) ((double) (times.size() - 1) * 0.999)];
        check (p999 < blockMs * 0.5, "no CPU spike: 99.9 % of blocks under " + juce::String (p999, 2) + " ms of their "
                                       + juce::String (blockMs, 1) + " ms, switching included (slowest single block " + juce::String (worstMs, 2) + " ms)");
        check (sane, "...and the audio stayed clean (no NaN / inf)");
        check (snaps.framesInFlight() <= 2 && tb::SampleBuffer::liveInstances() <= buffersBefore + 1,
               "nothing piles up: old frames are retired, no extra sample memory (" + juce::String (snaps.framesInFlight()) + " frames in flight)");
    }

    void testFixedVelocity (const juce::File& tone)
    {
        std::cout << "\n== 0.5.2: fixed velocity ==" << std::endl;
        Rig r;
        r.load (0, tone);
        r.settle();
        const auto hard = renderNote (r.p, r.host, 36, 127, 1);
        r.settle();
        auto soft = renderNote (r.p, r.host, 36, 20, 1);
        check (peakOf (soft) < peakOf (hard) * 0.3f, "normally a soft hit is quieter");
        r.settle();

        setPadParam (r.p, 0, tb::params::padFixVel, 1.0f);
        r.settle();
        soft = renderNote (r.p, r.host, 36, 20, 1);
        float diff = 0.0f;
        for (int i = 0; i < kBlock; ++i) diff = juce::jmax (diff, std::abs (soft.getSample (0, i) - hard.getSample (0, i)));
        check (diff < 1.0e-6f, "FIXED VEL on: a soft hit plays at exactly full level");
        r.settle();
        r.p.triggerPadFromUi (0, 0.1f, true);
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        runBlock (r.p, r.host, buf, midi);
        check (std::abs (peakOf (buf) - peakOf (hard)) < 1.0e-5f, "...also for mouse clicks low on the pad");
        r.settle();

        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::Button* fix = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* b = dynamic_cast<juce::Button*> (&c)) if (b->getTooltip().startsWith ("FIXED VELOCITY")) fix = b;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (fix != nullptr && fix->getToggleState(), "the PLAY page FIX key shows it's on");
        if (fix != nullptr)
        {
            fix->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
            check (r.p.getParameters().getRawParameterValue (tb::params::padId (0, tb::params::padFixVel))->load() < 0.5f,
                   "clicking FIX turns fixed velocity off (automatable: pad01_fixvel)");
        }
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
    //  A6: sequence pads
    //==========================================================================
    void testSeqPads()
    {
        std::cout << "\n== 0.14: sequence pads (SEQ ONE / SEQ LOOP, LAUNCH, SWAP between sequences) ==" << std::endl;
        check (TwistedBreaksProcessor().getParameters().processor.getParameters().size() == 1065,
               "LAUNCH, SEQ LEN and SWAP FROM on every pad (1065 parameters in total)");

        SeqRig x;                                         // 120 BPM: 1/16 = 6000, beat = 24000, bar = 96000
        auto setPad = [&x] (int pad, const char* id, float v) { setPadParam (x.r.p, pad, id, v); };
        auto lane = [&x] (int pad, std::initializer_list<int> steps) { x.seq.clearLane (pad); for (int k : steps) x.step (pad, k); };
        auto hit  = [] (int at, int note = 36, int vel = 127) { return TimedEvent { at, juce::MidiMessage::noteOn (1, note, (juce::uint8) vel) }; };
        auto list = [] (const std::vector<int>& v) { return listOf (v); };
        auto run  = [&x] (int n, std::vector<TimedEvent> ev, bool playing = true) { return x.run (n, ev, playing); };

        x.seq.clearAll();
        setPad (0, tb::params::padMode, (float) tb::params::modeSeqLoop);
        setPad (0, tb::params::padLaunch, (float) tb::params::launchBeat);
        lane (0, { 0, 2 });
        x.r.settle();
        auto o = onsets (run (170000, { hit (30000) }), 0);
        check (o == std::vector<int> { 48000, 60000, 144000, 156000 },
               "SEQ LOOP: nothing until the pad is hit; a hit at 30000 starts its lane on the next beat (48000) and it loops every 16 steps (" + list (o) + ")");

        setPad (0, tb::params::padMode, (float) tb::params::modeSeqOne);
        x.r.settle();
        o = onsets (run (250000, { hit (30000) }), 0);
        check (o == std::vector<int> { 48000, 60000 }, "SEQ ONE: the lane plays once, then stops (" + list (o) + ")");

        const std::pair<int, std::vector<int>> launches[] = { { tb::params::launchNow, { 31000, 43000 } }, { tb::params::launchSixteenth, { 36000, 48000 } },
                                                              { tb::params::launchBar, { 96000, 108000 } } };
        bool launchOk = true; juce::String desc;
        for (auto& [l, expect] : launches)
        {
            setPad (0, tb::params::padLaunch, (float) l);
            x.r.settle();
            o = onsets (run (130000, { hit (31000) }), 0);
            launchOk = launchOk && o == expect;
            desc << list (o) << " / ";
        }
        check (launchOk, "LAUNCH NOW / 1/16 / BAR: step 1 lands on that exact sample, mid-block (" + desc.dropLastCharacters (3) + ")");

        // SEQ LEN, and SEQ ONE hit again restarts at the next boundary.
        setPad (0, tb::params::padSeqLen, 4.0f);
        setPad (0, tb::params::padLaunch, (float) tb::params::launchSixteenth);
        lane (0, { 0, 1 });
        x.r.settle();
        o = onsets (run (60000, { hit (0), hit (10000) }), 0);
        check (o == std::vector<int> { 0, 6000, 12000, 18000 },
               "SEQ LEN 4 + SEQ ONE: a second hit starts it over from step 1 on the next 1/16 (" + list (o) + ")");
        setPad (0, tb::params::padSeqLen, 0.0f);

        // SEQ LOOP: hit again = stop at the next boundary; a third hit cancels the stop.
        setPad (0, tb::params::padMode, (float) tb::params::modeSeqLoop);
        setPad (0, tb::params::padLaunch, (float) tb::params::launchBeat);
        x.seq.clearLane (0);
        for (int k = 0; k < 16; ++k) x.step (0, k);
        x.r.settle();
        o = onsets (run (110000, { hit (0), hit (50000) }), 0);
        check (! o.empty() && o.back() == 66000 && o.size() == 12, "hitting a looping sequence stops it at the next beat (last hit 66000, none from 72000)");
        o = onsets (run (110000, { hit (0), hit (50000), hit (60000) }), 0);
        check (o.size() == 19 && o.back() == 108000, "...and hitting it once more before then cancels the stop");

        // Velocity of the hit scales the steps (VEL 100 %).
        auto lv = [&] (int vel) { auto out = run (8000, { hit (0, 36, vel) }); return out.getSample (0, 45); };
        setPad (0, tb::params::padLaunch, (float) tb::params::launchNow);
        x.r.settle();
        const float full = lv (127), half = lv (64);
        check (std::abs (half / full - 64.0f / 127.0f) < 0.01f, "a softer hit plays the whole sequence softer (x" + juce::String (half / full, 3) + ")");

        // Conditions count from the launch: FIRST = its first time round.
        lane (0, { 4 });
        { tb::SeqStep st; st.on = true; st.velocity = 127; st.cond = tb::condFirst; x.seq.set (0, 0, st); }
        setPad (0, tb::params::padLaunch, (float) tb::params::launchBeat);
        x.r.settle();
        o = onsets (run (180000, { hit (30000) }), 0);
        check (o == std::vector<int> { 48000, 72000, 168000 }, "conditions count passes from the launch (FIRST only on its first time round) (" + list (o) + ")");

        // Live stopped: the free clock, straight away.
        lane (0, { 0, 1 });
        setPad (0, tb::params::padLaunch, (float) tb::params::launchNow);
        x.r.settle();
        o = onsets (run (20000, { hit (1000) }, false), 0);
        check (o.size() >= 2 && o[0] == 1000 && o[1] == 7000, "with Live stopped a hit runs it on the free clock (" + list (o) + ")");

        // ---- SWAP groups between sequences.
        auto swapRig = [&] (int at, int from, std::initializer_list<int> lane1)
        {
            x.seq.clearAll();
            for (int pad : { 0, 1 })
            {
                setPad (pad, tb::params::padMode, (float) tb::params::modeSeqLoop);
                setPad (pad, tb::params::padLaunch, (float) tb::params::launchNow);
                setPad (pad, tb::params::padSwap, 1.0f);
                setPad (pad, tb::params::padSwapAt, (float) at);
            }
            setPad (1, tb::params::padSwapFrom, (float) from);
            for (int k = 0; k < 16; ++k) x.step (0, k);
            lane (1, lane1);
            x.r.settle();
            return run (130000, { hit (0, 36), hit (50000, 37) });
        };
        const int every[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
        auto all16 = std::initializer_list<int> { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
        juce::ignoreUnused (every);
        bool swapOk = true; desc = {};
        const std::pair<int, int> ats[] = { { tb::params::swapAtSlice, 54000 }, { tb::params::swapAtSixteenth, 54000 },
                                            { tb::params::swapAtBeat, 72000 }, { tb::params::swapAtBar, 96000 } };
        for (auto& [at, landing] : ats)
        {
            auto out = swapRig (at, 0, all16);
            const auto l = onsets (out, 0), r = onsets (out, 1);
            swapOk = swapOk && ! l.empty() && l.back() == landing - 6000 && ! r.empty() && r.front() == landing;
            desc << (l.empty() ? -1 : l.back()) << "|" << (r.empty() ? -1 : r.front()) << " ";
        }
        check (swapOk, "SWAP between sequences at STEP / 1/16 / BEAT / BAR: the outgoing one's last step and the incoming one's first meet exactly ("
                         + desc.trim() + ")");
        auto out = swapRig (tb::params::swapAtSlice, 0, { 0 });
        auto r1 = onsets (out, 1);
        check (! r1.empty() && r1.front() == 96000, "SWAP FROM SAME STEP: the incoming sequence carries on from the same position (its step 1 next at 96000)");
        out = swapRig (tb::params::swapAtSlice, 1, { 0 });
        r1 = onsets (out, 1);
        check (! r1.empty() && r1.front() == 54000, "SWAP FROM STEP 1: it starts from the top, right at the hand-off (54000)");

        // A sequence handing over to a plain pad.
        setPad (1, tb::params::padMode, (float) tb::params::modeOneShot);
        x.seq.clearLane (1);                             // a plain pad's lane runs with Live: keep it empty here
        x.r.settle();
        out = run (100000, { hit (0, 36), hit (50000, 37) });
        auto l0 = onsets (out, 0); r1 = onsets (out, 1);
        check (! l0.empty() && l0.back() == 48000 && r1 == std::vector<int> { 54000 }, "mixed SWAP group: a sequence hands over to a plain pad on its next step ("
                 + (l0.empty() ? juce::String ("-") : juce::String (l0.back())) + " | " + listOf (r1) + ")");
        for (int pad : { 0, 1 }) setPad (pad, tb::params::padSwap, 0.0f);

        // Choke stops a running sequence; Live stopping ends it.
        setPad (0, tb::params::padChoke, 1.0f);
        setPad (1, tb::params::padChoke, 1.0f);
        x.r.settle();
        out = run (100000, { hit (0, 36), hit (50000, 37) });
        l0 = onsets (out, 0);
        check (! l0.empty() && l0.back() == 48000, "a choke-group partner stops a running sequence");
        setPad (0, tb::params::padChoke, 0.0f); setPad (1, tb::params::padChoke, 0.0f);
        {
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            renderTimeline (x.r, 20000, { hit (0, 36) });
            x.r.host.playing = false;
            renderTimeline (x.r, 4096, {});
            x.r.host.playing = true;
            auto after = renderTimeline (x.r, 60000, {});
            x.r.host.playing = false;
            x.r.p.requestPanic(); x.r.settle();
            check (onsets (after, 0).empty(), "stopping Live ends a launched sequence");
        }

        // A loop-brace wrap: the launched sequence carries straight on (no gap, no double).
        {
            x.seq.clearAll();
            setPad (0, tb::params::padMode, (float) tb::params::modeSeqLoop);
            setPad (0, tb::params::padLaunch, (float) tb::params::launchNow);
            for (int k = 0; k < 16; ++k) x.step (0, k);
            x.r.settle();
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            auto first = renderTimeline (x.r, 30 * kBlock, { hit (0, 36) });      // 15360 samples
            x.r.host.ppq = 0.0;                                                       // the brace wraps back to bar 1
            auto second = renderTimeline (x.r, 40 * kBlock, {});
            x.r.host.playing = false; x.r.p.requestPanic(); x.r.settle();
            std::vector<int> all = onsets (first, 0);
            for (int t : onsets (second, 0)) all.push_back (t + 30 * kBlock);
            bool even = all.size() >= 5;
            for (size_t i = 1; i < all.size(); ++i) even = even && all[i] - all[i - 1] == 6000;
            check (even, "a loop-brace wrap doesn't upset a launched sequence: still every 1/16 straight through (" + listOf (all) + ")");
        }

        // Saved with the set; an older set's modes keep their meaning.
        setPad (0, tb::params::padSeqLen, 12.0f);
        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        auto raw = [&q] (const char* id) { return q.getParameters().getRawParameterValue (tb::params::padId (0, id))->load(); };
        check (juce::roundToInt (raw (tb::params::padMode)) == tb::params::modeSeqLoop && juce::roundToInt (raw (tb::params::padSeqLen)) == 12,
               "SEQ modes, LAUNCH, SEQ LEN and SWAP FROM are saved with the set (mode ONE / LOOP / SLICE keep their old values)");
        setPad (0, tb::params::padMode, 0.0f);
        x.r.settle();
    }

    void testSeqPadView()
    {
        std::cout << "\n== 0.14: sequence pads on screen ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::PadEditorPanel* editor = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* e = dynamic_cast<tb::ui::PadEditorPanel*> (&c)) editor = e;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        if (editor == nullptr) { check (false, "found the pad editor"); return; }
        editor->setPage (tb::ui::PadEditorPanel::Page::play);
        setPadParam (r.p, 0, tb::params::padMode, (float) tb::params::modeSeqLoop);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
        bool launchShown = false, syncHidden = true;
        for (auto* ch : editor->getChildren())
        {
            if (auto* rs = dynamic_cast<tb::ui::RotarySelector*> (ch)) launchShown = launchShown || (rs->isVisible() && rs->getTooltip().contains ("starts this pad's sequence"));
            if (auto* ts = dynamic_cast<tb::ui::ToggleSwitch*> (ch)) if (ts->getName().contains ("SYNC") || ts->getTooltip().contains ("tempo")) syncHidden = syncHidden && ! ts->isVisible();
        }
        check (launchShown && syncHidden, "PLAY page in SEQ LOOP: LAUNCH and LEN take the places of SYNC and RATE");
        editor->setPage (tb::ui::PadEditorPanel::Page::swap);
        juce::Button* from = nullptr;
        for (auto* ch : editor->getChildren()) if (auto* b = dynamic_cast<juce::Button*> (ch)) if (b->getButtonText() == "FROM STEP 1") from = b;
        check (from != nullptr && from->isVisible(), "SWAP page: FROM STEP 1");
        if (from != nullptr)
        {
            from->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
            check (r.p.getParameters().getRawParameterValue (tb::params::padId (0, tb::params::padSwapFrom))->load() > 0.5f, "...which sets Swap From to Step 1");
        }
        setPadParam (r.p, 0, tb::params::padMode, 0.0f);
    }

    //==========================================================================
    //  PHASE 13: Scene A / B + crossfader
    //==========================================================================
    void testScenes()
    {
        std::cout << "\n== Phase 13: scenes A / B + crossfader ==" << std::endl;
        {
            TwistedBreaksProcessor p;
            auto* b1 = dynamic_cast<juce::RangedAudioParameter*> (p.getParameters().getParameter (tb::params::padId (16, tb::params::padGain)));
            check (p.getParameters().processor.getParameters().size() == 1065 && b1 != nullptr && b1->getName (64) == "Pad B01 Gain"
                     && b1->getVersionHint() == tb::params::kV15,
                   "32 pads: scene B's 16 have their own parameters (\"Pad B01 Gain\" ...), new in 0.13 (969 in total)");
        }

        SeqRig x;
        x.r.load (16, writeAudio ("scene_b_blip", [] { juce::AudioBuffer<float> b (1, 4000); b.clear(); for (int i = 0; i < 50; ++i) b.setSample (0, i, 0.5f); return b; }(),
                                  48000.0, Fmt::wavFloat));
        setPadParam (x.r.p, 16, tb::params::padPan, 1.0f);             // B01 on the right, pad 01 on the left
        x.r.settle();
        x.seq.clearAll();
        x.step (0, 0); x.step (16, 4);
        auto out = x.run (30000);
        check (onsets (out, 0) == std::vector<int> { 0 } && onsets (out, 1) == std::vector<int> { 24000 },
               "both scenes play at once: pad 01's lane on the left, B01's lane on the right");

        auto levels = [&] (float xf, int curve, int cut = -1)
        {
            setParam (x.r.p, tb::params::xfade, xf);
            setParam (x.r.p, tb::params::xfadeCurve, (float) curve);
            x.r.p.getEngine().setCrossfaderCut (cut);
            x.r.settle(); x.r.settle();
            x.seq.clearAll();
            x.step (0, 0); x.step (16, 0);
            auto o = x.run (8000);
            x.r.p.getEngine().setCrossfaderCut (-1);
            return std::make_pair (o.getSample (0, 20) / 0.5f, o.getSample (1, 20) / 0.5f);
        };
        auto near = [] (std::pair<float, float> v, float a, float b) { return std::abs (v.first - a) < 0.01f && std::abs (v.second - b) < 0.01f; };
        auto txt  = [] (std::pair<float, float> v) { return juce::String (v.first, 2) + " / " + juce::String (v.second, 2); };
        const auto centre = levels (0.5f, 0), left = levels (0.0f, 0), quarter = levels (0.75f, 0), sharp = levels (0.75f, 1), edge = levels (0.975f, 1);
        check (near (centre, 1, 1), "crossfader in the middle: both scenes at full level (an old set sounds the same) (" + txt (centre) + ")");
        check (near (left, 1, 0), "all the way to A: only scene A (" + txt (left) + ")");
        check (near (quarter, 0.5f, 1), "SMOOTH at 3/4: A half, B full (" + txt (quarter) + ")");
        check (near (sharp, 1, 1) && near (edge, 0.5f, 1), "SHARP: both full until the last 5 %, then a quick cut (" + txt (sharp) + ", " + txt (edge) + ")");
        const auto cutA = levels (1.0f, 0, 0);
        check (near (cutA, 1, 0), "CUT A held: only scene A, wherever the fader is (" + txt (cutA) + ")");
        setParam (x.r.p, tb::params::xfade, 0.5f);

        // MIDI: notes 36-51 play the scene on view.
        x.seq.clearAll();
        auto o = x.run (8000, { { 0, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127) } }, false);
        const bool aPlayed = ! onsets (o, 0).empty() && onsets (o, 1).empty();
        setParam (x.r.p, tb::params::sceneView, 1.0f);
        x.r.settle();
        o = x.run (8000, { { 0, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127) } }, false);
        check (aPlayed && onsets (o, 0).empty() && ! onsets (o, 1).empty(), "MIDI C1 plays pad 01 while A is on view, B01 while B is (a 16-pad controller plays what you see)");
        x.r.p.getEngine().setPadNote (16, 60);
        setParam (x.r.p, tb::params::sceneView, 0.0f);
        x.r.settle();
        o = x.run (8000, { { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127) } }, false);
        check (! onsets (o, 1).empty() && x.r.p.getEngine().getPadNote (0) == 36, "a note learned for a B pad plays it whatever is on view (and A keeps its own)");
        x.r.p.getEngine().setPadNote (16, 36);

        check (! tb::SnapshotManager::isExcluded (tb::params::padId (20, tb::params::padPitch)) && tb::SnapshotManager::isExcluded (tb::params::xfade)
                 && tb::SnapshotManager::isExcluded (tb::params::sceneView),
               "snapshots hold both scenes' pads, never the crossfader or the view");

        // State: scene B's sample and pattern come back.
        x.step (17, 3);
        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        waitForLoads (q);
        check (q.getSamples().getInfo (16).buffer != nullptr && q.getBridge().seq.get (17, 3).on, "scene B's samples and patterns are saved with the set");

        // An old set (no scene B anywhere) opens with an empty scene B.
        {
            auto xml = x.r.p.getParameters().copyState().createXml();
            juce::MemoryBlock fresh;
            TwistedBreaksProcessor old;
            old.getStateInformation (fresh);
            TwistedBreaksProcessor r2;
            r2.setStateInformation (fresh.getData(), (int) fresh.getSize());
            check (r2.getSamples().getInfo (16).status == tb::SampleManager::Status::empty && ! r2.getBridge().seq.laneHasSteps (16),
                   "a set without scene B opens with scene B empty");
        }

        // SPREAD stays inside its scene.
        {
            Rig r;
            r.load (14, writeAudio ("scene_spread", makeTone (48000, 1), 48000.0, Fmt::wavFloat));
            r.p.getSamples().setSliceCount (14, 8);
            r.p.getSamples().equalSlice (14);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
            const int n = r.p.spreadSlices (14);
            waitForLoads (r.p);
            check (n == 2 && r.p.getSamples().getInfo (16).buffer == nullptr, "SPREAD from pad 15 fills 15 and 16 only (it never spills into scene B)");
        }

        // The controller's lights show the scene on view.
        check (x.r.p.getLights().hardwarePadFor (16) == x.r.p.getLights().hardwarePadFor (0),
               "the iRig's pads stand for the same positions in both scenes");
    }

    void testSceneView()
    {
        std::cout << "\n== Phase 13: VIEW A / B + crossfader on screen ==" << std::endl;
        Rig r;
        r.load (17, writeAudio ("scene_view_b", makeTone (12000, 1), 48000.0, Fmt::wavFloat));
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::SceneStrip* strip = nullptr; tb::ui::PadGrid* grid = nullptr; tb::ui::SequencerPanel* seqPanel = nullptr;
        std::vector<tb::ui::PadComponent*> tiles;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* s = dynamic_cast<tb::ui::SceneStrip*> (&c)) strip = s;
            if (auto* g = dynamic_cast<tb::ui::PadGrid*> (&c)) grid = g;
            if (auto* sp = dynamic_cast<tb::ui::SequencerPanel*> (&c)) seqPanel = sp;
            if (auto* t = dynamic_cast<tb::ui::PadComponent*> (&c)) tiles.push_back (t);
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (strip && grid && seqPanel && tiles.size() == 16, "one 4 x 4 matrix (16 tiles) and the scene strip under it");
        if (! (strip && grid && seqPanel)) return;

        grid->setSelectedPad (2);
        strip->viewButton (1).triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        auto raw = [&] (const juce::String& id) { return r.p.getParameters().getRawParameterValue (id)->load(); };
        check (juce::roundToInt (raw (tb::params::sceneView)) == 1 && grid->getScene() == 1 && tiles[0]->getPadIndex() == 16 && grid->getSelectedPad() == 18,
               "VIEW B: the tiles become B01-B16, and pad 03's place (B03) stays selected");
        check (seqPanel->getSelectedPad() == 18, "...the sequencer follows to scene B's lanes");

        // Automation / a MIDI-mapped control switches the view too.
        setParam (r.p, tb::params::sceneView, 0.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (grid->getScene() == 0 && grid->getSelectedPad() == 2, "the Scene View parameter switches it back (map it to a controller button)");

        // Crossfader drag.
        auto& f = strip->fader();
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&] (juce::Component* comp, juce::Point<float> pos, bool down)
        {
            const auto t = juce::Time::getCurrentTime();
            return juce::MouseEvent (src, pos, down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(), 1.0f, 0, 0, 0, 0, comp, comp, t, pos, t, 1, false);
        };
        const auto s = f.slot();
        const juce::Point<float> start { s.getCentreX(), s.getCentreY() }, end { s.getX() + s.getWidth() * 0.9f, s.getCentreY() };
        f.mouseDown (ev (&f, start, true));
        f.mouseDrag (ev (&f, end, true));
        f.mouseUp (ev (&f, end, false));
        check (std::abs (raw (tb::params::xfade) - 0.9f) < 0.02f, "dragging the crossfader moves the Crossfader parameter (" + juce::String (raw (tb::params::xfade), 2) + ")");
        f.mouseDoubleClick (ev (&f, end, false));
        check (std::abs (raw (tb::params::xfade) - 0.5f) < 0.002f, "double-click: back to the middle");

        auto& cut = strip->cutButton (1);
        const auto cp = cut.getLocalBounds().getCentre().toFloat();
        cut.mouseDown (ev (&cut, cp, true));
        const bool held = r.p.getEngine().getCrossfaderCut() == 1;
        cut.mouseUp (ev (&cut, cp, false));
        check (held && r.p.getEngine().getCrossfaderCut() == -1, "CUT B: only scene B while held");
    }

    //==========================================================================
    //  PHASE 12: vintage CHARACTER + RESAMPLE
    //==========================================================================
    void testCharacter()
    {
        std::cout << "\n== Phase 12: vintage CHARACTER ==" << std::endl;
        using V = tb::PadVoice;
        check (TwistedBreaksProcessor().getParameters().processor.getParameters().size() == 1065, "16 CHARACTER + 6 RESAMPLE parameters (969 in total with scene B)");

        // The read path on its own: a 48 kHz ramp.
        std::vector<float> ramp (4800);
        for (int i = 0; i < 4800; ++i) ramp[(size_t) i] = -0.9f + 1.8f * (float) i / 4799.0f;
        const float* d = ramp.data();
        bool clean = true;
        for (double pos = 10.0; pos < 4700.0; pos += 7.37) clean &= std::abs (V::readCharacter (V::character (0), d, 4800, pos, 48000.0) - V::hermite (d, 4800, pos)) < 1.0e-7f;
        check (clean, "CLEAN reads exactly as before (4-point Hermite)");

        // A steep sawtooth too, so 8-bit steps don't hide the 11 kHz ones.
        std::vector<float> saw (4800);
        for (int i = 0; i < 4800; ++i) saw[(size_t) i] = -0.9f + 1.8f * (float) (i % 480) / 479.0f;
        auto stats = [&] (int c, double inc, const float* d = nullptr)
        {
            if (d == nullptr) d = ramp.data();
            // Output-rate run lengths of held values, and the bit grid.
            int runs = 0, samples = 0; bool onGrid = true; float last = 99.0f;
            const float q = (float) (1 << (V::character (c).bits - 1));
            for (double pos = 100.0; pos < 4000.0; pos += inc)
            {
                const float v = V::readCharacter (V::character (c), d, 4800, pos, 48000.0);
                onGrid &= std::abs (v * q - std::round (v * q)) < 1.0e-3f;
                if (v != last) ++runs;
                last = v; ++samples;
            }
            return std::make_pair ((double) samples / runs, onGrid);
        };
        const auto sp  = stats (1, 1.0), spDown = stats (1, 0.5), lofi = stats (4, 1.0, saw.data());
        check (sp.second && std::abs (sp.first - 48000.0 / 26040.0) < 0.05,
               "SP-1200: 12-bit values, each held for one 26.04 kHz sample (" + juce::String (sp.first, 3) + " output samples)");
        check (std::abs (spDown.first / sp.first - 2.0) < 0.05, "...an octave down, each step is held twice as long: the gritty pitched-down sound");
        check (lofi.second && std::abs (lofi.first - 48000.0 / 11025.0) < 0.1, "LO-FI: 8-bit, 11 kHz steps (" + juce::String (lofi.first, 2) + ")");

        // Through a pad: noise, CLEAN vs MPC60 vs S950 (their output filters).
        juce::AudioBuffer<float> noise (1, 48000);
        juce::Random rnd (9);
        for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample (0, i, rnd.nextFloat() * 0.5f - 0.25f);
        Rig r;
        r.load (0, writeAudio ("char_noise", noise, 48000.0, Fmt::wavFloat));
        setParam (r.p, tb::params::masterLimiter, 0.0f);
        setPadParam (r.p, 0, tb::params::padFixVel, 1.0f);
        r.settle();
        auto hf = [&] (int character)
        {
            setPadParam (r.p, 0, tb::params::padCharacter, (float) character);
            r.settle();
            auto out = renderTimeline (r, 12000, { { 0, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127) } });
            double dd = 0.0, e = 0.0;
            for (int i = 1001; i < 11000; ++i) { const float a = out.getSample (0, i), b = out.getSample (0, i - 1); dd += (a - b) * (a - b); e += a * a; }
            r.p.requestPanic(); r.settle();
            return std::make_pair (dd / e, out);
        };
        const auto c0 = hf (0), mpc = hf (2), s950 = hf (3), sp12 = hf (1);
        check (c0.first > mpc.first * 1.2 && mpc.first > s950.first * 1.5,
               "MPC60's soft low-pass and S950's darker one (HF ratio clean " + juce::String (c0.first, 3) + ", MPC60 " + juce::String (mpc.first, 3)
                 + ", S950 " + juce::String (s950.first, 3) + ")");
        bool grid = true;
        for (int i = 2000; i < 10000; ++i) { const float v = sp12.second.getSample (0, i) * 2048.0f; grid &= std::abs (v - std::round (v)) < 2.0e-3f; }
        check (grid, "a pad set to SP-1200 really plays 12-bit sample values (full level, centre pan)");
    }

    void testResample()
    {
        std::cout << "\n== Phase 12: RESAMPLE ==" << std::endl;
        SeqRig x;
        auto& rs = x.r.p.getResampler();
        auto& eng = x.r.p.getEngine();
        const auto folder = testDir().getChildFile ("resamples");
        folder.deleteRecursively();
        rs.setFolder (folder);
        setParam (x.r.p, tb::params::masterLimiter, 0.0f);
        setPadParam (x.r.p, 1, tb::params::padPan, 0.0f);
        x.seq.clearAll();
        for (int k : { 0, 4, 6, 10, 12 }) x.step (0, k, 127);
        for (int k : { 2, 8, 14 }) x.step (1, k, 90);

        auto waitSaved = [&] (int count)
        {
            for (int i = 0; i < 500 && rs.getCount() < count; ++i) { rs.poll(); juce::MessageManager::getInstance()->runDispatchLoopUntil (10); }
            waitForLoads (x.r.p);
            x.r.settle();
            for (int i = 0; i < 300; ++i) { rs.poll(); juce::MessageManager::getInstance()->runDispatchLoopUntil (5); if (! x.r.p.getSamples().isBusy()) break; }
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            rs.poll();
            waitForLoads (x.r.p);
        };

        // NEXT BAR, 1 bar, the master, onto pad 4. Live starts half-way through bar 1.
        setParam (x.r.p, tb::params::rsmpBars, 0.0f);
        setParam (x.r.p, tb::params::rsmpStart, 1.0f);
        rs.setSelectedPad (3);
        rs.record();
        check (rs.getState() == tb::Resampler::State::armed, "REC arms the recorder");
        x.r.host.ppq = 2.0; x.r.host.playing = true;
        auto out = renderTimeline (x.r, 96000 + 48000 + 2048, {});
        x.r.host.playing = false;
        rs.poll();
        const bool done = eng.getResampleState() == tb::AudioEngine::rsDone || rs.getState() == tb::Resampler::State::saving;
        bool exact = eng.getResampleLength() == 96000;
        const auto& take = eng.getResampleTake();
        for (int i = 0; i < 96000 && exact; i += 7) exact &= take.getSample (0, i) == out.getSample (0, 48000 + i) && take.getSample (1, i) == out.getSample (1, 48000 + i);
        check (done && exact, "it records exactly one bar of what you hear, from the bar-line sample (48000) on");
        waitSaved (1);
        const auto f = rs.getLastFile();
        std::unique_ptr<juce::AudioFormatReader> rd;
        { juce::AudioFormatManager fm; fm.registerBasicFormats(); rd.reset (fm.createReaderFor (f)); }
        check (f.existsAsFile() && f.isAChildOf (folder) && rd != nullptr && rd->lengthInSamples == 96000 && rd->bitsPerSample == 24 && rd->numChannels == 2,
               "...saves it as a 24-bit stereo WAV in the resample folder (" + f.getFileName() + ")");
        const auto info = x.r.p.getSamples().getInfo (3);
        auto raw = [&] (int pad, const char* id) { return x.r.p.getParameters().getRawParameterValue (tb::params::padId (pad, id))->load(); };
        check (info.buffer != nullptr && info.file == f && juce::roundToInt (raw (3, tb::params::padMode)) == tb::params::modeLoop && raw (3, tb::params::padSync) > 0.5f,
               "...and puts it on pad 4 as a tempo-synced loop");
        check (info.slices.count >= 4, "...AUTO SLICEd at its hits (" + juce::String (info.slices.count) + " slices)");
        rd.reset();

        // Pad 4 plays it back in time: resample == what the pattern played.
        x.seq.clearAll();
        x.r.p.requestPanic(); x.r.settle();

        // A group only, starting NOW with Live stopped.
        x.seq.clearAll();
        setPadParam (x.r.p, 1, tb::params::padGroup, 1.0f);           // pad 2 on B
        x.r.settle();
        setParam (x.r.p, tb::params::rsmpSource, 2.0f);                // Group B
        setParam (x.r.p, tb::params::rsmpStart, 0.0f);
        rs.setSelectedPad (5);
        rs.record();
        auto out2 = renderTimeline (x.r, 96000 + 4096, { { 100, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127) },        // pad 1 (A)
                                                         { 30000, juce::MidiMessage::noteOn (1, 37, (juce::uint8) 127) } });   // pad 2 (B)
        rs.poll();
        const auto& t2 = eng.getResampleTake();
        const auto o2 = onsets (t2, 0);
        check (eng.getResampleLength() == 96000 && o2 == std::vector<int> { 30000 },
               "GROUP B with Live stopped: starts at once, 1 bar at 120 BPM, and holds only group B's hit (" + listOf (o2) + ")");
        waitSaved (2);
        check (x.r.p.getSamples().getInfo (5).buffer != nullptr, "...onto pad 6");
        setParam (x.r.p, tb::params::rsmpSource, 0.0f);

        // CANCEL while armed, and the Resample parameter.
        setParam (x.r.p, tb::params::rsmpStart, 1.0f);
        rs.record();
        rs.cancel();
        x.r.settle();
        check (rs.getState() == tb::Resampler::State::idle && rs.getCount() == 2, "CANCEL drops an armed take");
        setParam (x.r.p, tb::params::rsmpRec, 1.0f);
        rs.poll();
        check (rs.getState() == tb::Resampler::State::armed, "the Resample parameter arms it too (MIDI-map it to a pad)");
        setParam (x.r.p, tb::params::rsmpRec, 0.0f);
        rs.cancel(); x.r.settle(); rs.poll();

        // The set reloads it like any sample.
        juce::MemoryBlock blob;
        x.r.p.getStateInformation (blob);
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        waitForLoads (q);
        check (q.getSamples().getInfo (3).file == f && q.getSamples().getInfo (3).buffer != nullptr, "a saved set reloads the resample from its file");
    }

    void testResampleView()
    {
        std::cout << "\n== Phase 12: RESAMPLE view ==" << std::endl;
        Rig r;
        r.p.getResampler().setFolder (testDir().getChildFile ("resamples_ui"));
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::MainView* view = nullptr; tb::ui::ResamplePanel* panel = nullptr; juce::Button* key = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            if (auto* rp = dynamic_cast<tb::ui::ResamplePanel*> (&c)) panel = rp;
            if (auto* sk = dynamic_cast<tb::ui::SelectorKey*> (&c)) if (sk->getName() == "RESAMPLE") key = sk;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        check (view && panel && key && key->isEnabled(), "a 7th top-rail key, RESAMPLE, is online");
        if (! (view && panel && key)) return;
        key->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (panel->isVisible() && view->getView() == tb::ui::MainView::View::resample, "it opens the RESAMPLE view in the screen area");

        panel->characterButton (3).triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        auto ch = [&] (int pad) { return juce::roundToInt (r.p.getParameters().getRawParameterValue (tb::params::padId (pad, tb::params::padCharacter))->load()); };
        check (ch (0) == 3 && ch (1) == 0, "a CHARACTER key sets the selected pad's mode (S950)");
        panel->allPadsButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        bool all = true;
        for (int p = 0; p < 16; ++p) all &= ch (p) == 3;
        check (all, "ALL PADS gives every pad that CHARACTER");

        panel->recButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (r.p.getResampler().getState() == tb::Resampler::State::armed, "REC arms the recorder");
        panel->cancelButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        r.settle();
        check (r.p.getResampler().getState() == tb::Resampler::State::idle, "CANCEL stops it");
    }

    //==========================================================================
    //  PHASE 11: performance macros
    //==========================================================================
    void testMacros()
    {
        std::cout << "\n== Phase 11: performance macros ==" << std::endl;
        namespace mc = tb::macro;
        check (TwistedBreaksProcessor().getParameters().processor.getParameters().size() == 1065, "11 macro parameters (969 in total with the later phases)");

        Rig r;
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        auto blocks = [&] (int n) { for (int i = 0; i < n; ++i) runBlock (r.p, r.host, buf, midi); };
        auto& br = r.p.getBridge();
        auto& eng = r.p.getEngine();
        auto live = [&] (int g) { return br.fxEngaged[(size_t) g].load() | br.fxArmed[(size_t) g].load(); };

        bool allEngage = true, allRelease = true, allValues = true;
        juce::String bad;
        for (int m = 0; m < mc::kNumMacros; ++m)
        {
            const auto& d = mc::def (m);
            setParam (r.p, mc::paramId (m), 1.0f);
            blocks (2);
            bool ok = true;
            for (int g = 0; g < 4; ++g) ok &= live (g) == d.fxMask();
            for (int i = 0; i < d.numSettings; ++i)
                allValues &= std::abs (eng.getAppliedFxParam (0, d.settings[i].fx, d.settings[i].param) - mc::valueAt (d.settings[i], 0.6f)) < 1.0e-6f;
            allEngage &= ok;
            if (! ok) bad << d.key << " ";
            setParam (r.p, mc::paramId (m), 0.0f);
            blocks (40);
            for (int g = 0; g < 4; ++g) allRelease &= (br.fxEngaged[(size_t) g].load() & ~((1u << tb::fx::delay) | (1u << tb::fx::reverb))) == 0;
            r.p.requestPanic(); blocks (4);
        }
        check (allEngage, "each of the 8 macros switches on exactly its recipe of effects in every group (GROUP ALL) " + bad);
        check (allValues, "...with the recipe's settings at MACRO AMOUNT 60 %");
        check (allRelease, "...and letting go switches them off again (DELAY / REVERB tails ring out)");

        // AMOUNT: mild .. hard.
        const auto& destroy = mc::def (0);
        setParam (r.p, tb::params::macroAmount, 0.0f);
        setParam (r.p, mc::paramId (0), 1.0f);
        blocks (1);
        const float bitsMild = eng.getAppliedFxParam (0, tb::fx::crush, 0);
        setParam (r.p, tb::params::macroAmount, 100.0f);
        blocks (1);
        const float bitsHard = eng.getAppliedFxParam (0, tb::fx::crush, 0);
        check (std::abs (bitsMild - destroy.settings[3].mild) < 1.0e-6f && std::abs (bitsHard - destroy.settings[3].hard) < 1.0e-6f,
               "MACRO AMOUNT moves the recipe from MILD to HARD while it's held (DESTROY bits " + juce::String (bitsMild, 2) + " -> " + juce::String (bitsHard, 2) + ")");
        setParam (r.p, mc::paramId (0), 0.0f);
        setParam (r.p, tb::params::macroAmount, 60.0f);
        r.p.requestPanic(); blocks (40);

        // GROUP: just one group.
        setParam (r.p, tb::params::macroGroup, 1.0f);
        setParam (r.p, mc::paramId (3), 1.0f);              // DARKEN on B
        blocks (2);
        check (live (1) == mc::def (3).fxMask() && live (0) == 0 && live (2) == 0 && live (3) == 0, "MACRO GROUP B: only group B gets it");
        setParam (r.p, mc::paramId (3), 0.0f);
        setParam (r.p, tb::params::macroGroup, 4.0f);
        r.p.requestPanic(); blocks (40);

        // Your own settings come back.
        setParam (r.p, tb::fx::paramId (0, tb::fx::lpf), 1.0f);
        setParam (r.p, tb::fx::paramId (0, tb::fx::lpf, 0), 0.8f);
        blocks (2);
        setParam (r.p, mc::paramId (3), 1.0f);
        blocks (2);
        const float during = eng.getAppliedFxParam (0, tb::fx::lpf, 0);
        setParam (r.p, mc::paramId (3), 0.0f);
        blocks (2);
        const float after = eng.getAppliedFxParam (0, tb::fx::lpf, 0);
        check (std::abs (during - mc::valueAt (mc::def (3).settings[0], 0.6f)) < 1.0e-6f && std::abs (after - 0.8f) < 1.0e-4f
                 && (br.fxEngaged[0].load() & (1u << tb::fx::lpf)) != 0,
               "a macro borrows an effect you had on (your LPF at 0.8 -> DARKEN's " + juce::String (during, 3) + "), and gives it back exactly as it was");
        setParam (r.p, tb::fx::paramId (0, tb::fx::lpf), 0.0f);
        r.p.requestPanic(); blocks (40);

        // MIDI notes E4..B4: hold, or toggle with LATCH.
        midi.addEvent (juce::MidiMessage::noteOn (1, mc::kFirstMidiNote + 6, (juce::uint8) 100), 0);   // JUNGLE ROLL
        runBlock (r.p, r.host, buf, midi);
        blocks (1);
        const bool heldOn = (br.macroActive.load() & (1u << 6)) != 0 && live (0) == mc::def (6).fxMask();
        midi.addEvent (juce::MidiMessage::noteOff (1, mc::kFirstMidiNote + 6), 0);
        runBlock (r.p, r.host, buf, midi);
        check (heldOn && br.macroActive.load() == 0, "MIDI note A#4 holds JUNGLE ROLL while it's down");
        setParam (r.p, tb::params::macroLatch, 1.0f);
        blocks (1);
        midi.addEvent (juce::MidiMessage::noteOn (1, mc::kFirstMidiNote, (juce::uint8) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOff (1, mc::kFirstMidiNote), 10);
        runBlock (r.p, r.host, buf, midi);
        const bool latched = (br.macroActive.load() & 1u) != 0;
        midi.addEvent (juce::MidiMessage::noteOn (1, mc::kFirstMidiNote, (juce::uint8) 100), 0);
        runBlock (r.p, r.host, buf, midi);
        check (latched && br.macroActive.load() == 0, "with LATCH, E4 toggles DESTROY on and off");
        setParam (r.p, tb::params::macroLatch, 0.0f);
        r.p.requestPanic(); blocks (40);

        // Audible: DARKEN on noise, MILD vs HARD.
        {
            juce::AudioBuffer<float> noise (1, 48000);
            juce::Random rnd (5);
            for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample (0, i, rnd.nextFloat() * 0.5f - 0.25f);
            Rig n;
            n.load (0, writeAudio ("macro_noise", noise, 48000.0, Fmt::wavFloat));
            setPadParam (n.p, 0, tb::params::padMode, (float) tb::params::modeLoop);
            setPadParam (n.p, 0, tb::params::padSync, 0.0f);
            setParam (n.p, tb::params::masterLimiter, 0.0f);
            n.settle();
            auto brightness = [&] (float onValue, float amount)
            {
                setParam (n.p, tb::params::macroAmount, amount);
                setParam (n.p, mc::paramId (3), onValue);
                auto out = renderTimeline (n, 30000, { { 0, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127) } });
                double d = 0.0, e = 0.0;
                for (int i = 20001; i < 30000; ++i) { const float a = out.getSample (0, i), b = out.getSample (0, i - 1); d += (a - b) * (a - b); e += a * a; }
                renderTimeline (n, 512, { { 0, juce::MidiMessage::noteOff (1, 36) } });
                setParam (n.p, mc::paramId (3), 0.0f);
                n.p.requestPanic(); n.settle(); n.settle(); n.settle();
                return e > 0.0 ? d / e : 0.0;
            };
            const double dry = brightness (0.0f, 60.0f), mild = brightness (1.0f, 0.0f), hard = brightness (1.0f, 100.0f);
            check (dry > mild * 2.0 && mild > hard * 1.5,
                   "DARKEN audibly darkens noise, more at HARD (HF ratio dry " + juce::String (dry, 3) + ", mild " + juce::String (mild, 3) + ", hard " + juce::String (hard, 3) + ")");
        }

        check (tb::SnapshotManager::isExcluded (mc::paramId (0)) && ! tb::SnapshotManager::isExcluded (tb::params::macroAmount),
               "snapshots leave the macro keys alone (they're moves), but keep AMOUNT / GROUP / LATCH");
    }

    void testMacroView()
    {
        std::cout << "\n== Phase 11: MACRO page ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::FXGrid* grid = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* g = dynamic_cast<tb::ui::FXGrid*> (&c)) grid = g;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        if (grid == nullptr) { check (false, "found the FX grid"); return; }
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto mouse = [&] (juce::Point<float> pos, bool down)
        {
            const auto t = juce::Time::getCurrentTime();
            juce::MouseEvent e (src, pos, down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(), 1.0f, 0, 0, 0, 0, grid, grid, t, pos, t, 1, false);
            if (down) grid->mouseDown (e); else grid->mouseUp (e);
        };
        auto raw = [&] (const juce::String& id) { return r.p.getParameters().getRawParameterValue (id)->load(); };

        mouse (grid->macroTabBounds().getCentre(), true); mouse (grid->macroTabBounds().getCentre(), false);
        check (grid->isMacroPage() && grid->latchButton().isVisible(), "the MACRO tab shows the macro keys, AMOUNT, GROUP and LATCH");

        const auto k = grid->macroKeyBounds (4).getCentre();        // TAPE STOP
        mouse (k, true);
        const bool down = raw (tb::macro::paramId (4)) > 0.5f;
        mouse (k, false);
        check (down && raw (tb::macro::paramId (4)) < 0.5f, "a macro key holds its macro while pressed");

        grid->latchButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        mouse (k, true); mouse (k, false);
        r.settle();
        const bool latchedOn = raw (tb::macro::paramId (4)) > 0.5f;
        mouse (k, true); mouse (k, false);
        r.settle();
        check (latchedOn && raw (tb::macro::paramId (4)) < 0.5f, "with LATCH a click toggles it on, another off");

        mouse (grid->groupBounds (2).getCentre(), true); mouse (grid->groupBounds (2).getCentre(), false);
        check (! grid->isMacroPage() && grid->getGroup() == 2, "a group tab goes back to the 16 FX keys of that group");
    }

    //==========================================================================
    //  PHASE 10: MUTATE
    //==========================================================================
    void testMutateRules()
    {
        std::cout << "\n== Phase 10: MUTATE rules ==" << std::endl;
        using M = tb::Mutator;
        const uint32_t all = 0x7fu;

        // A jungle-ish 16-step lane: kick / snare on beats, ghosts in between.
        M::Lane orig {};
        auto put = [&orig] (int k, int vel, int micro = 0) { orig[(size_t) k].on = true; orig[(size_t) k].velocity = vel; orig[(size_t) k].micro = micro; };
        put (0, 120); put (4, 115); put (6, 60, 9); put (8, 120); put (10, 70); put (12, 115); put (15, 50, -5);

        juce::Random r (1);
        M::Lane out;
        M::mutateLane (out, orig, 16, 4, 16, 0.0f, all, r);
        bool same = true;
        for (int k = 0; k < 32; ++k) same &= out[(size_t) k] == orig[(size_t) k];
        check (same, "AMOUNT 0 %: the lane comes back exactly as written");

        bool anchorsKept = true, onGrid = true, ghostsNear = true, ghostsQuiet = true, noDownbeatRoll = true, fxLastBeat = true,
             outsideUntouched = true, inRange = true;
        int changed = 0, ghosts = 0, drops = 0, rolls = 0, pitched = 0, fxThrows = 0, chances = 0, sliced = 0;
        const float avg = (120 + 115 + 60 + 120 + 70 + 115 + 50) / 7.0f;
        for (int seed = 0; seed < 300; ++seed)
        {
            juce::Random rr (seed);
            M::mutateLane (out, orig, 16, 4, 12, 1.0f, all, rr);
            bool diff = false;
            for (int k = 0; k < 32; ++k)
            {
                const auto& o = orig[(size_t) k];
                const auto& m = out[(size_t) k];
                diff = diff || ! (o == m);
                if (k >= 16) { outsideUntouched &= o == m; continue; }
                if (k % 4 == 0 && o.on)
                    anchorsKept &= m.on && m.velocity == o.velocity && m.micro == o.micro && m.slice == 0 && m.pitch == 0 && ! m.rev
                                   && m.probability == 100 && m.fx == 0;
                if (m.on && o.on)  onGrid &= m.micro == o.micro;
                if (m.on && ! o.on)
                {
                    ++ghosts;
                    onGrid &= m.micro == 0;
                    ghostsNear &= orig[(size_t) ((k + 15) % 16)].on || orig[(size_t) ((k + 1) % 16)].on;
                    ghostsQuiet &= (float) m.velocity < avg;
                }
                if (o.on && ! m.on) ++drops;
                if (m.on && m.ratchet > 1) { ++rolls; noDownbeatRoll &= k % 16 != 0; }
                if (m.on && m.pitch != 0) ++pitched;
                if (m.on && m.slice > 0) { ++sliced; inRange &= m.slice <= 12; }
                if (m.on && m.fx > 0) { ++fxThrows; fxLastBeat &= k % 16 >= 12; }
                if (m.on && m.probability < 100) { ++chances; inRange &= m.probability >= 50 && m.probability <= 90; }
            }
            changed += diff ? 1 : 0;
        }
        check (anchorsKept, "300 mutations at 100 %: every hit on the beat stays exactly as written (no drop, re-slice, pitch, reverse, chance, FX)");
        check (onGrid && ghostsNear && ghostsQuiet,
               "nothing moves off the grid; new hits (" + juce::String (ghosts) + ") are only quieter ghosts right next to existing hits");
        check (noDownbeatRoll && fxLastBeat && inRange && outsideUntouched,
               "rolls never on the bar's downbeat, FX throws only in the last beat, slices / chance in range, steps past LEN untouched");
        check (changed >= 295 && drops > 0 && rolls > 0 && pitched > 0 && fxThrows > 0 && chances > 0 && sliced > 0,
               "...and it really varies: " + juce::String (changed) + " / 300 differ (" + juce::String (drops) + " drops, " + juce::String (rolls)
                 + " rolls, " + juce::String (pitched) + " pitched, " + juce::String (sliced) + " re-sliced, " + juce::String (fxThrows) + " FX, "
                 + juce::String (chances) + " chance)");

        // Targets: only what's switched on changes.
        bool onlyRatchet = true;
        for (int seed = 0; seed < 100; ++seed)
        {
            juce::Random rr (seed);
            M::mutateLane (out, orig, 16, 4, 12, 1.0f, 1u << tb::params::mutTRatchet, rr);
            for (int k = 0; k < 32; ++k)
            {
                auto a = out[(size_t) k]; a.ratchet = orig[(size_t) k].ratchet;
                onlyRatchet &= a == orig[(size_t) k];
            }
        }
        check (onlyRatchet, "with only RAT switched on, nothing but ratchets changes");

        M::Lane empty {};
        M::mutateLane (out, empty, 16, 4, 16, 1.0f, all, r);
        bool stillEmpty = true;
        for (auto& st : out) stillEmpty &= ! st.on;
        check (stillEmpty, "an empty lane stays empty (MUTATE never invents a part)");

        // Slice sequence: beats stay, off-beats trade slices.
        M::Slots so {};
        for (int k = 0; k < 16; ++k) so[(size_t) k] = k % 2 == 0 ? tb::SliceStep::playing (k / 2) : tb::SliceStep {};   // 8 slices on 1/8s, ties between
        bool beatsKept = true, slicesOk = true; int swaps = 0, repeats = 0;
        for (int seed = 0; seed < 200; ++seed)
        {
            juce::Random rr (seed);
            M::Slots sm;
            M::mutateSlices (sm, so, 16, 4, 8, 1.0f, all, rr);
            for (int k = 0; k < 16; ++k)
            {
                const auto& o = so[(size_t) k]; const auto& m = sm[(size_t) k];
                if (k % 4 == 0) beatsKept &= m.kind == o.kind && m.slice == o.slice && m.pitch == 0 && ! m.rev && m.chance == 100;
                if (m.kind == tb::SliceStep::play) slicesOk &= m.slice >= 0 && m.slice < 8;
                if (o.kind == tb::SliceStep::play && m.kind == tb::SliceStep::play && m.slice != o.slice) ++swaps;
                if (o.kind == tb::SliceStep::tie && m.kind == tb::SliceStep::play) ++repeats;
            }
        }
        check (beatsKept && slicesOk && swaps > 0 && repeats > 0,
               "slice sequences: the beats keep their slices; off-beats trade slices (" + juce::String (swaps) + ") and tied steps re-hit (" + juce::String (repeats) + ")");
    }

    void testMutate()
    {
        std::cout << "\n== Phase 10: MUTATE (original, undo, reset, timing, auto) ==" << std::endl;
        check (TwistedBreaksProcessor().getParameters().processor.getParameters().size() == 1065, "13 MUTATE parameters (969 in total with the later phases)");

        SeqRig x;
        auto& mut = x.r.p.getMutator();
        mut.setSeed (7);
        auto lanes = [&x] { std::array<std::array<uint64_t, 32>, 16> a {}; for (int p = 0; p < 16; ++p) for (int k = 0; k < 32; ++k) a[(size_t) p][(size_t) k] = x.seq.getPacked (p, k); return a; };
        x.seq.clearAll();
        for (int k : { 0, 4, 6, 8, 10, 12, 14 }) x.step (0, k, k % 4 == 0 ? 120 : 70);
        for (int k : { 2, 5, 10, 13 }) x.step (1, k, 90);
        const auto original = lanes();
        setParam (x.r.p, tb::params::mutAmount, 100.0f);
        setParam (x.r.p, tb::params::mutAt, 0.0f);                      // NOW

        mut.mutate();
        check (mut.isWaiting(), "MUTATE arms a change for the audio thread");
        x.r.settle();
        mut.poll();
        const auto first = lanes();
        check (! mut.isWaiting() && mut.getCount() == 1 && first != original && mut.hasOriginal() && mut.canUndo(),
               "...which lands on the next block: the lanes changed");
        mut.mutate(); x.r.settle(); mut.poll();
        const auto second = lanes();
        bool beatsSame = true;
        for (int k : { 0, 4, 8, 12 }) beatsSame &= x.seq.get (0, k).on && x.seq.get (0, k).velocity == 120;
        check (second != first && beatsSame, "each MUTATE is a new variation (of the original: the beats are still the written ones)");

        mut.undo(); x.r.settle(); mut.poll();
        check (lanes() == first, "UNDO goes back to the previous variation");
        mut.undo(); x.r.settle(); mut.poll();
        check (lanes() == second, "...and UNDO again redoes");
        mut.reset(); x.r.settle(); mut.poll();
        check (lanes() == original, "RESET brings back the original pattern exactly");

        // A hand edit becomes the new original.
        x.step (2, 3, 100);
        const auto edited = lanes();
        mut.mutate(); x.r.settle(); mut.poll();
        mut.reset(); x.r.settle(); mut.poll();
        check (lanes() == edited, "an edit after mutating becomes the new original (RESET returns to the edited pattern)");

        // DRIFT: builds on the last result. With only PITCH on, a pitched step
        // stays pitched (pitch moves only between non-zero values), so pitched
        // steps can only accumulate; without DRIFT each pass starts clean.
        for (int t = 0; t < tb::params::kNumMutTargets; ++t) setParam (x.r.p, tb::params::mutTargetId (t), t == tb::params::mutTPitch ? 1.0f : 0.0f);
        setParam (x.r.p, tb::params::mutAmount, 60.0f);
        auto pitched = [&x] { uint32_t m = 0; for (int k = 0; k < 16; ++k) if (x.seq.get (0, k).on && x.seq.get (0, k).pitch != 0) m |= 1u << k; return m; };
        auto runPasses = [&] (bool drift)
        {
            setParam (x.r.p, tb::params::mutDrift, drift ? 1.0f : 0.0f);
            bool kept = true, lost = false;
            uint32_t last = 0;
            for (int i = 0; i < 12; ++i)
            {
                mut.mutate(); x.r.settle(); mut.poll();
                const uint32_t now = pitched();
                kept = kept && (now & last) == last;
                lost = lost || (now & last) != last;
                last = now;
            }
            mut.reset(); x.r.settle(); mut.poll();
            return std::make_pair (kept, lost);
        };
        const auto withDrift = runPasses (true);
        const auto noDrift   = runPasses (false);
        check (withDrift.first && noDrift.second && lanes() == edited,
               "DRIFT builds on the last mutation (changes accumulate), off it starts from the original each time; RESET still finds the original");
        for (int t = 0; t < tb::params::kNumMutTargets; ++t) setParam (x.r.p, tb::params::mutTargetId (t), 1.0f);
        setParam (x.r.p, tb::params::mutDrift, 0.0f);
        setParam (x.r.p, tb::params::mutAmount, 100.0f);

        // SCOPE: selected pad only.
        setParam (x.r.p, tb::params::mutScope, 0.0f);
        mut.setSelectedPad (1);
        mut.mutate(); x.r.settle(); mut.poll();
        auto l = lanes();
        check (l[0] == edited[0] && l[2] == edited[2] && l[1] != edited[1], "SCOPE PAD: only the selected pad's lane changes");
        mut.reset(); x.r.settle(); mut.poll();
        setParam (x.r.p, tb::params::mutScope, 1.0f);

        // The "Mutate" parameter (MIDI-mappable): rising edge.
        const int before = mut.getCount();
        setParam (x.r.p, tb::params::mutGo, 1.0f);
        mut.poll(); x.r.settle(); mut.poll();
        mut.poll(); x.r.settle(); mut.poll();
        setParam (x.r.p, tb::params::mutGo, 0.0f);
        mut.poll(); x.r.settle(); mut.poll();
        check (mut.getCount() == before + 1, "the Mutate parameter mutates once per press (MIDI-map it to a pad)");
        mut.reset(); x.r.settle(); mut.poll();

        // Exact timing: a frame armed for the next bar lands on the bar-line sample, mid-block.
        {
            x.seq.clearAll();
            for (int k = 0; k < 16; ++k) x.step (0, k, 127);       // pad 1 (left) every 1/16
            auto* f = new tb::MutationFrame();
            f->quant = tb::params::switchBar;
            f->laneMask = 0x3u;
            for (int k = 0; k < 16; ++k) { tb::SeqStep st; st.on = true; st.velocity = 127; f->seq[1][(size_t) k] = st.pack(); }   // -> pad 2 (right)
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            auto first4 = renderTimeline (x.r, 40000, {});             // into bar 1
            mut.adoptForDeletion (x.r.p.armMutationFrame (f));
            auto rest = renderTimeline (x.r, 80000, {});
            x.r.host.playing = false; x.r.p.requestPanic(); x.r.settle(); mut.poll();
            const auto left = onsets (rest, 0), right = onsets (rest, 1);
            const int bar = 96000 - (40000 / kBlock + (40000 % kBlock ? 1 : 0)) * kBlock;   // bar line inside `rest`
            check (! left.empty() && left.back() < bar && ! right.empty() && right.front() == bar && bar % kBlock != 0,
                   "a mutation armed for NEXT BAR swaps the pattern on the bar-line sample, mid-block (last old hit "
                     + juce::String (left.empty() ? -1 : left.back()) + ", first new " + juce::String (right.empty() ? -1 : right.front())
                     + ", bar " + juce::String (bar) + ")");
        }

        // AUTO every 2 bars: lands on bars 3 and 5 (song start = bar 1), nowhere else.
        {
            x.seq.clearAll();
            for (int k : { 0, 4, 6, 8, 10, 12, 14 }) x.step (0, k, 100);
            setParam (x.r.p, tb::params::mutAuto, 2.0f);              // 2 bars
            x.r.host.ppq = 0.0; x.r.host.playing = true;
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            std::vector<int> changedAt;
            auto v = x.seq.getVersion();
            const int c0 = mut.getCount();
            for (int b = 0; b < (96000 * 5 + 48000) / kBlock; ++b)
            {
                runBlock (x.r.p, x.r.host, buf, midi);
                if (x.seq.getVersion() != v) { changedAt.push_back (b); v = x.seq.getVersion(); }
                if (b % 3 == 0) mut.poll();                           // the message thread's ~30 Hz
            }
            mut.poll();
            x.r.host.playing = false;
            setParam (x.r.p, tb::params::mutAuto, 0.0f);
            x.r.p.requestPanic(); x.r.settle(); mut.poll();
            const std::vector<int> expect { 192000 / kBlock, 384000 / kBlock };
            juce::StringArray got; for (int b : changedAt) got.add (juce::String (b * kBlock));
            check (changedAt == expect && mut.getCount() == c0 + 2,
                   "AUTO 2 BARS mutates on the bar lines of bars 3 and 5, nowhere else (blocks at " + got.joinIntoString (" ") + ")");
            mut.reset(); x.r.settle(); mut.poll();
        }

        // Slice sequences mutate too (their current slot).
        {
            SliceRig y;
            auto& m2 = y.r.p.getMutator();
            m2.setSeed (3);
            setParam (y.r.p, tb::params::mutAmount, 100.0f);
            setParam (y.r.p, tb::params::mutAt, 0.0f);
            m2.mutate(); y.r.settle(); m2.poll();
            const auto slot = y.data.getSlot (0, 0);
            std::array<tb::SliceStep, 32> pat;
            std::array<tb::SliceStep, 32> def;
            const double beats = tb::PadVoice::loopBeatsFor ((double) y.brk.getNumSamples() / 48000.0);
            const auto info = y.r.p.getSamples().getInfo (0);
            tb::SliceSeqData::defaultPattern (info.slices, y.brk.getNumSamples(), 48000.0, 0.25, beats, def);
            bool beats4 = true, differs = false;
            for (int k = 0; k < 32; ++k)
            {
                pat[(size_t) k] = y.data.getStep (0, 0, k);
                if (k % 4 == 0 && def[(size_t) k].kind == tb::SliceStep::play) beats4 &= pat[(size_t) k].slice == def[(size_t) k].slice && pat[(size_t) k].kind == tb::SliceStep::play;
                differs = differs || ! (pat[(size_t) k] == def[(size_t) k]);
            }
            check (slot.written && differs && beats4, "a SLICE SEQ pad's slot is mutated too: the break's beats stay, the rest is rearranged");
            m2.reset(); y.r.settle(); m2.poll();
            bool back = true;
            for (int k = 0; k < 32; ++k) back &= y.data.getStep (0, 0, k) == def[(size_t) k];
            check (back, "...and RESET gives the break back as recorded");
        }
    }

    void testMutateView()
    {
        std::cout << "\n== Phase 10: MUTATE page ==" << std::endl;
        Rig r;
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        tb::ui::MainView* view = nullptr; tb::ui::SequencerPanel* panel = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* v = dynamic_cast<tb::ui::MainView*> (&c)) view = v;
            if (auto* sp = dynamic_cast<tb::ui::SequencerPanel*> (&c)) panel = sp;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*ed);
        if (! (view && panel)) { check (false, "found the sequencer panel"); return; }
        view->showView (tb::ui::MainView::View::sequencer);
        juce::Button* tab = nullptr;
        for (auto* ch : panel->getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (ch)) if (b->getName() == "MUTATE") tab = b;
        check (tab != nullptr && ! panel->mutateButton().isVisible(), "the MUTATE tab sits under STEP; its page starts hidden");
        if (tab == nullptr) return;
        tab->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (panel->getPage() == tb::ui::SequencerPanel::Page::mutate && panel->mutateButton().isVisible() && ! panel->fillButton().isVisible(),
               "clicking it shows the MUTATE page in place of the STEP row");

        auto* pitchOn = r.p.getParameters().getRawParameterValue (tb::params::mutTargetId (tb::params::mutTPitch));
        const bool was = pitchOn->load() > 0.5f;
        panel->targetButton (tb::params::mutTPitch).triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check ((pitchOn->load() > 0.5f) != was, "target keys switch their Mutate parameters (PITCH)");

        auto& seq = r.p.getBridge().seq;
        tb::SeqStep st; st.on = true;
        for (int k = 0; k < 16; ++k) seq.set (0, k, st);
        setParam (r.p, tb::params::mutAmount, 100.0f);
        setParam (r.p, tb::params::mutAt, 0.0f);
        panel->mutateButton().triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        r.settle();
        r.p.getMutator().poll();
        panel->refresh();
        check (r.p.getMutator().getCount() == 1 && panel->undoButton().isEnabled(), "the GO key mutates; UNDO lights up");
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
        const bool fxView    = out.getFileName().contains ("fx");
        const bool sliceSeqView = out.getFileName().contains ("sseq");
        const bool snapView = out.getFileName().contains ("snap");
        if (snapView)
        {
            for (int i : { 0, 1, 2, 4 }) p.getSnapshots().store (i);
            p.getSnapshots().recall (1, 0);
            p.getBank().copyFromPad (0, 0);
            p.getBank().copyFromPad (1, 2);
            p.getBank().copyFromPad (2, 1);
            setParam (p, tb::params::bankActive, 2.0f);
            setPadParam (p, 0, tb::params::padFollowBank, 1.0f);
        }
        const bool seqView   = out.getFileName().contains ("seq") && ! sliceSeqView;
        if (sliceSeqView)
        {
            p.getSamples().autoSlice (0);
            waitForLoads (p);
            setPadParam (p, 0, tb::params::padSliceSeq, 1.0f);
            setPadParam (p, 0, tb::params::padMode, (float) tb::params::modeOneShot);
            p.materializeSliceSlot (0, 0);
            auto& d = p.getBridge().sliceSeq;
            using SS = tb::SliceStep;
            auto put = [&] (int k, SS st) { d.setStep (0, 0, k, st); };
            put (2, SS::playing (4)); { SS st = SS::playing (2); st.rev = true; put (6, st); }
            { SS st; st.kind = SS::rest; put (11, st); }
            { SS st = SS::playing (7); st.ratchet = 3; put (14, st); }
            { SS st = SS::playing (1); st.pitch = -5; put (20, st); }
            { SS st = SS::playing (9); st.chance = 50; put (27, st); }
        }
        if (seqView)
        {
            auto& seq = p.getBridge().seq;
            auto put = [&] (int pad, const char* pattern, int vel)
            {
                for (int i = 0; pattern[i] != 0 && i < 32; ++i)
                    if (pattern[i] != '.') { tb::SeqStep st; st.on = true; st.velocity = pattern[i] == 'x' ? vel : vel / 2; seq.set (pad, i, st); }
            };
            put (1, "x.........x.....x.x.......x.....", 120);
            put (2, "....x.......x..o....x.......x..o", 115);
            put (3, "x.x.x.oxx.x.x.x.x.x.x.oxx.x.x.ox", 90);
            put (5, "..............x...............x.", 100);
            { tb::SeqStep st; st.on = true; st.velocity = 80; st.micro = -64; seq.set (4, 7, st); }
            // Phase 9 step features.
            auto tweak = [&seq] (int pad, int s, std::function<void (tb::SeqStep&)> f) { auto st = seq.get (pad, s); st.on = true; f (st); seq.set (pad, s, st); };
            tweak (0, 0,  [] (auto& st) { st.velocity = 110; st.ratchet = 2; st.probability = 75; st.pitch = 5; st.cond = tb::cond1of2; });
            tweak (0, 12, [] (auto& st) { st.velocity = 100; st.slice = 7; st.rev = true; });
            tweak (0, 28, [] (auto& st) { st.fx = tb::fx::stutter + 1; st.hit = false; });
            tweak (3, 7,  [] (auto& st) { st.ratchet = 3; });
            tweak (3, 23, [] (auto& st) { st.ratchet = 4; st.cond = tb::condFill; });
            tweak (3, 13, [] (auto& st) { st.probability = 50; });
            tweak (3, 29, [] (auto& st) { st.probability = 35; });
            tweak (2, 31, [] (auto& st) { st.fx = tb::fx::reverse + 1; });
            setParam (p, tb::params::seqLength, 32.0f);
            seq.setArmed (3, true);
            if (out.getFileName().contains ("mut"))
            {
                setParam (p, tb::params::mutAmount, 45.0f);
                setParam (p, tb::params::mutAuto, 3.0f);
                setParam (p, tb::params::mutAt, 2.0f);
                p.getMutator().setSeed (11);
                p.getMutator().mutate (0);
                p.applyMutationNow();
                p.getMutator().poll();
            }
        }
        if (fxView)
        {
            setParam (p, tb::fx::paramId (0, tb::fx::lpf), 1.0f);
            setParam (p, tb::fx::paramId (0, tb::fx::lpf, 0), 0.42f);
            setParam (p, tb::fx::paramId (0, tb::fx::lpf, 1), 0.55f);
            setParam (p, tb::fx::paramId (0, tb::fx::delay), 1.0f);
            setParam (p, tb::fx::paramId (0, tb::fx::tape), 1.0f);
        }
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
        if (snapView)
        {
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* mv = dynamic_cast<tb::ui::MainView*> (&c)) mv->showView (tb::ui::MainView::View::snapshots);
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*ed);
        }
        if (sliceSeqView)
        {
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* pe = dynamic_cast<tb::ui::PadEditorPanel*> (&c)) pe->setPage (tb::ui::PadEditorPanel::Page::seq);
                if (auto* ssp = dynamic_cast<tb::ui::SliceSeqPanel*> (&c)) ssp->selectStep (6);
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*ed);
            host.playing = true;
        }
        if (out.getFileName().contains ("scenes"))
        {
            setParam (p, tb::params::xfade, 0.32f);
        }
        if (out.getFileName().contains ("resample"))
        {
            setPadParam (p, 0, tb::params::padCharacter, 1.0f);       // pad 1 on SP-1200
            setParam (p, tb::params::rsmpBars, 1.0f);
            p.getResampler().setFolder (testDir().getChildFile ("snap_resamples"));
            p.getResampler().setSelectedPad (0);
            p.getResampler().record();
            std::function<void (juce::Component&)> findView = [&] (juce::Component& c)
            {
                if (auto* mv = dynamic_cast<tb::ui::MainView*> (&c)) mv->showView (tb::ui::MainView::View::resample);
                for (auto* child : c.getChildren()) findView (*child);
            };
            findView (*ed);
        }
        if (out.getFileName().contains ("macro"))
        {
            setParam (p, tb::macro::paramId (6), 1.0f);       // JUNGLE ROLL held
            std::function<void (juce::Component&)> findGrid = [&] (juce::Component& c)
            {
                if (auto* g = dynamic_cast<tb::ui::FXGrid*> (&c)) g->showMacros (true);
                for (auto* child : c.getChildren()) findGrid (*child);
            };
            findGrid (*ed);
        }
        if (seqView)
        {
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* mv = dynamic_cast<tb::ui::MainView*> (&c)) mv->showView (tb::ui::MainView::View::sequencer);
                if (auto* sp = dynamic_cast<tb::ui::SequencerPanel*> (&c))
                    if (out.getFileName().contains ("mut")) sp->showPage (tb::ui::SequencerPanel::Page::mutate);
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*ed);
            host.playing = true;
        }
        if (fxView)
        {
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* mv = dynamic_cast<tb::ui::MainView*> (&c)) mv->showView (tb::ui::MainView::View::fx);
                if (auto* grid = dynamic_cast<tb::ui::FXGrid*> (&c)) grid->setSelected (tb::fx::delay);
                for (auto* child : c.getChildren()) find (*child);
            };
            find (*ed);
        }
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
        testFixedVelocity (tone);
        testFxFilters();
        testFxTime();
        testFxSpace();
        testFxUi();
        testFxCpu();
        testSequencer();
        testLiveRecord();
        testSequencerView();
        testStepFeatures();
        testStepEditorView();
        testSliceSequencer();
        testSliceSeqEditing();
        testSliceSeqSwap();
        testSnapshots (tone);
        testBreakBank();
        testFindMissing();
        testSnapshotView();
        testSnapshotStress (tone);
        testMutateRules();
        testMutate();
        testMutateView();
        testMacros();
        testMacroView();
        testCharacter();
        testResample();
        testResampleView();
        testScenes();
        testSceneView();
        testSeqPads();
        testSeqPadView();
        testEditorDrivesState();
        testRealtimeSafety();

        std::cout << "\n" << (checks - failures) << " / " << checks << " checks passed" << std::endl;
        result = failures == 0 ? 0 : 1;
    }

    testDir().deleteRecursively();
    return result;
}
