#include "Core/SongMode.h"
#include "Core/Parameters.h"
#include "PluginProcessor.h"

namespace tb
{
    SongMode::SongMode (TwistedBreaksProcessor& p) : processor (p) { startTimerHz (30); }
    SongMode::~SongMode() { stopTimer(); }

    bool SongMode::isOn() const
    {
        auto* v = const_cast<TwistedBreaksProcessor&> (processor).getParameters().getRawParameterValue (params::songOn);
        return v != nullptr && v->load() > 0.5f;
    }

    void SongMode::add (Entry e)
    {
        if ((int) entries.size() >= kMaxEntries) return;
        e.slot = juce::jlimit (0, SnapshotManager::kSlots - 1, e.slot);
        e.bars = juce::jlimit (1, 64, e.bars);
        entries.push_back (e);
        ++version;
        processor.markStateChanged();
    }

    void SongMode::set (int i, Entry e)
    {
        if (! juce::isPositiveAndBelow (i, (int) entries.size())) return;
        e.slot = juce::jlimit (0, SnapshotManager::kSlots - 1, e.slot);
        e.bars = juce::jlimit (1, 64, e.bars);
        entries[(size_t) i] = e;
        ++version;
        processor.markStateChanged();
    }

    void SongMode::remove (int i)
    {
        if (! juce::isPositiveAndBelow (i, (int) entries.size())) return;
        entries.erase (entries.begin() + i);
        ++version;
        processor.markStateChanged();
    }

    void SongMode::move (int i, int d)
    {
        const int j = i + d;
        if (! juce::isPositiveAndBelow (i, (int) entries.size()) || ! juce::isPositiveAndBelow (j, (int) entries.size())) return;
        std::swap (entries[(size_t) i], entries[(size_t) j]);
        ++version;
        processor.markStateChanged();
    }

    void SongMode::clear()
    {
        entries.clear();
        ++version;
        processor.markStateChanged();
    }

    int SongMode::totalBars() const noexcept
    {
        int n = 0;
        for (const auto& e : entries) n += e.bars;
        return n;
    }

    int SongMode::entryAt (int bar, int* inEntry) const noexcept
    {
        const int total = totalBars();
        if (total <= 0) return -1;
        int b = ((bar % total) + total) % total;
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            if (b < entries[(size_t) i].bars) { if (inEntry != nullptr) *inEntry = b; return i; }
            b -= entries[(size_t) i].bars;
        }
        return -1;
    }

    void SongMode::poll()
    {
        auto& tr = processor.getBridge().transport;
        const bool playing = tr.isPlaying.load();
        if (! isOn() || entries.empty() || ! playing)
        {
            if (! playing) { lastBar = -1; requestedAt = -1; wantedSlot = -1; }
            wasPlaying = playing;
            return;
        }

        const double barLen = juce::jmax (1, tr.timeSigNum.load()) * 4.0 / juce::jmax (1, tr.timeSigDen.load());
        const double ppq = tr.ppq.load();
        const int bar = juce::jmax (0, (int) std::floor (ppq / barLen + 1.0e-6));
        int inEntry = 0;
        const int idx = entryAt (bar, &inEntry);
        auto& snaps = processor.getSnapshots();
        auto stored = [&snaps] (int slot) { return snaps.getState (slot) != SnapshotManager::SlotState::empty; };

        if (bar != lastBar)
        {
            // Started, or jumped: this bar's entry at once (unless it's already on).
            const bool jumped = lastBar < 0 || bar != lastBar + 1;
            if (jumped && idx >= 0)
            {
                const int slot = entries[(size_t) idx].slot;
                if (stored (slot) && snaps.getActive() != slot) snaps.recall (slot, params::switchNow);
                wantedSlot = slot;
            }
            if (idx != current) { current = idx; ++version; }
            lastBar = bar;
            barInSong = ((bar % juce::jmax (1, totalBars())) + totalBars()) % juce::jmax (1, totalBars());
            ++version;
        }

        // The last bar of this entry: queue the next one for the bar line.
        // (Not while a recall is still on its way: one switch at a time.)
        if (idx >= 0 && inEntry == entries[(size_t) idx].bars - 1 && requestedAt != bar && ! snaps.isSwitching())
        {
            requestedAt = bar;
            const int next = entryAt (bar + 1);
            if (next >= 0)
            {
                const int slot = entries[(size_t) next].slot;
                if (stored (slot) && slot != wantedSlot) snaps.recall (slot, params::switchBar);
                wantedSlot = slot;
            }
        }
        wasPlaying = playing;
    }

    juce::ValueTree SongMode::toValueTree() const
    {
        juce::ValueTree t (songId);
        for (const auto& e : entries)
        {
            juce::ValueTree n ("ENTRY");
            n.setProperty ("slot", e.slot, nullptr);
            n.setProperty ("bars", e.bars, nullptr);
            t.appendChild (n, nullptr);
        }
        return t;
    }

    void SongMode::fromValueTree (const juce::ValueTree& t)
    {
        entries.clear();
        for (const auto& n : t)
            if (n.hasType ("ENTRY") && (int) entries.size() < kMaxEntries)
                entries.push_back ({ juce::jlimit (0, SnapshotManager::kSlots - 1, (int) n.getProperty ("slot", 0)),
                                     juce::jlimit (1, 64, (int) n.getProperty ("bars", 4)) });
        lastBar = -1; current = -1; requestedAt = -1; wantedSlot = -1;
        ++version;
    }
}
