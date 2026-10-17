#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

class TwistedBreaksProcessor;

namespace tb
{
    /*
        SongMode (A11 part 5)
        ---------------------
        A chain of snapshots, each for a number of bars: 1 x4, 1 x4, 2 x4, 3 x1 ...
        With SONG on and Live playing, bar 1 of Live's timeline starts the first
        entry; in the last bar of each entry the next one's snapshot is recalled
        with NEXT BAR, so it lands exactly on the bar line. The chain loops.
        Jumping in Live (or starting mid-song) recalls the entry for that bar at once.

        The "Song" parameter switches it on (automatable). Message thread.
    */
    class SongMode final : private juce::Timer
    {
    public:
        struct Entry { int slot = 0; int bars = 4; };
        static constexpr int kMaxEntries = 32;

        explicit SongMode (TwistedBreaksProcessor&);
        ~SongMode() override;

        const std::vector<Entry>& getEntries() const noexcept { return entries; }
        void add (Entry);
        void set (int index, Entry);
        void remove (int index);
        void move (int index, int delta);
        void clear();
        int  totalBars() const noexcept;

        /** Which entry plays at bar `bar` (0-based from Live's bar 1), and the bar inside it. */
        int entryAt (int bar, int* barInEntry = nullptr) const noexcept;
        int getCurrent() const noexcept { return current; }
        int getBarInSong() const noexcept { return barInSong; }
        bool isOn() const;

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static inline const juce::Identifier songId { "SONG" };

        uint32_t getVersion() const noexcept { return version; }

        /** The timer's work (public for tests). */
        void poll();

    private:
        void timerCallback() override { poll(); }

        TwistedBreaksProcessor& processor;
        std::vector<Entry> entries;
        int lastBar = -1, current = -1, requestedAt = -1, barInSong = 0, wantedSlot = -1;
        bool wasPlaying = false;
        uint32_t version = 0;
    };
}
