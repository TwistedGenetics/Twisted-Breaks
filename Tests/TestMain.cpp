/*
    TwistedBreaksTests — headless verification for each development phase.

    Runs the real processor (same sources as the plugin) against a fake host
    playhead and synthetic MIDI, so Phase 1's checklist can be proven without
    opening Ableton:
        * builds and instantiates, stereo out only
        * audio output works; MIDI reaches the processor sample-accurately
        * pads 1..16 map to notes 36..51
        * host BPM / PPQ / transport / bar-beat are read correctly
        * transport start, stop and jump (loop wrap) detection
        * panic, limiter ceiling, state save/restore
    Optionally renders the editor to a PNG:  TwistedBreaksTests --snapshot out.png
*/
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <iostream>

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
        p.processBlock (buf, midi);
        host.advance (buf.getNumSamples(), kSr);
        midi.clear();
    }

    void setParam (TwistedBreaksProcessor& p, const char* id, float plainValue)
    {
        auto* param = p.getParameters().getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

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

        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "silent with no input");
    }

    void testMidiAndAudio()
    {
        std::cout << "\n== MIDI reaches processor / audio output ==" << std::endl;
        TwistedBreaksProcessor p;
        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 120), 100);
        runBlock (p, host, buf, midi);

        auto& br = p.getBridge();
        check (isSilent (peakOf (buf, 0, 100)), "nothing before the note's sample offset (sample-accurate)");
        check (peakOf (buf, 100) > 0.001f, "audio after note 36 (pad 1 fires)");
        check (br.lastMidiNote.load() == 36, "UI bridge sees last MIDI note 36");
        check (br.padTriggerCount[0].load() == 1, "pad 1 trigger count = 1");

        // Every default mapping 36..51 -> pads 1..16
        for (int n = 37; n <= 51; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), n - 37);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 40);   // unmapped
        runBlock (p, host, buf, midi);

        bool allMapped = true;
        for (int pad = 1; pad < 16; ++pad)
            allMapped &= br.padTriggerCount[(size_t) pad].load() == 1;
        check (allMapped, "notes 37..51 trigger pads 2..16");
        check (br.midiEventCount.load() == 17, "17 note-ons counted (incl. unmapped 60)");

        // Mouse path: UI queue -> audio thread
        p.triggerPadFromUi (5, 1.0f, true);
        runBlock (p, host, buf, midi);
        check (br.padTriggerCount[5].load() == 2, "UI pad press reaches the audio thread");

        // Remap
        p.getEngine().setPadNote (0, 48);
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        runBlock (p, host, buf, midi);
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

        host.playing = true;
        p.processBlock (buf, midi);
        // Read the engine's own TransportState via a second sync object to
        // test edge flags directly.
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

        h2.ppq = 0.0;     // Ableton loop brace wraps back to bar 1
        s = sync.update (&h2, kBlock);
        check (s.jumped, "loop wrap / relocate detected as jump");

        h2.playing = false;
        s = sync.update (&h2, kBlock);
        check (s.justStopped, "stop edge detected");

        // Sample-accurate grid lookup: 120 BPM, 48 kHz, PPQ 0.9 -> next 16th at 1.0
        tb::TransportSync sync2; sync2.prepare (kSr);
        FakeHost h3; h3.bpm = 120.0; h3.ppq = 0.9; h3.playing = true;
        const auto s3 = sync2.update (&h3, 4096);
        const auto b  = s3.nextBoundary (0.25);
        check (b.has_value() && *b == 2400, "next 16th boundary found at sample 2400");
        check (! s3.nextBoundary (4.0).has_value(), "no bar boundary inside this block");

        tb::TransportSync sync3; sync3.prepare (kSr);
        FakeHost h4; h4.num = 7; h4.den = 8; h4.ppq = 3.5 + 0.75; h4.playing = true;   // 7/8: bar = 3.5 qn
        const auto s4 = sync3.update (&h4, kBlock);
        check (s4.barIndex == 1 && s4.beatInBar == 1, "7/8 time signature bar/beat");

        host.provide = false;
        runBlock (p, host, buf, midi);
        check (! t.hostProvidesInfo.load(), "no playhead info handled without crashing");
    }

    void testPanicAndLimiter()
    {
        std::cout << "\n== Master: panic, limiter, level ==" << std::endl;
        TwistedBreaksProcessor p;
        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        // Everything at once, master +6 dB: limiter must hold the ceiling.
        setParam (p, tb::params::masterGain, 6.0f);
        for (int n = 36; n <= 51; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 127), 0);
        float worst = 0.0f;
        for (int i = 0; i < 20; ++i) { runBlock (p, host, buf, midi); worst = juce::jmax (worst, peakOf (buf)); }
        check (worst <= 0.9661f, "limiter ceiling holds (-0.3 dBFS) with 16 pads at +6 dB: peak "
                                     + juce::String (worst, 4));
        check (p.getBridge().limiterReductionDb.load() >= 0.0f, "limiter gain reduction published");

        // Panic via the automatable parameter's rising edge.
        for (int n = 36; n <= 39; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 127), 0);
        runBlock (p, host, buf, midi);
        setParam (p, tb::params::masterPanic, 1.0f);
        runBlock (p, host, buf, midi);          // 5 ms fade happens in here (240 samples)
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "panic parameter silences all voices within one block");

        // Panic from the UI button, plus check the fade isn't a hard cut.
        setParam (p, tb::params::masterPanic, 0.0f);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (p, host, buf, midi);
        p.requestPanic();
        runBlock (p, host, buf, midi);
        float maxStep = 0.0f;
        for (int i = 1; i < 240; ++i)
            maxStep = juce::jmax (maxStep, std::abs (buf.getSample (0, i) - buf.getSample (0, i - 1)));
        check (isSilent (peakOf (buf, 300)), "UI panic silences after the 5 ms fade");
        check (maxStep < 0.1f, "panic fade is click-free (max step " + juce::String (maxStep, 4) + ")");

        // MIDI CC 120 = all sound off
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        runBlock (p, host, buf, midi);
        midi.addEvent (juce::MidiMessage::allSoundOff (1), 0);
        runBlock (p, host, buf, midi);
        runBlock (p, host, buf, midi);
        check (isSilent (peakOf (buf)), "MIDI CC 120 (all sound off) acts as panic");

        // Master level -inf really mutes.
        setParam (p, tb::params::masterGain, tb::params::masterGainMinDb);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        for (int i = 0; i < 10; ++i) runBlock (p, host, buf, midi);
        check (peakOf (buf) < 1.0e-3f, "master at -inf is effectively silent");
    }

    void testState()
    {
        std::cout << "\n== State save / restore (Ableton Live Set) ==" << std::endl;
        juce::MemoryBlock blob;
        {
            TwistedBreaksProcessor p;
            setParam (p, tb::params::masterGain, -6.0f);
            setParam (p, tb::params::masterLimiter, 0.0f);
            p.getEngine().setPadNote (0, 48);
            p.getEngine().setPadNote (15, 70);
            p.getStateInformation (blob);
        }
        TwistedBreaksProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        const float g = q.getParameters().getRawParameterValue (tb::params::masterGain)->load();
        check (std::abs (g + 6.0f) < 0.02f, "master_gain restored (-6 dB): " + juce::String (g, 2));
        check (q.getParameters().getRawParameterValue (tb::params::masterLimiter)->load() < 0.5f, "limiter off restored");
        check (q.getEngine().getPadNote (0) == 48 && q.getEngine().getPadNote (15) == 70, "MIDI note map restored");
        check (q.getEngine().getPadNote (1) == 37, "untouched pad keeps default note");

        const char junk[] = "not a valid state";
        q.setStateInformation (junk, (int) sizeof (junk));
        check (q.getEngine().getPadNote (0) == 48, "garbage state blob ignored without crashing");
    }

    //==========================================================================
    int renderSnapshot (const juce::File& out)
    {
        std::cout << "\n== Rendering editor snapshot ==" << std::endl;
        TwistedBreaksProcessor p;
        FakeHost host;
        host.playing = true; host.bpm = 174.0; host.ppq = 4.0 * 11 + 2.5;
        p.setPlayHead (&host);
        p.prepareToPlay (kSr, kBlock);

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (tb::ui::MainView::kWidth, tb::ui::MainView::kHeight);

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        // Play a little break so lamps, meter and scope have something to show.
        const int pattern[] = { 0, 10, 4, 10, 1, 4, 10, 5 };
        for (int i = 0; i < 40; ++i)
        {
            if (i % 5 == 0)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + pattern[(i / 5) % 8], (juce::uint8) 120), 0);
                if (i % 10 == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 46, (juce::uint8) 90), 64);
            }
            for (int k = 0; k < 3; ++k) runBlock (p, host, buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (25);
        }
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 127), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 41, (juce::uint8) 110), 0);
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

    if (argc >= 3 && juce::String (argv[1]) == "--snapshot")
        return renderSnapshot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    std::cout << "TWISTED BREAKS test runner" << std::endl;
    testBasics();
    testMidiAndAudio();
    testTransport();
    testPanicAndLimiter();
    testState();

    std::cout << "\n" << (checks - failures) << " / " << checks << " checks passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
