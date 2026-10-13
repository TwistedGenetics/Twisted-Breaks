#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

/*
    FxDefs
    ------
    The 16 performance effects, in chain order, with their settings. One
    table drives the parameter layout, the engine and the FX editor, so they
    can never disagree.

    Parameter IDs (stable, per group a..d):
        grp_a_lpf            on / off
        grp_a_lpf_cutoff     setting (0..1 for knobs, index for choices)

    Knob settings are stored 0..1 and mapped to real units here
    (cutoffHz(), bits(), ...), so the curves can be tuned without
    breaking saved sets or automation.
*/
namespace tb::fx
{
    enum Id
    {
        mute, lpf, hpf, bpf,
        crush, rate, drive, tape,
        stutter, repeat, gate, pitch,
        delay, reverb, comb, reverse,
        kNumFx
    };

    enum class Text { percent, hz, bits, db, semis, rateHz, choice };

    struct ParamDef
    {
        const char* key;          // id suffix: "cutoff"
        const char* label;        // knob caption: "CUTOFF"
        Text        text;
        float       def;          // 0..1, or choice index
        const char* const* choices = nullptr;
        int         numChoices = 0;
        bool isChoice() const noexcept { return text == Text::choice; }
    };

    struct FxDef
    {
        const char* key;          // "lpf"
        const char* label;        // grid key text: "LPF"
        const char* name;         // full name: "LOW PASS FILTER"
        int         numParams;
        ParamDef    params[3];
    };

    // ---- Choice lists (append only: indices are saved) ------------------------
    inline const char* const kSizes[]      = { "1/2", "1/4", "1/8", "1/16", "1/32" };
    inline constexpr double  kSizeBeats[]  = { 2.0,   1.0,   0.5,   0.25,   0.125 };
    inline const char* const kGateDivs[]   = { "1/8", "1/16", "1/32" };
    inline constexpr double  kGateBeats[]  = { 0.5,   0.25,   0.125 };
    inline const char* const kDropTimes[]  = { "1/16", "1/8", "1/4", "1/2", "1 BAR" };
    inline constexpr double  kDropBeats[]  = { 0.25,   0.5,   1.0,   2.0,   4.0 };
    inline const char* const kDelayTimes[] = { "1/16", "1/8", "3/16", "1/4", "3/8", "1/2" };
    inline constexpr double  kDelayBeats[] = { 0.25,   0.5,   0.75,   1.0,   1.5,   2.0 };
    inline const char* const kRevSizes[]   = { "1/8", "1/4", "1/2", "1 BAR" };
    inline constexpr double  kRevBeats[]   = { 0.5,   1.0,   2.0,   4.0 };
    inline const char* const kDriveTypes[] = { "SOFT", "HARD", "SAT" };
    inline const char* const kRateModes[]  = { "SMOOTH", "STEPPED" };

    inline const FxDef& def (int id)
    {
        using T = Text;
        static const FxDef defs[kNumFx] = {
            { "fxmute",  "MUTE",    "MUTE",               0, {} },     // not "mute": grp_a_mute is the mixer's mute
            { "lpf",     "LPF",     "LOW PASS FILTER",    3, { { "cutoff", "CUTOFF", T::hz, 0.55f }, { "res", "RES", T::percent, 0.3f }, { "drive", "DRIVE", T::percent, 0.0f } } },
            { "hpf",     "HPF",     "HIGH PASS FILTER",   3, { { "cutoff", "CUTOFF", T::hz, 0.45f }, { "res", "RES", T::percent, 0.3f }, { "drive", "DRIVE", T::percent, 0.0f } } },
            { "bpf",     "BPF",     "BAND PASS FILTER",   3, { { "cutoff", "CUTOFF", T::hz, 0.5f  }, { "res", "RES", T::percent, 0.5f }, { "drive", "DRIVE", T::percent, 0.0f } } },
            { "crush",   "CRUSH",   "BIT CRUSH",          2, { { "bits", "BITS", T::bits, 0.33f }, { "mix", "MIX", T::percent, 1.0f } } },
            { "rate",    "RATE",    "SAMPLE RATE REDUCE", 3, { { "freq", "RATE", T::rateHz, 0.5f }, { "mode", "MODE", T::choice, 0.0f, kRateModes, 2 }, { "mix", "MIX", T::percent, 1.0f } } },
            { "drive",   "DRIVE",   "DISTORTION / DRIVE", 3, { { "type", "TYPE", T::choice, 0.0f, kDriveTypes, 3 }, { "amount", "DRIVE", T::db, 0.5f }, { "mix", "MIX", T::percent, 1.0f } } },
            { "tape",    "TAPE",    "TAPE SATURATION",    2, { { "sat", "SAT", T::percent, 0.5f }, { "tone", "TONE", T::hz, 0.75f } } },
            { "stutter", "STUTTER", "STUTTER",            1, { { "size", "SIZE", T::choice, 3.0f, kSizes, 5 } } },
            { "repeat",  "REPEAT",  "BEAT REPEAT",        1, { { "div", "DIV", T::choice, 2.0f, kSizes, 5 } } },
            { "gate",    "GATE",    "GATE",               2, { { "div", "DIV", T::choice, 1.0f, kGateDivs, 3 }, { "duty", "DUTY", T::percent, 0.5f } } },
            { "pitch",   "PITCH",   "PITCH DROP",         1, { { "time", "TIME", T::choice, 2.0f, kDropTimes, 5 } } },
            { "delay",   "DELAY",   "DELAY",              3, { { "time", "TIME", T::choice, 2.0f, kDelayTimes, 6 }, { "fb", "FEEDBACK", T::percent, 0.45f }, { "mix", "MIX", T::percent, 0.4f } } },
            { "reverb",  "REVERB",  "REVERB",             3, { { "size", "SIZE", T::percent, 0.7f }, { "damp", "DAMP", T::percent, 0.4f }, { "mix", "MIX", T::percent, 0.35f } } },
            { "comb",    "COMB",    "COMB / RESONATOR",   3, { { "pitch", "PITCH", T::semis, 0.5f }, { "fb", "FEEDBACK", T::percent, 0.75f }, { "damp", "DAMP", T::percent, 0.3f } } },
            { "reverse", "REVERSE", "REVERSE",            1, { { "size", "SIZE", T::choice, 1.0f, kRevSizes, 4 } } },
        };
        return defs[juce::jlimit (0, kNumFx - 1, id)];
    }

    // ---- 0..1 -> real units ------------------------------------------------------
    inline float cutoffHz (float n) noexcept { return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, n)); }    // 20 Hz .. 20 kHz
    inline float toneHz   (float n) noexcept { return cutoffHz (n); }
    inline float bits     (float n) noexcept { return 1.0f + 15.0f * juce::jlimit (0.0f, 1.0f, n); }                // 1 .. 16
    inline float rateHz   (float n) noexcept { return 200.0f * std::pow (220.5f, juce::jlimit (0.0f, 1.0f, n)); }   // 200 Hz .. 44.1 kHz
    inline float driveDb  (float n) noexcept { return 36.0f * juce::jlimit (0.0f, 1.0f, n); }                       // 0 .. 36 dB
    inline float combSemis (float n) noexcept { return -24.0f + 48.0f * juce::jlimit (0.0f, 1.0f, n); }             // around C3
    inline float combHz   (float n) noexcept { return 130.81f * std::pow (2.0f, combSemis (n) / 12.0f); }           // 32.7 Hz .. 523 Hz

    inline juce::String valueText (const ParamDef& p, float v)
    {
        switch (p.text)
        {
            case Text::percent: return juce::String (juce::roundToInt (v * 100.0f)) + " %";
            case Text::hz:      { const float hz = cutoffHz (v); return hz >= 1000.0f ? juce::String (hz / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz"; }
            case Text::bits:    return juce::String (bits (v), 1) + " bit";
            case Text::db:      return "+" + juce::String (driveDb (v), 1) + " dB";
            case Text::semis:   { const float s = combSemis (v); return (s >= 0.0f ? "+" : "") + juce::String (s, 1) + " st"; }
            case Text::rateHz:  { const float hz = rateHz (v); return hz >= 1000.0f ? juce::String (hz / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz"; }
            case Text::choice:  { const int i = juce::jlimit (0, p.numChoices - 1, juce::roundToInt (v)); return p.choices[i]; }
        }
        return {};
    }

    /** "grp_a_lpf" (on/off) or "grp_a_lpf_cutoff" (setting). */
    inline juce::String paramId (int group, int fxId, int param = -1)
    {
        juce::String s = juce::String ("grp_") + juce::String::charToString ((juce::juce_wchar) ('a' + group)) + "_" + def (fxId).key;
        if (param >= 0) s << "_" << def (fxId).params[param].key;
        return s;
    }

    /** Default key modes: the "hit" effects are MOMENTARY, the rest LOCKED. */
    inline constexpr uint32_t kDefaultLockedMask = 0xffffu & ~((1u << mute) | (1u << stutter) | (1u << repeat) | (1u << pitch) | (1u << reverse));

    /** MIDI: notes C3..D#4 (60..75) play FX 1..16 of the FX panel's selected group. */
    inline constexpr int kFirstMidiNote = 60;
}
