#include "Engine/CalibrationVoice.h"
#include <cmath>

namespace tb
{
    namespace
    {
        constexpr float kFadeInMs  = 2.0f;
        constexpr float kFadeOutMs = 5.0f;
        constexpr float kSilence   = 1.0e-4f;   // ~ -80 dB

        float perSampleDecay (double sampleRate, float timeToMinus60dBms) noexcept
        {
            const double samples = sampleRate * (double) timeToMinus60dBms * 0.001;
            return (float) std::pow (0.001, 1.0 / juce::jmax (1.0, samples));
        }
    }

    void CalibrationVoice::prepare (double newSampleRate) noexcept
    {
        sampleRate  = newSampleRate;
        fadeInStep  = 1.0f / juce::jmax (1.0f, (float) (sampleRate * kFadeInMs  * 0.001));
        fadeOutStep = 1.0f / juce::jmax (1.0f, (float) (sampleRate * kFadeOutMs * 0.001));
    }

    void CalibrationVoice::configureForPad (int padIndex) noexcept
    {
        const int row = juce::jlimit (0, 3, padIndex / 4);
        const int col = padIndex % 4;
        rng ^= (uint32_t) (padIndex + 1) * 2654435761u;

        // Spread columns across the stereo field a little, like a kit.
        pan = (float) (col - 1.5f) * 0.18f;

        switch (row)
        {
            case 0: // sub kick — the low end of a jungle kit
                timbre     = Timbre::kick;
                baseFreq   = 42.0f + 7.0f * (float) col;
                sweepFreq  = 180.0f;
                sweepDecay = perSampleDecay (sampleRate, 45.0f);
                ampDecay   = perSampleDecay (sampleRate, 420.0f + 60.0f * (float) col);
                noiseAmt   = 0.0f;
                break;

            case 1: // snare — tone body + bright noise
                timbre     = Timbre::snare;
                baseFreq   = 180.0f + 25.0f * (float) col;
                sweepFreq  = 90.0f;
                sweepDecay = perSampleDecay (sampleRate, 25.0f);
                ampDecay   = perSampleDecay (sampleRate, 230.0f);
                noiseAmt   = 0.75f;
                noiseHpCoef = 0.55f;
                break;

            case 2: // hats — high-passed noise, shorter on the right
                timbre     = Timbre::hat;
                baseFreq   = 0.0f;
                sweepFreq  = 0.0f;
                sweepDecay = 1.0f;
                ampDecay   = perSampleDecay (sampleRate, col == 3 ? 380.0f : 70.0f - 10.0f * (float) col);
                noiseAmt   = 1.0f;
                noiseHpCoef = 0.92f;
                break;

            default: // dosimeter tick — short pitched blip, a tuning reference
                timbre     = Timbre::tick;
                baseFreq   = 440.0f * std::pow (2.0f, (float) (col * 3) / 12.0f);
                sweepFreq  = 0.0f;
                sweepDecay = 1.0f;
                ampDecay   = perSampleDecay (sampleRate, 160.0f);
                noiseAmt   = 0.05f;
                noiseHpCoef = 0.9f;
                break;
        }
    }

    void CalibrationVoice::start (float newVelocity) noexcept
    {
        velocity  = juce::jlimit (0.0f, 1.0f, newVelocity);
        active    = true;
        releasing = false;
        phase     = 0.0;
        sweep     = sweepFreq;
        amp       = 1.0f;
        fade      = 0.0f;           // fades in over 2 ms: no onset click
        noiseLp   = 0.0f;
    }

    void CalibrationVoice::release() noexcept
    {
        if (active) releasing = true;
    }

    void CalibrationVoice::kill() noexcept
    {
        active = releasing = false;
        fade = amp = 0.0f;
    }

    float CalibrationVoice::nextNoise() noexcept
    {
        // xorshift32 — deterministic, allocation-free, cheap.
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (float) (int32_t) rng * (1.0f / 2147483648.0f);
    }

    float CalibrationVoice::renderAdding (juce::AudioBuffer<float>& out, int startSample, int num) noexcept
    {
        if (! active) return 0.0f;

        auto* left  = out.getWritePointer (0);
        auto* right = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;

        // Equal-power pan.
        const float angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float gainL = std::cos (angle) * velocity;
        const float gainR = std::sin (angle) * velocity;
        const double twoPiOverSr = juce::MathConstants<double>::twoPi / sampleRate;

        float peak = 0.0f;

        for (int i = startSample; i < startSample + num; ++i)
        {
            // Anti-click envelope.
            if (releasing)
            {
                fade -= fadeOutStep;
                if (fade <= 0.0f) { kill(); break; }
            }
            else if (fade < 1.0f)
            {
                fade = juce::jmin (1.0f, fade + fadeInStep);
            }

            float s = 0.0f;

            if (timbre != Timbre::hat)
            {
                const float freq = baseFreq + sweep;
                s += (float) std::sin (phase);
                phase += (double) freq * twoPiOverSr;
                if (phase > juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
                sweep *= sweepDecay;
            }

            if (noiseAmt > 0.0f)
            {
                // One-pole low-pass subtracted from input = simple high-pass.
                const float n = nextNoise();
                noiseLp += (1.0f - noiseHpCoef) * (n - noiseLp);
                s = s * (1.0f - noiseAmt) + (n - noiseLp) * noiseAmt * 1.4f;
            }

            if (timbre == Timbre::kick)
                s = std::tanh (s * 1.6f);          // a little weight

            const float v = s * amp * fade * 0.6f;
            amp *= ampDecay;

            left[i] += v * gainL;
            if (right != nullptr) right[i] += v * gainR;
            peak = juce::jmax (peak, std::abs (v));

            if (amp < kSilence) { kill(); break; }
        }

        return peak;
    }
}
