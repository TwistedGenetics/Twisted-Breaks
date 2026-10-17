#include "Core/TransportSync.h"
#include <cmath>

namespace tb
{
    //==========================================================================
    int TransportState::stepAt (int sampleOffset, int stepsPerPattern) const noexcept
    {
        // 16th note = 0.25 PPQ. Anchored to song start so patterns line up
        // with Ableton's bar 1 regardless of where playback began.
        const double sixteenths = std::floor (ppqAt (sampleOffset) * 4.0 + 1.0e-9);
        const auto   n          = (long long) sixteenths;
        const auto   len        = (long long) juce::jmax (1, stepsPerPattern);
        return (int) (((n % len) + len) % len);
    }

    std::optional<int> TransportState::nextBoundary (double divisionPpq, int fromSample) const noexcept
    {
        if (! isPlaying || ppqPerSample <= 0.0 || divisionPpq <= 0.0 || fromSample >= numSamples)
            return std::nullopt;

        const double startPpq = ppqAt (fromSample);

        // Small epsilon so a boundary sitting exactly on fromSample counts.
        const double eps        = ppqPerSample * 0.5;
        const double boundary   = std::ceil ((startPpq - eps) / divisionPpq) * divisionPpq;
        const double offset     = (boundary - ppqAtBlockStart) / ppqPerSample;
        const int    sample     = (int) std::llround (offset);

        if (sample < fromSample)  return fromSample;
        if (sample >= numSamples) return std::nullopt;
        return sample;
    }

    //==========================================================================
    void TransportSync::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        reset();
    }

    void TransportSync::reset() noexcept
    {
        state         = {};
        state.sampleRate = sampleRate;
        wasPlaying    = false;
        hasPrediction = false;
        predictedPpq  = 0.0;
    }

    const TransportState& TransportSync::update (juce::AudioPlayHead* playHead, int numSamples) noexcept
    {
        TransportState s;
        s.sampleRate = sampleRate;
        s.numSamples = numSamples;

        if (playHead != nullptr)
        {
            if (const auto pos = playHead->getPosition())
            {
                s.hostProvidesInfo = true;
                s.isPlaying        = pos->getIsPlaying();
                s.isRecording      = pos->getIsRecording();

                if (const auto bpm = pos->getBpm())
                    s.bpm = juce::jlimit (20.0, 999.0, *bpm);

                if (const auto sig = pos->getTimeSignature())
                {
                    s.timeSigNumerator   = juce::jmax (1, sig->numerator);
                    s.timeSigDenominator = juce::jmax (1, sig->denominator);
                }

                if (const auto ppq = pos->getPpqPosition())
                    s.ppqAtBlockStart = *ppq;
                else if (const auto secs = pos->getTimeInSeconds())
                    s.ppqAtBlockStart = *secs * s.bpm / 60.0;     // fallback

                if (const auto barStart = pos->getPpqPositionOfLastBarStart())
                    s.ppqOfBarStart = *barStart;
                else
                    s.ppqOfBarStart = std::floor (s.ppqAtBlockStart / s.ppqPerBar()) * s.ppqPerBar();
            }
        }

        // Host gave nothing (some hosts while not playing, or the test runner
        // without a playhead): keep the last tempo, stay stopped.
        if (! s.hostProvidesInfo)
        {
            s.bpm             = state.bpm;
            s.ppqAtBlockStart = state.ppqAtBlockStart;
            s.ppqOfBarStart   = state.ppqOfBarStart;
        }

        s.ppqPerSample = s.isPlaying ? (s.bpm / 60.0) / sampleRate : 0.0;

        // Bar / beat. Derived from the host's bar start when available, which
        // copes with time-signature changes mid-song in Ableton.
        const double qnPerBeat = 4.0 / (double) s.timeSigDenominator;
        s.barIndex  = (int) std::floor (s.ppqOfBarStart / s.ppqPerBar() + 1.0e-6);
        s.beatInBar = juce::jlimit (0, s.timeSigNumerator - 1,
                                    (int) std::floor ((s.ppqAtBlockStart - s.ppqOfBarStart) / qnPerBeat + 1.0e-6));

        // Edge detection.
        s.justStarted = s.isPlaying && ! wasPlaying;
        s.justStopped = ! s.isPlaying && wasPlaying;

        // Jump detection: while playing, the new block should start exactly
        // where the last one ended. Allow a quarter of a 64th-note of slop
        // for hosts that round positions.
        if (s.isPlaying && wasPlaying && hasPrediction)
            s.jumped = std::abs (s.ppqAtBlockStart - predictedPpq) > (1.0 / 64.0);

        predictedPpq  = s.ppqAtBlockStart + (double) numSamples * s.ppqPerSample;
        hasPrediction = s.isPlaying;
        wasPlaying    = s.isPlaying;

        state = s;
        return state;
    }

    void TransportSync::publish (TransportUiState& ui) const noexcept
    {
        ui.hostProvidesInfo.store (state.hostProvidesInfo, std::memory_order_relaxed);
        ui.isPlaying       .store (state.isPlaying,        std::memory_order_relaxed);
        ui.isRecording     .store (state.isRecording,      std::memory_order_relaxed);
        ui.bpm             .store (state.bpm,              std::memory_order_relaxed);
        ui.ppq             .store (state.ppqAtBlockStart,  std::memory_order_relaxed);
        ui.bar             .store (state.barIndex,         std::memory_order_relaxed);
        ui.beat            .store (state.beatInBar,        std::memory_order_relaxed);
        ui.step            .store (state.stepAt (0),       std::memory_order_relaxed);
        ui.timeSigNum      .store (state.timeSigNumerator, std::memory_order_relaxed);
        ui.timeSigDen      .store (state.timeSigDenominator, std::memory_order_relaxed);
    }
}
