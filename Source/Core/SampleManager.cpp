#include "Core/SampleManager.h"
#include "Engine/AudioEngine.h"
#include "Engine/TransientDetector.h"

namespace tb
{
    const juce::Identifier SampleManager::padsId { "PADS" };

    namespace
    {
        const juce::Identifier kPadId      { "PAD" };
        const juce::Identifier kIndexId    { "index" };
        const juce::Identifier kPathId     { "path" };
        const juce::Identifier kSlicesId   { "slices" };
        const juce::Identifier kDetectedId { "detected" };
        const juce::Identifier kSensId     { "sensitivity" };
        const juce::Identifier kCountId    { "sliceCount" };

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
        followLeader.fill (-1);
        startTimerHz (20);
    }

    SampleManager::~SampleManager()
    {
        stopTimer();
        loader.removeAllJobs (true, 10000);   // let any running decode / analysis finish
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

    void SampleManager::modify (int pad, const std::function<void (PadInfo&)>& change, bool slicesChanged)
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;
        auto info = getInfo (pad);
        change (info);
        if (slicesChanged) sendSlices (pad, info.slices);
        setInfo (pad, std::move (info));
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
        {
            if (p.status == Status::loading || p.slicing) return true;
            for (const auto& l : p.layers) if (l.status == Status::loading) return true;
        }
        return false;
    }

    //==========================================================================
    SampleBuffer::Ptr SampleManager::findLoaded (const juce::File& file) const
    {
        // Same file, unchanged on disk since it was decoded -> share it.
        const auto modTime = file.getLastModificationTime();
        const auto size    = file.getSize();
        for (auto* b : keepAlive)
            if (b->file == file && b->fileModTime == modTime && b->fileSize == size)
                return b;
        return nullptr;
    }

    void SampleManager::loadFile (int pad, const juce::File& file)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        hasRestoreSlices[(size_t) pad] = false;
        startLoad (pad, file, nullptr);
    }

    void SampleManager::startLoad (int pad, const juce::File& file, const SliceTable* keepSlices)
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;

        const auto gen = ++generation[(size_t) pad];
        followLeader[(size_t) pad] = -1;

        // Already decoded (another pad has it): share instantly.
        if (auto shared = findLoaded (file))
        {
            installBuffer (pad, shared, keepSlices);
            hasRestoreSlices[(size_t) pad] = false;
            return;
        }

        PadInfo info = getInfo (pad);
        info.status = Status::loading;
        info.name   = file.getFileNameWithoutExtension();
        info.file   = file;       // keep playing the old sample until the new one is ready
        setInfo (pad, std::move (info));

        // Another pad is decoding this very file right now: wait for it.
        for (int q = 0; q < kNumPads; ++q)
        {
            if (q == pad) continue;
            const auto other = getInfo (q);
            if (other.status == Status::loading && other.file == file && followLeader[(size_t) q] < 0)
            {
                followLeader[(size_t) pad] = q;
                followGen[(size_t) pad]    = gen;
                return;
            }
        }

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
        ++sliceGeneration[(size_t) pad];
        followLeader[(size_t) pad] = -1;
        hasRestoreSlices[(size_t) pad] = false;
        const bool hadLayers = getInfo (pad).layerCount() > 0;
        for (auto& g : layerGen[(size_t) pad]) ++g;
        setInfo (pad, {});
        sendSample (pad, nullptr);
        sendSlices (pad, {});
        if (hadLayers) for (int l = 0; l < PadInfo::kLayers; ++l) sendLayer (pad, l, nullptr);
    }

    void SampleManager::installBuffer (int pad, const SampleBuffer::Ptr& buffer, const SliceTable* keepSlices)
    {
        PadInfo info = getInfo (pad);
        info.status   = Status::ready;
        info.buffer   = buffer;
        info.name     = buffer->name;
        info.file     = buffer->file;
        info.slicing  = false;
        info.slices   = {};
        info.detected = {};

        if (keepSlices != nullptr)
        {
            // Restored / spread markers are only valid if they fit this audio.
            auto t = *keepSlices;
            t.normalise (buffer->getNumSamples());
            info.slices = t;
            info.detected = t;
            if (hasRestoreSlices[(size_t) pad] && ! restoreDetected[(size_t) pad].isEmpty())
            {
                auto d = restoreDetected[(size_t) pad];
                d.normalise (buffer->getNumSamples());
                info.detected = d;
            }
        }

        if (! keepAlive.contains (buffer.get())) keepAlive.add (buffer.get());
        sendSample (pad, buffer);
        sendSlices (pad, info.slices);
        setInfo (pad, std::move (info));
    }

    void SampleManager::install (int pad, uint32_t gen, const SampleLoader::Result& result)
    {
        if (gen != generation[(size_t) pad]) return;   // superseded by a newer request

        const SliceTable* keep = hasRestoreSlices[(size_t) pad] ? &restoreSlices[(size_t) pad] : nullptr;

        if (result.status == SampleLoader::Status::ok)
        {
            installBuffer (pad, result.buffer, keep);
        }
        else
        {
            PadInfo info = getInfo (pad);
            info.status = toStatus (result.status);
            info.buffer = nullptr;       // pad goes silent and shows the problem
            if (keep != nullptr) info.slices = *keep;   // keep markers so re-saving doesn't lose them
            sendSample (pad, nullptr);
            sendSlices (pad, {});
            setInfo (pad, std::move (info));
        }
        hasRestoreSlices[(size_t) pad] = false;

        // Pads that were waiting on this decode get the same result.
        for (int p = 0; p < kNumPads; ++p)
        {
            if (followLeader[(size_t) p] != pad || followGen[(size_t) p] != generation[(size_t) p]) continue;
            followLeader[(size_t) p] = -1;
            install (p, generation[(size_t) p], result);
        }
    }

    //==========================================================================
    void SampleManager::requestBuffer (const juce::File& file, std::function<void (SampleBuffer::Ptr)> done)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (file == juce::File()) { done (nullptr); return; }
        if (auto shared = findLoaded (file)) { done (shared); return; }

        juce::WeakReference<SampleManager> weak (this);
        auto* fm = &formats;
        loader.addJob ([weak, file, fm, done]
        {
            auto result = SampleLoader::loadFile (file, *fm);
            juce::MessageManager::callAsync ([weak, result, done]
            {
                auto* self = weak.get();
                if (self == nullptr) return;
                if (result.status != SampleLoader::Status::ok) { done (nullptr); return; }
                // Someone may have decoded the same file meanwhile: share theirs.
                auto buf = self->findLoaded (result.buffer->file);
                if (buf == nullptr) { buf = result.buffer; self->keepAlive.add (buf.get()); }
                done (buf);
            });
        });
    }

    void SampleManager::requestSlices (const SampleBuffer::Ptr& buf, float sensitivity, int count, std::function<void (SliceTable)> done)
    {
        if (buf == nullptr) { done ({}); return; }
        juce::WeakReference<SampleManager> weak (this);
        loader.addJob ([weak, buf, sensitivity, count, done]
        {
            const auto table = TransientDetector::detect (*buf, sensitivity, count);
            juce::MessageManager::callAsync ([weak, table, done] { if (weak.get() != nullptr) done (table); });
        });
    }

    void SampleManager::sendBank (int slot, const SampleBuffer::Ptr& buf, const SliceTable& slices)
    {
        retain (buf);
        Pending p;
        p.pad = AudioEngine::kBankCommandBase + slot;
        p.setSample = true; p.buffer = buf;
        p.setSlices = true; p.slices = slices;
        pending.push_back (p);
        flushPending();
    }

    void SampleManager::retain (const SampleBuffer::Ptr& buf)
    {
        if (buf != nullptr && ! keepAlive.contains (buf.get())) keepAlive.add (buf.get());
    }

    void SampleManager::adoptPads (const juce::ValueTree& tree, const std::array<SampleBuffer::Ptr, kNumPads>& buffers)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        for (int p = 0; p < kNumPads; ++p)
        {
            ++generation[(size_t) p];               // cancel loads in flight for this pad
            ++sliceGeneration[(size_t) p];
            followLeader[(size_t) p] = -1;
            hasRestoreSlices[(size_t) p] = false;

            juce::ValueTree e;
            for (const auto& c : tree)
                if ((int) c.getProperty (kIndexId, -1) == p) e = c;

            const auto path = e.isValid() ? e.getProperty (kPathId).toString() : juce::String();
            PadInfo info;
            info.layers = getInfo (p).layers;          // layers follow separately (restoreLayers)
            if (path.isNotEmpty())
            {
                info.file = juce::File (path);
                info.name = info.file.getFileNameWithoutExtension();
                info.sensitivity = (float) e.getProperty (kSensId, 0.5f);
                info.sliceCount  = juce::jlimit (1, SliceTable::kMax, (int) e.getProperty (kCountId, 16));
                info.slices   = SliceTable::fromString (e.getProperty (kSlicesId).toString());
                info.detected = SliceTable::fromString (e.getProperty (kDetectedId).toString());
                const auto& buf = buffers[(size_t) p];
                if (buf != nullptr)
                {
                    info.status = Status::ready;
                    info.buffer = buf;
                    info.name   = buf->name;
                    info.slices.normalise (buf->getNumSamples());
                    info.detected.normalise (buf->getNumSamples());
                    retain (buf);
                }
                else
                {
                    info.status = Status::missing;
                    restoreSlices[(size_t) p] = info.slices;     // kept for a later relocate
                    restoreDetected[(size_t) p] = info.detected;
                }
            }
            // The engine already switched to these at the boundary; sending
            // them again is a no-op there, and keeps the queue consistent.
            sendSample (p, info.buffer);
            sendSlices (p, info.buffer != nullptr ? info.slices : SliceTable{});
            setInfo (p, std::move (info));
        }
        if (onStateChanged) onStateChanged();
    }

    int SampleManager::countMissing() const
    {
        int n = 0;
        for (int p = 0; p < kNumPads; ++p) if (getInfo (p).status == Status::missing) ++n;
        return n;
    }

    int SampleManager::relocateMissing (const juce::File& folder)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        int found = 0;
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto info = getInfo (p);
            if (info.status != Status::missing) continue;
            const auto name = info.file.getFileName();
            juce::File hit;
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, name, juce::File::findFiles))
            {
                hit = entry.getFile();
                break;
            }
            if (hit == juce::File()) continue;

            ++found;
            if (! info.slices.isEmpty()) restoreSlices[(size_t) p] = info.slices;
            hasRestoreSlices[(size_t) p] = ! restoreSlices[(size_t) p].isEmpty();
            startLoad (p, hit, hasRestoreSlices[(size_t) p] ? &restoreSlices[(size_t) p] : nullptr);
        }
        return found;
    }

    //==========================================================================
    void SampleManager::autoSlice (int pad)
    {
        const auto info = getInfo (pad);
        if (info.buffer == nullptr || info.slicing) return;

        const auto gen  = ++sliceGeneration[(size_t) pad];
        const auto buf  = info.buffer;
        const auto sens = info.sensitivity;
        const auto max  = info.sliceCount;
        modify (pad, [] (PadInfo& i) { i.slicing = true; }, false);

        juce::WeakReference<SampleManager> weak (this);
        loader.addJob ([weak, pad, gen, buf, sens, max]
        {
            const auto table = TransientDetector::detect (*buf, sens, max);
            juce::MessageManager::callAsync ([weak, pad, gen, buf, table]
            {
                auto* self = weak.get();
                if (self == nullptr) return;
                const bool current = gen == self->sliceGeneration[(size_t) pad]
                                  && self->getInfo (pad).buffer == buf;
                self->modify (pad, [&] (PadInfo& i)
                {
                    i.slicing = false;
                    if (current) { i.slices = table; i.detected = table; }
                }, current);
            });
        });
    }

    void SampleManager::equalSlice (int pad)
    {
        const auto info = getInfo (pad);
        if (info.buffer == nullptr) return;
        const auto t = SliceTable::equal (info.buffer->getNumSamples(), info.sliceCount);
        modify (pad, [&] (PadInfo& i) { i.slices = t; i.detected = t; }, true);
    }

    void SampleManager::resetSlices (int pad)
    {
        modify (pad, [] (PadInfo& i) { i.slices = i.detected; }, true);
    }

    void SampleManager::clearSlices (int pad)
    {
        modify (pad, [] (PadInfo& i) { i.slices = {}; i.detected = {}; }, true);
    }

    void SampleManager::setSlices (int pad, SliceTable t)
    {
        const auto info = getInfo (pad);
        if (info.buffer == nullptr) return;
        t.normalise (info.buffer->getNumSamples());
        if (t == info.slices) return;
        modify (pad, [&] (PadInfo& i) { i.slices = t; }, true);
    }

    bool SampleManager::addMarker (int pad, int position)
    {
        auto t = getInfo (pad).slices;
        if (t.count >= SliceTable::kMax) return false;
        if (t.count == 0) t.starts[(size_t) t.count++] = 0;       // first marker on an unsliced pad also marks the start
        t.starts[(size_t) t.count++] = position;
        setSlices (pad, t);
        return true;
    }

    void SampleManager::moveMarker (int pad, int index, int position)
    {
        const auto info = getInfo (pad);
        auto t = info.slices;
        if (info.buffer == nullptr || ! juce::isPositiveAndBelow (index, t.count)) return;

        // A marker can't cross its neighbours.
        const int lo = index > 0 ? t.starts[(size_t) index - 1] + SliceTable::kMinGap : 0;
        const int hi = index + 1 < t.count ? t.starts[(size_t) index + 1] - SliceTable::kMinGap
                                           : info.buffer->getNumSamples() - SliceTable::kMinGap;
        if (hi < lo) return;
        t.starts[(size_t) index] = juce::jlimit (lo, hi, position);
        setSlices (pad, t);
    }

    void SampleManager::deleteMarker (int pad, int index)
    {
        auto t = getInfo (pad).slices;
        if (! juce::isPositiveAndBelow (index, t.count)) return;
        for (int i = index; i + 1 < t.count; ++i) t.starts[(size_t) i] = t.starts[(size_t) i + 1];
        --t.count;
        modify (pad, [&] (PadInfo& i) { i.slices = t; }, true);
    }

    void SampleManager::setSensitivity (int pad, float s)
    {
        modify (pad, [s] (PadInfo& i) { i.sensitivity = juce::jlimit (0.0f, 1.0f, s); }, false);
    }

    void SampleManager::setSliceCount (int pad, int n)
    {
        modify (pad, [n] (PadInfo& i) { i.sliceCount = juce::jlimit (1, SliceTable::kMax, n); }, false);
    }

    void SampleManager::assignPad (int pad, const PadInfo& src)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;
        if (src.status == Status::empty || (src.buffer == nullptr && src.file == juce::File()))
        {
            if (getInfo (pad).status != Status::empty) clearPad (pad);
            return;
        }
        ++generation[(size_t) pad];              // cancel anything loading there
        ++sliceGeneration[(size_t) pad];
        followLeader[(size_t) pad] = -1;
        hasRestoreSlices[(size_t) pad] = false;

        if (src.buffer != nullptr)
        {
            installBuffer (pad, src.buffer, &src.slices);
            modify (pad, [&] (PadInfo& p)
            {
                p.detected    = src.detected;
                p.sensitivity = src.sensitivity;
                p.sliceCount  = src.sliceCount;
            }, false);
        }
        else
        {
            modify (pad, [&] (PadInfo& p) { p.sensitivity = src.sensitivity; p.sliceCount = src.sliceCount; }, false);
            restoreSlices[(size_t) pad]    = src.slices;
            restoreDetected[(size_t) pad]  = src.detected;
            hasRestoreSlices[(size_t) pad] = true;
            startLoad (pad, src.file, &restoreSlices[(size_t) pad]);
        }

        // Layers: shared buffers as they are, files reloaded, the rest cleared.
        for (int l = 0; l < PadInfo::kLayers; ++l)
        {
            const auto& sl = src.layers[(size_t) l];
            if (sl.buffer != nullptr)
            {
                ++layerGen[(size_t) pad][(size_t) l];
                modify (pad, [&] (PadInfo& p) { p.layers[(size_t) l] = sl; }, false);
                sendLayer (pad, l, sl.buffer);
            }
            else if (sl.status != Status::empty && sl.file != juce::File()) loadLayer (pad, l, sl.file);
            else clearLayer (pad, l);
        }
    }

    void SampleManager::repointFile (int pad, const juce::File& f)
    {
        modify (pad, [&f] (PadInfo& p) { if (p.status != Status::empty) p.file = f; }, false);
    }

    void SampleManager::preview (const SampleBuffer::Ptr& buffer)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        retain (buffer);
        sendSample (AudioEngine::kPreviewCommand, buffer);
    }

    int SampleManager::spreadToPads (int source)
    {
        const auto src = getInfo (source);
        if (src.buffer == nullptr || src.slices.isEmpty()) return 0;

        int filled = 0;
        const int sceneEnd = (sceneOf (source) + 1) * kPadsPerScene;          // SPREAD stays inside its scene
        for (int i = 0; i < src.slices.count && source + i < sceneEnd; ++i)
        {
            const int target = source + i;
            if (target != source)
            {
                ++generation[(size_t) target];        // cancel anything loading there
                followLeader[(size_t) target] = -1;
                hasRestoreSlices[(size_t) target] = false;
                installBuffer (target, src.buffer, &src.slices);
                modify (target, [&] (PadInfo& p)
                {
                    p.detected = src.detected;
                    p.sensitivity = src.sensitivity;
                    p.sliceCount = src.sliceCount;
                }, false);
            }
            ++filled;
        }
        return filled;
    }

    //==========================================================================
    void SampleManager::sendSample (int pad, const SampleBuffer::Ptr& buffer)
    {
        Pending p; p.pad = pad; p.setSample = true; p.buffer = buffer;
        pending.push_back (std::move (p));
        flushPending();
    }

    void SampleManager::sendSlices (int pad, const SliceTable& t)
    {
        Pending p; p.pad = pad; p.setSlices = true; p.slices = t;
        pending.push_back (std::move (p));
        flushPending();
    }

    void SampleManager::flushPending()
    {
        size_t sent = 0;
        for (; sent < pending.size(); ++sent)
        {
            auto& item = pending[sent];
            AudioEngine::PadCommand cmd;
            cmd.pad       = item.pad;
            cmd.setSample = item.setSample;
            cmd.setSlices = item.setSlices;
            cmd.slices    = item.slices;
            cmd.buffer    = item.buffer.get();
            if (cmd.buffer != nullptr) cmd.buffer->incReferenceCount();      // reference handed to the engine

            if (! engine.postCommand (cmd))
            {
                if (cmd.buffer != nullptr) cmd.buffer->decReferenceCountWithoutDeleting();
                break;                                                       // queue full: retry on the timer
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
            if (! info.slices.isEmpty())   pad.setProperty (kSlicesId,   info.slices.toString(), nullptr);
            if (! info.detected.isEmpty()) pad.setProperty (kDetectedId, info.detected.toString(), nullptr);
            pad.setProperty (kSensId,  info.sensitivity, nullptr);
            pad.setProperty (kCountId, info.sliceCount, nullptr);
            for (int l = 0; l < PadInfo::kLayers; ++l)
                if (info.layers[(size_t) l].status != Status::empty)
                {
                    juce::ValueTree n ("LAYER");
                    n.setProperty (kIndexId, l, nullptr);
                    n.setProperty (kPathId, info.layers[(size_t) l].file.getFullPathName(), nullptr);
                    pad.appendChild (n, nullptr);
                }
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

        std::array<juce::ValueTree, kNumPads> entries;
        for (const auto& pad : tree)
        {
            const int index = (int) pad.getProperty (kIndexId, -1);
            if (juce::isPositiveAndBelow (index, kNumPads))
                entries[(size_t) index] = pad;
        }

        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& e = entries[(size_t) p];
            const auto path = e.isValid() ? e.getProperty (kPathId).toString() : juce::String();
            if (path.isEmpty() || ! juce::File::isAbsolutePath (path))
            {
                if (getInfo (p).status != Status::empty) clearPad (p);
                continue;
            }

            // Slicer settings first, then the sample; markers re-apply when it arrives.
            const auto detected = SliceTable::fromString (e.getProperty (kDetectedId).toString());
            modify (p, [&] (PadInfo& i)
            {
                i.sensitivity = (float) e.getProperty (kSensId, 0.5f);
                i.sliceCount  = juce::jlimit (1, SliceTable::kMax, (int) e.getProperty (kCountId, 16));
            }, false);

            restoreSlices[(size_t) p]    = SliceTable::fromString (e.getProperty (kSlicesId).toString());
            restoreDetected[(size_t) p]  = detected;
            hasRestoreSlices[(size_t) p] = true;
            startLoad (p, juce::File (path), &restoreSlices[(size_t) p]);
        }
        restoreLayers (tree);
    }

    //==========================================================================
    //  Layers (A11)
    //==========================================================================
    void SampleManager::sendLayer (int pad, int layer, const SampleBuffer::Ptr& buf)
    {
        if (buf != nullptr) retain (buf);
        sendSample (AudioEngine::kLayerCommandBase + pad * PadInfo::kLayers + layer, buf);
    }

    int SampleManager::addLayer (int pad, const juce::File& f)
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return -1;
        const auto info = getInfo (pad);
        for (int l = 0; l < PadInfo::kLayers; ++l)
            if (info.layers[(size_t) l].status == Status::empty) { loadLayer (pad, l, f); return l; }
        return -1;
    }

    void SampleManager::loadLayer (int pad, int layer, const juce::File& f)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (! juce::isPositiveAndBelow (pad, kNumPads) || ! juce::isPositiveAndBelow (layer, PadInfo::kLayers)) return;
        const auto gen = ++layerGen[(size_t) pad][(size_t) layer];
        modify (pad, [&] (PadInfo& i)
        {
            auto& l = i.layers[(size_t) layer];
            l.status = Status::loading;
            l.file = f;
            l.name = f.getFileNameWithoutExtension();
        }, false);
        juce::WeakReference<SampleManager> weak (this);
        requestBuffer (f, [weak, pad, layer, gen, f] (SampleBuffer::Ptr buf)
        {
            auto* self = weak.get();
            if (self == nullptr || self->layerGen[(size_t) pad][(size_t) layer] != gen) return;
            self->modify (pad, [&] (PadInfo& i)
            {
                auto& l = i.layers[(size_t) layer];
                l.status = buf != nullptr ? Status::ready : (f.existsAsFile() ? Status::unreadable : Status::missing);
                l.buffer = buf;
                if (buf != nullptr) l.name = buf->name;
            }, false);
            self->sendLayer (pad, layer, buf);
        });
    }

    void SampleManager::clearLayer (int pad, int layer)
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads) || ! juce::isPositiveAndBelow (layer, PadInfo::kLayers)) return;
        ++layerGen[(size_t) pad][(size_t) layer];
        if (getInfo (pad).layers[(size_t) layer].status == Status::empty) return;
        modify (pad, [layer] (PadInfo& i) { i.layers[(size_t) layer] = {}; }, false);
        sendLayer (pad, layer, nullptr);
    }

    void SampleManager::repointLayer (int pad, int layer, const juce::File& f)
    {
        if (! juce::isPositiveAndBelow (layer, PadInfo::kLayers)) return;
        modify (pad, [&] (PadInfo& i) { if (i.layers[(size_t) layer].status != Status::empty) i.layers[(size_t) layer].file = f; }, false);
    }

    void SampleManager::restoreLayers (const juce::ValueTree& tree)
    {
        if (! juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            juce::WeakReference<SampleManager> weak (this);
            juce::MessageManager::callAsync ([weak, copy = tree.createCopy()] { if (auto* self = weak.get()) self->restoreLayers (copy); });
            return;
        }
        for (int p = 0; p < kNumPads; ++p)
        {
            std::array<juce::String, PadInfo::kLayers> wanted;
            for (const auto& e : tree)
                if ((int) e.getProperty (kIndexId, -1) == p)
                    for (const auto& n : e)
                        if (n.hasType ("LAYER") && juce::isPositiveAndBelow ((int) n.getProperty (kIndexId, -1), PadInfo::kLayers))
                            wanted[(size_t) (int) n.getProperty (kIndexId)] = n.getProperty (kPathId).toString();
            const auto info = getInfo (p);
            for (int l = 0; l < PadInfo::kLayers; ++l)
            {
                const auto& have = info.layers[(size_t) l];
                if (wanted[(size_t) l].isEmpty()) { if (have.status != Status::empty) clearLayer (p, l); continue; }
                if (have.status != Status::empty && have.file.getFullPathName() == wanted[(size_t) l]) continue;
                loadLayer (p, l, juce::File (wanted[(size_t) l]));
            }
        }
    }
}
