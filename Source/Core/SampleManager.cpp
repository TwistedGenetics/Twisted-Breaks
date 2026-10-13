#include "Core/SampleManager.h"
#include "Engine/AudioEngine.h"

namespace tb
{
    const juce::Identifier SampleManager::padsId { "PADS" };

    namespace
    {
        const juce::Identifier kPadId   { "PAD" };
        const juce::Identifier kIndexId { "index" };
        const juce::Identifier kPathId  { "path" };

        SampleManager::Status toStatus (SampleLoader::Status s)
        {
            switch (s)
            {
                case SampleLoader::Status::ok:         return SampleManager::Status::ready;
                case SampleLoader::Status::missing:    return SampleManager::Status::missing;
                case SampleLoader::Status::tooLong:    return SampleManager::Status::tooLong;
                case SampleLoader::Status::empty:
                case SampleLoader::Status::unreadable:
                default:                               return SampleManager::Status::unreadable;
            }
        }
    }

    SampleManager::SampleManager (AudioEngine& e)
        : engine (e),
          loader (juce::ThreadPoolOptions{}.withThreadName ("TwistedBreaks loader").withNumberOfThreads (1))
    {
        formats.registerBasicFormats();
        startTimerHz (20);
    }

    SampleManager::~SampleManager()
    {
        stopTimer();
        loader.removeAllJobs (true, 10000);   // let any running decode finish
        pending.clear();
    }

    juce::String SampleManager::statusText (Status s)
    {
        switch (s)
        {
            case Status::empty:      return "EMPTY";
            case Status::loading:    return "LOADING";
            case Status::ready:      return "READY";
            case Status::missing:    return "MISSING";
            case Status::unreadable: return "UNREADABLE";
            case Status::tooLong:    return "TOO LONG";
        }
        return {};
    }

    //==========================================================================
    void SampleManager::setInfo (int pad, PadInfo&& info)
    {
        {
            const juce::ScopedLock sl (lock);
            info.version = pads[(size_t) pad].version + 1;
            pads[(size_t) pad] = std::move (info);
        }
        if (onStateChanged) onStateChanged();
    }

    SampleManager::PadInfo SampleManager::getInfo (int pad) const
    {
        const juce::ScopedLock sl (lock);
        return juce::isPositiveAndBelow (pad, kNumPads) ? pads[(size_t) pad] : PadInfo {};
    }

    uint32_t SampleManager::getVersion (int pad) const
    {
        const juce::ScopedLock sl (lock);
        return juce::isPositiveAndBelow (pad, kNumPads) ? pads[(size_t) pad].version : 0;
    }

    bool SampleManager::isBusy() const
    {
        const juce::ScopedLock sl (lock);
        for (const auto& p : pads)
            if (p.status == Status::loading) return true;
        return false;
    }

    //==========================================================================
    void SampleManager::loadFile (int pad, const juce::File& file)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;

        const auto gen = ++generation[(size_t) pad];

        PadInfo info;
        info.status = Status::loading;
        info.name   = file.getFileNameWithoutExtension();
        info.file   = file;
        {
            // Keep playing the old sample until the new one is ready.
            const juce::ScopedLock sl (lock);
            info.buffer = pads[(size_t) pad].buffer;
        }
        setInfo (pad, std::move (info));

        juce::WeakReference<SampleManager> weak (this);
        auto* fm = &formats;

        loader.addJob ([weak, pad, gen, file, fm]
        {
            auto result = SampleLoader::loadFile (file, *fm);
            juce::MessageManager::callAsync ([weak, pad, gen, result]
            {
                if (auto* self = weak.get())
                    self->install (pad, gen, result);
            });
        });
    }

    void SampleManager::clearPad (int pad)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;
        ++generation[(size_t) pad];          // cancels an in-flight load
        setInfo (pad, {});
        queueForEngine (pad, nullptr);
    }

    void SampleManager::install (int pad, uint32_t gen, const SampleLoader::Result& result)
    {
        if (gen != generation[(size_t) pad]) return;   // superseded by a newer request

        PadInfo info = getInfo (pad);
        info.status = toStatus (result.status);

        if (result.status == SampleLoader::Status::ok)
        {
            info.buffer = result.buffer;
            info.name   = result.buffer->name;
            keepAlive.add (result.buffer.get());
            queueForEngine (pad, result.buffer);
        }
        else
        {
            info.buffer = nullptr;       // pad goes silent and shows the problem
            queueForEngine (pad, nullptr);
        }

        setInfo (pad, std::move (info));
    }

    //==========================================================================
    void SampleManager::queueForEngine (int pad, const SampleBuffer::Ptr& buffer)
    {
        pending.push_back ({ pad, buffer });
        flushPending();
    }

    void SampleManager::flushPending()
    {
        size_t sent = 0;
        for (; sent < pending.size(); ++sent)
        {
            auto* raw = pending[sent].buffer.get();
            if (raw != nullptr) raw->incReferenceCount();          // reference handed to the engine

            if (! engine.postSample (pending[sent].pad, raw))
            {
                if (raw != nullptr) raw->decReferenceCountWithoutDeleting();
                break;                                              // queue full: retry on the timer
            }
        }
        pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) sent);
    }

    void SampleManager::timerCallback()
    {
        flushPending();

        // Free buffers only this list still references. Doing it here keeps
        // every deallocation off the audio thread.
        for (int i = keepAlive.size(); --i >= 0;)
            if (keepAlive.getObjectPointerUnchecked (i)->getReferenceCount() == 1)
                keepAlive.remove (i);
    }

    //==========================================================================
    juce::ValueTree SampleManager::toValueTree() const
    {
        juce::ValueTree tree (padsId);
        const juce::ScopedLock sl (lock);
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& info = pads[(size_t) p];
            if (info.status == Status::empty) continue;

            juce::ValueTree pad (kPadId);
            pad.setProperty (kIndexId, p, nullptr);
            pad.setProperty (kPathId, info.file.getFullPathName(), nullptr);
            tree.appendChild (pad, nullptr);
        }
        return tree;
    }

    void SampleManager::restoreFromValueTree (const juce::ValueTree& tree)
    {
        if (! juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::WeakReference<SampleManager> weak (this);
            juce::MessageManager::callAsync ([weak, copy = tree.createCopy()]
            {
                if (auto* self = weak.get()) self->restoreFromValueTree (copy);
            });
            return;
        }

        std::array<juce::String, kNumPads> paths;
        for (const auto& pad : tree)
        {
            const int index = (int) pad.getProperty (kIndexId, -1);
            if (juce::isPositiveAndBelow (index, kNumPads))
                paths[(size_t) index] = pad.getProperty (kPathId).toString();
        }

        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& path = paths[(size_t) p];
            if (path.isEmpty() || ! juce::File::isAbsolutePath (path))
            {
                if (getInfo (p).status != Status::empty) clearPad (p);
            }
            else
            {
                loadFile (p, juce::File (path));
            }
        }
    }
}
