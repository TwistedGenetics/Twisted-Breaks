#pragma once

#include <juce_core/juce_core.h>
#include "Core/FxDefs.h"

/*
    MacroDefs  (Phase 11: performance macros)
    ---------
    Eight one-key moves, each a recipe of the 16 performance effects with
    their settings. While a macro is on, its effects are switched on in the
    target group(s) and their settings are taken from the recipe, at a point
    between MILD and HARD set by MACRO AMOUNT. Let go and the effects go back
    to exactly how you had them (on / off and settings).

    Setting values use the FX parameter units: 0..1 for knobs, the choice
    index for choices (rounded). Append only: keys are parameter IDs
    ("macro_<key>") and MIDI notes E4.. (76..83) follow the order.
*/
namespace tb::macro
{
    struct Setting { int fx; int param; float mild; float hard; };   // param -1: just switch it on

    struct MacroDef
    {
        const char* key;          // id suffix
        const char* label;        // key text (2 lines max)
        const char* name;         // parameter name
        const char* tip;
        int         numSettings;
        Setting     settings[10];

        uint32_t fxMask() const noexcept
        {
            uint32_t m = 0;
            for (int i = 0; i < numSettings; ++i) m |= 1u << settings[i].fx;
            return m;
        }
    };

    inline constexpr int kNumMacros = 8;
    inline constexpr int kFirstMidiNote = 76;      // E4..B4: macros 1..8 (FX are C3..D#4)

    inline const MacroDef& def (int i)
    {
        using namespace tb::fx;
        static const MacroDef defs[kNumMacros] = {
            { "destroy", "DESTROY", "Destroy", "Hard drive, crushed bits, stepped sample-rate grit", 8,
              { { drive, 0, 1, 1 }, { drive, 1, 0.55f, 1.0f }, { drive, 2, 0.8f, 1.0f },
                { crush, 0, 0.45f, 0.12f }, { crush, 1, 0.6f, 1.0f },
                { rate, 0, 0.55f, 0.28f }, { rate, 1, 1, 1 }, { rate, 2, 0.6f, 1.0f } } },
            { "mangle", "AMEN\nMANGLE", "Amen Mangle", "Beat-repeat chops gated on the grid, a touch of crush", 5,
              { { repeat, 0, 3, 4 }, { gate, 0, 1, 2 }, { gate, 1, 0.7f, 0.45f },
                { crush, 0, 0.6f, 0.4f }, { crush, 1, 0.5f, 0.8f } } },
            { "rewind", "REWIND", "Rewind", "The last beat backwards through tape, thinned out", 6,
              { { reverse, 0, 1, 2 }, { tape, 0, 0.4f, 0.8f }, { tape, 1, 0.7f, 0.5f },
                { hpf, 0, 0.25f, 0.4f }, { hpf, 1, 0.2f, 0.3f }, { hpf, 2, 0.0f, 0.0f } } },
            { "darken", "DARKEN", "Darken", "Low-pass, warm tape and a dark room", 9,
              { { lpf, 0, 0.42f, 0.18f }, { lpf, 1, 0.35f, 0.55f }, { lpf, 2, 0.0f, 0.3f },
                { tape, 0, 0.3f, 0.6f }, { tape, 1, 0.5f, 0.3f },
                { reverb, 0, 0.6f, 0.75f }, { reverb, 1, 0.7f, 0.8f }, { reverb, 2, 0.15f, 0.3f } } },
            { "tapestop", "TAPE\nSTOP", "Tape Stop", "Slows to a halt over 1/2 bar (HARD: a whole bar), tape and low-pass", 6,
              { { pitch, 0, 3, 4 }, { tape, 0, 0.5f, 0.9f }, { tape, 1, 0.6f, 0.35f },
                { lpf, 0, 0.7f, 0.45f }, { lpf, 1, 0.2f, 0.3f }, { lpf, 2, 0.0f, 0.0f } } },
            { "dive", "PITCH\nDIVE", "Pitch Dive", "A fast drop (1/8, HARD: 1/16) thrown into a 1/4 delay", 4,
              { { pitch, 0, 1, 0 }, { delay, 0, 3, 3 }, { delay, 1, 0.35f, 0.6f }, { delay, 2, 0.3f, 0.45f } } },
            { "roll", "JUNGLE\nROLL", "Jungle Roll", "1/16 stutter roll (HARD: 1/32), lifted with a high-pass and a 1/16 echo", 7,
              { { stutter, 0, 3, 4 }, { hpf, 0, 0.2f, 0.35f }, { hpf, 1, 0.25f, 0.35f }, { hpf, 2, 0.0f, 0.0f },
                { delay, 0, 0, 0 }, { delay, 1, 0.2f, 0.4f }, { delay, 2, 0.15f, 0.3f } } },
            { "scifi", "SCI-FI", "Sci-Fi", "Resonator, a 3/16 echo and a resonant band-pass", 9,
              { { comb, 0, 0.55f, 0.75f }, { comb, 1, 0.78f, 0.93f }, { comb, 2, 0.2f, 0.2f },
                { delay, 0, 2, 2 }, { delay, 1, 0.55f, 0.75f }, { delay, 2, 0.35f, 0.5f },
                { bpf, 0, 0.55f, 0.7f }, { bpf, 1, 0.5f, 0.8f }, { bpf, 2, 0.0f, 0.2f } } },
        };
        return defs[juce::jlimit (0, kNumMacros - 1, i)];
    }

    /** The recipe's value for `amount` 0..1 (choices round to an index). */
    inline float valueAt (const Setting& s, float amount) noexcept
    {
        const float v = s.mild + (s.hard - s.mild) * juce::jlimit (0.0f, 1.0f, amount);
        return fx::def (s.fx).params[s.param].isChoice() ? (float) juce::roundToInt (v) : v;
    }

    inline juce::String paramId (int i) { return juce::String ("macro_") + def (i).key; }
}
