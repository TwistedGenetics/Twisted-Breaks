#include "Core/BreakBank.h"
#include "Core/SampleManager.h"

namespace tb
{
    BreakBank::BreakBank (SampleManager& s) : samples (s) {}

    BreakBank::Slot BreakBank::get (int slot) const
    {
        return juce::isPositiveAndBelow (slot, kSlots) ? slots[(size_t) slot] : Slot{};
    }

    void BreakBank::install (int slot, const SampleBuffer::Ptr& buf, const SliceTable& slices)
    {
        auto& s = slots[(size_t) slot];
        s.buffer = buf;
        s.status = buf != nullptr ? Status::ready : (s.file != juce::File() ? Status::missing : Status::empty);
        if (buf != nullptr) { s.name = buf->name; s.file = buf->file; }
        s.slices = slices;
        if (buf != nullptr) s.slices.normalise (buf->getNumSamples());
        samples.sendBank (slot, buf, buf != nullptr ? s.slices : SliceTable{});
        ++version;
    }

    void BreakBank::loadFile (int slot, const juce::File& file, const SliceTable* keepSlices)
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        const auto g = ++gen[(size_t) slot];
        auto& s = slots[(size_t) slot];
        s.status = Status::loading;
        s.file = file;
        s.name = file.getFileNameWithoutExtension();
        ++version;

        const SliceTable keep = keepSlices != nullptr ? *keepSlices : SliceTable{};
        const bool haveSlices = keepSlices != nullptr && ! keepSlices->isEmpty();
        juce::WeakReference<BreakBank> weak (this);
        samples.requestBuffer (file, [weak, slot, g, keep, haveSlices] (SampleBuffer::Ptr buf)
        {
            auto* self = weak.get();
            if (self == nullptr || g != self->gen[(size_t) slot]) return;
            if (buf == nullptr || haveSlices) { self->install (slot, buf, keep); return; }
            // New break: AUTO SLICE it (16) in the background.
            self->samples.requestSlices (buf, 0.5f, 16, [weak, slot, g, buf] (SliceTable t)
            {
                auto* me = weak.get();
                if (me != nullptr && g == me->gen[(size_t) slot]) me->install (slot, buf, t);
            });
        });
    }

    void BreakBank::copyFromPad (int slot, int pad)
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        const auto info = samples.getInfo (pad);
        if (info.buffer == nullptr) return;
        ++gen[(size_t) slot];
        slots[(size_t) slot].file = info.buffer->file;
        install (slot, info.buffer, info.slices.isEmpty() ? SliceTable::equal (info.buffer->getNumSamples(), 16) : info.slices);
    }

    void BreakBank::clear (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        ++gen[(size_t) slot];
        slots[(size_t) slot] = {};
        install (slot, nullptr, {});
    }

    juce::ValueTree BreakBank::toValueTree() const
    {
        juce::ValueTree t (bankId);
        for (int i = 0; i < kSlots; ++i)
        {
            const auto& s = slots[(size_t) i];
            if (s.file == juce::File()) continue;
            juce::ValueTree n ("SLOT");
            n.setProperty ("index", i, nullptr);
            n.setProperty ("path", s.file.getFullPathName(), nullptr);
            n.setProperty ("slices", s.slices.toString(), nullptr);
            t.appendChild (n, nullptr);
        }
        return t;
    }

    void BreakBank::fromValueTree (const juce::ValueTree& t)
    {
        for (int i = 0; i < kSlots; ++i) if (slots[(size_t) i].file != juce::File()) clear (i);
        for (const auto& n : t)
        {
            const int i = (int) n.getProperty ("index", -1);
            if (! juce::isPositiveAndBelow (i, kSlots)) continue;
            const auto slices = SliceTable::fromString (n.getProperty ("slices").toString());
            loadFile (i, juce::File (n.getProperty ("path").toString()), &slices);
        }
    }
}
