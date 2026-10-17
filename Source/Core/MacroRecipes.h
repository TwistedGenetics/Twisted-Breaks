#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include "Core/MacroDefs.h"

namespace tb
{
    /*
        MacroRecipes  (A12 part 4)
        ------------
        The eight macros' recipes, editable: for each macro, which of the 16
        effects it switches on and, for each of an effect's settings, either
        "leave it as it is" or a MILD and a HARD value (MACRO AMOUNT moves
        between them). Factory recipes come from MacroDefs.

        Plain atomics, so the audio thread reads them while the editor writes
        (a value may be one edit behind for one block, never torn). Names are
        message-thread only. Saved with the Live set, in kit presets and as
        .tbmacro files (one macro).
    */
    class MacroRecipes
    {
    public:
        static constexpr int kMacros = macro::kNumMacros;
        static constexpr int kFx = fx::kNumFx;

        MacroRecipes() { for (int m = 0; m < kMacros; ++m) resetToFactory (m); }

        // ---- Audio thread ------------------------------------------------------------
        bool  usesFx (int m, int f) const noexcept         { return fxOn[idx (m, f)].load (std::memory_order_relaxed); }
        bool  setsParam (int m, int f, int k) const noexcept { return set[pidx (m, f, k)].load (std::memory_order_relaxed); }
        float valueAt (int m, int f, int k, float amount) const noexcept
        {
            const float a = mild[pidx (m, f, k)].load (std::memory_order_relaxed);
            const float b = hard[pidx (m, f, k)].load (std::memory_order_relaxed);
            const float v = a + (b - a) * juce::jlimit (0.0f, 1.0f, amount);
            return fx::def (f).params[k].isChoice() ? (float) juce::roundToInt (v) : v;
        }
        uint32_t fxMask (int m) const noexcept
        {
            uint32_t mask = 0;
            for (int f = 0; f < kFx; ++f) if (usesFx (m, f)) mask |= 1u << f;
            return mask;
        }

        // ---- Message thread ------------------------------------------------------------
        float getMild (int m, int f, int k) const noexcept { return mild[pidx (m, f, k)].load(); }
        float getHard (int m, int f, int k) const noexcept { return hard[pidx (m, f, k)].load(); }
        void setUsesFx (int m, int f, bool on) noexcept     { fxOn[idx (m, f)] = on; bump(); }
        void setSetsParam (int m, int f, int k, bool on) noexcept { set[pidx (m, f, k)] = on; bump(); }
        void setMild (int m, int f, int k, float v) noexcept { mild[pidx (m, f, k)] = v; bump(); }
        void setHard (int m, int f, int k, float v) noexcept { hard[pidx (m, f, k)] = v; bump(); }

        juce::String getName (int m) const { return names[(size_t) juce::jlimit (0, kMacros - 1, m)]; }
        void setName (int m, const juce::String& n) { names[(size_t) juce::jlimit (0, kMacros - 1, m)] = n.substring (0, 24); bump(); }

        void resetToFactory (int m)
        {
            const auto& d = macro::def (m);
            names[(size_t) m] = juce::String (d.label);
            for (int f = 0; f < kFx; ++f)
            {
                fxOn[idx (m, f)] = false;
                for (int k = 0; k < 3; ++k)
                {
                    set[pidx (m, f, k)] = false;
                    const float def = fx::def (f).numParams > k ? fx::def (f).params[k].def : 0.0f;
                    mild[pidx (m, f, k)] = def;
                    hard[pidx (m, f, k)] = def;
                }
            }
            for (int i = 0; i < d.numSettings; ++i)
            {
                const auto& s = d.settings[i];
                fxOn[idx (m, s.fx)] = true;
                if (s.param >= 0)
                {
                    set[pidx (m, s.fx, s.param)] = true;
                    mild[pidx (m, s.fx, s.param)] = s.mild;
                    hard[pidx (m, s.fx, s.param)] = s.hard;
                }
            }
            bump();
        }

        bool isFactory (int m) const
        {
            MacroRecipes f;
            return toValueTree (m).isEquivalentTo (f.toValueTree (m));
        }

        juce::ValueTree toValueTree (int m) const
        {
            juce::ValueTree t ("MACRO");
            t.setProperty ("index", m, nullptr);
            t.setProperty ("name", getName (m), nullptr);
            for (int f = 0; f < kFx; ++f)
            {
                if (! usesFx (m, f)) continue;
                juce::ValueTree e ("FX");
                e.setProperty ("fx", fx::def (f).key, nullptr);
                for (int k = 0; k < fx::def (f).numParams; ++k)
                {
                    if (! setsParam (m, f, k)) continue;
                    juce::ValueTree p ("SET");
                    p.setProperty ("param", fx::def (f).params[k].key, nullptr);
                    p.setProperty ("mild", getMild (m, f, k), nullptr);
                    p.setProperty ("hard", getHard (m, f, k), nullptr);
                    e.appendChild (p, nullptr);
                }
                t.appendChild (e, nullptr);
            }
            return t;
        }

        /** Loads one macro's recipe into slot `m` (effects found by key, so the order can change). */
        void fromValueTree (int m, const juce::ValueTree& t)
        {
            if (! t.hasType ("MACRO")) return;
            for (int f = 0; f < kFx; ++f)
            {
                fxOn[idx (m, f)] = false;
                for (int k = 0; k < 3; ++k) set[pidx (m, f, k)] = false;
            }
            names[(size_t) m] = t.getProperty ("name", names[(size_t) m]).toString();
            for (const auto& e : t)
            {
                const int f = fxIndex (e.getProperty ("fx").toString());
                if (f < 0) continue;
                fxOn[idx (m, f)] = true;
                for (const auto& p : e)
                {
                    const auto key = p.getProperty ("param").toString();
                    for (int k = 0; k < fx::def (f).numParams; ++k)
                        if (key == fx::def (f).params[k].key)
                        {
                            set[pidx (m, f, k)] = true;
                            mild[pidx (m, f, k)] = (float) p.getProperty ("mild", 0.0f);
                            hard[pidx (m, f, k)] = (float) p.getProperty ("hard", 0.0f);
                        }
                }
            }
            bump();
        }

        static inline const juce::Identifier macrosId { "MACROS" };

        /** Only macros that differ from the factory are stored (so factory updates still reach the rest). */
        juce::ValueTree toValueTree() const
        {
            juce::ValueTree t (macrosId);
            for (int m = 0; m < kMacros; ++m) if (! isFactory (m)) t.appendChild (toValueTree (m), nullptr);
            return t;
        }

        void fromValueTree (const juce::ValueTree& t)
        {
            for (int m = 0; m < kMacros; ++m) resetToFactory (m);
            for (const auto& c : t)
            {
                const int m = (int) c.getProperty ("index", -1);
                if (juce::isPositiveAndBelow (m, kMacros)) fromValueTree (m, c);
            }
        }

        uint32_t getVersion() const noexcept { return version.load(); }

        static int fxIndex (const juce::String& key)
        {
            for (int f = 0; f < kFx; ++f) if (key == fx::def (f).key) return f;
            return -1;
        }

    private:
        static constexpr size_t idx (int m, int f) noexcept { return (size_t) (m * kFx + f); }
        static constexpr size_t pidx (int m, int f, int k) noexcept { return (size_t) ((m * kFx + f) * 3 + k); }
        void bump() noexcept { version.fetch_add (1); }

        std::array<std::atomic<bool>, kMacros * kFx> fxOn {};
        std::array<std::atomic<bool>, kMacros * kFx * 3> set {};
        std::array<std::atomic<float>, kMacros * kFx * 3> mild {}, hard {};
        std::array<juce::String, kMacros> names;
        std::atomic<uint32_t> version { 0 };
    };
}
