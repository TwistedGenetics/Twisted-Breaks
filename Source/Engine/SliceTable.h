#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>

namespace tb
{
    /*
        SliceTable
        ----------
        Up to 16 slice START positions (in source samples, ascending) for one
        pad's sample. Slice k plays [starts[k], starts[k+1]) and the last slice
        runs to the end of the sample.

        Plain fixed-size data: copied by value into the lock-free command
        queue, so handing a new table to the audio thread never allocates.
        Editing is always non-destructive — only these numbers change.
    */
    struct SliceTable
    {
        static constexpr int kMax = 16;
        static constexpr int kMinGap = 64;      // samples between markers

        int count = 0;
        std::array<int, kMax> starts {};

        bool isEmpty() const noexcept { return count <= 0; }

        juce::Range<int> region (int slice, int sampleLength) const noexcept
        {
            if (count <= 0) return { 0, sampleLength };
            slice = juce::jlimit (0, count - 1, slice);
            const int s = juce::jlimit (0, sampleLength, starts[(size_t) slice]);
            const int e = slice + 1 < count ? starts[(size_t) slice + 1] : sampleLength;
            return { s, juce::jlimit (s, sampleLength, e) };
        }

        /** Sorts, drops out-of-range / too-close markers, caps at 16. */
        void normalise (int sampleLength) noexcept
        {
            std::sort (starts.begin(), starts.begin() + count);
            int out = 0;
            for (int i = 0; i < count; ++i)
            {
                const int p = starts[(size_t) i];
                if (p < 0 || p >= sampleLength - kMinGap) continue;
                if (out > 0 && p - starts[(size_t) out - 1] < kMinGap) continue;
                starts[(size_t) out++] = p;
            }
            count = juce::jmin (out, kMax);
        }

        static SliceTable equal (int sampleLength, int slices) noexcept
        {
            SliceTable t;
            slices = juce::jlimit (1, kMax, slices);
            for (int i = 0; i < slices; ++i)
                t.starts[(size_t) i] = (int) ((juce::int64) sampleLength * i / slices);
            t.count = slices;
            t.normalise (sampleLength);
            return t;
        }

        /** Index of the slice containing `pos`. */
        int sliceAt (int pos) const noexcept
        {
            int k = 0;
            for (int i = 0; i < count; ++i)
                if (starts[(size_t) i] <= pos) k = i;
            return k;
        }

        juce::String toString() const
        {
            juce::StringArray a;
            for (int i = 0; i < count; ++i) a.add (juce::String (starts[(size_t) i]));
            return a.joinIntoString (",");
        }

        static SliceTable fromString (const juce::String& s)
        {
            SliceTable t;
            for (auto& tok : juce::StringArray::fromTokens (s, ",", {}))
                if (tok.trim().isNotEmpty() && t.count < kMax)
                    t.starts[(size_t) t.count++] = tok.getIntValue();
            return t;
        }

        bool operator== (const SliceTable& o) const noexcept
        {
            return count == o.count && std::equal (starts.begin(), starts.begin() + count, o.starts.begin());
        }
        bool operator!= (const SliceTable& o) const noexcept { return ! (*this == o); }
    };
}
