#include "Core/PresetManager.h"
#include "Core/Parameters.h"
#include "Core/PadOps.h"
#include "PluginProcessor.h"
#include <set>

namespace tb
{
    namespace
    {
        const char* const kXmlEntry = "preset.xml";

        /** Parameters a kit doesn't keep: momentary keys and macro moves. */
        bool isMomentary (const juce::String& id)
        {
            return id == params::snapRecall || id == params::seqFill || id == params::mutGo || id == params::rsmpRec
                || id == params::masterPanic
                || (id.startsWith ("macro_") && id != params::macroAmount && id != params::macroGroup && id != params::macroLatch);
        }

        /** The parameters a sequence preset carries. */
        bool isSequenceParam (const juce::String& id)
        {
            if (id == params::seqLength || id == params::seqRes || id == params::seqQuant) return true;
            if (! id.startsWith ("pad")) return false;
            const auto suffix = id.fromFirstOccurrenceOf ("_", false, false);
            return suffix == params::padSeqLen || suffix == params::padStepsWhole || suffix == params::padSliceSlot
                || suffix == params::padSliceSeq || suffix == params::padLaunch;
        }

        juce::String typeTag (PresetManager::Type t)
        {
            return t == PresetManager::Type::kit ? "kit" : t == PresetManager::Type::pad ? "pad" : "sequence";
        }

        void forEachPath (juce::ValueTree n, const std::function<void (juce::ValueTree&)>& fn)
        {
            if (n.hasProperty ("path")) fn (n);
            for (auto c : n) forEachPath (c, fn);
        }

        /** 64-bit FNV-1a, as 16 hex digits: names audio in the Library (same audio, same name). */
        struct Hasher
        {
            uint64_t h = 1469598103934665603ull;
            void add (const void* data, size_t n) noexcept
            {
                auto* p = static_cast<const uint8_t*> (data);
                for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
            }
            juce::String hex() const { return juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16); }
        };

        juce::String hashOf (const juce::MemoryBlock& b) { Hasher x; x.add (b.getData(), b.getSize()); return x.hex(); }
        juce::String hashOf (const juce::String& s)      { Hasher x; x.add (s.toRawUTF8(), s.getNumBytesAsUTF8()); return x.hex(); }
        juce::String hashOfFile (const juce::File& f)
        {
            Hasher x;
            juce::FileInputStream in (f);
            if (! in.openedOk()) return {};
            juce::HeapBlock<char> buf (65536);
            for (;;)
            {
                const int n = in.read (buf.get(), 65536);
                if (n <= 0) break;
                x.add (buf.get(), (size_t) n);
            }
            return x.hex();
        }

        juce::String legalName (const juce::String& s)
        {
            auto n = juce::File::createLegalFileName (s.trim());
            return n.isEmpty() ? juce::String ("Untitled") : n;
        }
    }

    //==========================================================================
    PresetManager::PresetManager (TwistedBreaksProcessor& p) : processor (p), root (defaultRoot())
    {
        formats.registerBasicFormats();
        startTimer (250);
    }

    PresetManager::~PresetManager() { stopTimer(); }

    const char* PresetManager::extensionFor (Type t)
    {
        return t == Type::kit ? ".tbkit" : t == Type::pad ? ".tbpad" : ".tbseq";
    }

    const char* PresetManager::typeName (Type t)
    {
        return t == Type::kit ? "KIT" : t == Type::pad ? "PAD" : "SEQ";
    }

    bool PresetManager::typeFromFile (const juce::File& f, Type& t)
    {
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".tbkit") { t = Type::kit; return true; }
        if (ext == ".tbpad") { t = Type::pad; return true; }
        if (ext == ".tbseq") { t = Type::sequence; return true; }
        return false;
    }

    juce::File PresetManager::defaultRoot()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Twisted Breaks");
    }

    juce::File PresetManager::presetsFolder (Type t) const
    {
        return root.getChildFile ("Presets").getChildFile (t == Type::kit ? "Kits" : t == Type::pad ? "Pads" : "Sequences");
    }

    juce::File PresetManager::factoryFolder() const { return root.getChildFile ("Presets").getChildFile ("Factory"); }
    juce::File PresetManager::audioLibrary() const  { return root.getChildFile ("Library").getChildFile ("Audio"); }

    juce::File PresetManager::newFileFor (Type t, const juce::String& name) const
    {
        auto folder = presetsFolder (t);
        folder.createDirectory();
        return folder.getNonexistentChildFile (legalName (name), extensionFor (t), false);
    }

    juce::File PresetManager::libraryFileFor (const juce::String& entry) const
    {
        // "audio/<md5>/<name>.flac" -> Library/Audio/<md5>/<name>.flac
        return audioLibrary().getChildFile (entry.fromFirstOccurrenceOf ("audio/", false, false));
    }

    //==========================================================================
    //  Content
    //==========================================================================
    juce::ValueTree PresetManager::buildKitContent() const
    {
        juce::ValueTree kit ("KIT");
        juce::ValueTree ps ("PARAMS");
        for (auto* prm : processor.getParameters().processor.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                const auto id = r->getParameterID();
                if (isMomentary (id)) continue;
                juce::ValueTree v ("PARAM");
                v.setProperty ("id", id, nullptr);
                v.setProperty ("value", r->convertFrom0to1 (r->getValue()), nullptr);
                ps.appendChild (v, nullptr);
            }
        kit.appendChild (ps, nullptr);
        auto& self = const_cast<TwistedBreaksProcessor&> (processor);
        kit.appendChild (self.getSamples().toValueTree(), nullptr);
        kit.appendChild (self.getBridge().seq.toValueTree(), nullptr);
        kit.appendChild (self.getBridge().sliceSeq.toValueTree(), nullptr);
        kit.appendChild (self.getSnapshots().toValueTree(), nullptr);
        kit.appendChild (self.getBank().toValueTree(), nullptr);
        juce::ValueTree fx ("FX");
        fx.setProperty ("lockedMask", (juce::int64) self.getEngine().getFxLockedMask(), nullptr);
        fx.setProperty ("group", self.getEngine().getFxTargetGroup(), nullptr);
        kit.appendChild (fx, nullptr);
        return kit;
    }

    juce::ValueTree PresetManager::buildSequenceContent() const
    {
        auto& self = const_cast<TwistedBreaksProcessor&> (processor);
        juce::ValueTree t ("SEQUENCE");
        juce::ValueTree ps ("PARAMS");
        for (auto* prm : self.getParameters().processor.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                if (isSequenceParam (r->getParameterID()))
                {
                    juce::ValueTree v ("PARAM");
                    v.setProperty ("id", r->getParameterID(), nullptr);
                    v.setProperty ("value", r->convertFrom0to1 (r->getValue()), nullptr);
                    ps.appendChild (v, nullptr);
                }
        t.appendChild (ps, nullptr);
        t.appendChild (self.getBridge().seq.toValueTree(), nullptr);
        t.appendChild (self.getBridge().sliceSeq.toValueTree(), nullptr);
        return t;
    }

    //==========================================================================
    //  Writing
    //==========================================================================
    juce::Result PresetManager::writePreset (const juce::File& file, const Meta& meta, juce::ValueTree content)
    {
        // Audio already decoded is encoded from memory; anything else from its file.
        std::map<juce::String, SampleBuffer::Ptr> loaded;
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto info = processor.getSamples().getInfo (p);
            if (info.buffer != nullptr) loaded[info.file.getFullPathName()] = info.buffer;
        }
        for (int s = 0; s < BreakBank::kSlots; ++s)
        {
            const auto slot = processor.getBank().get (s);
            if (slot.buffer != nullptr) loaded[slot.file.getFullPathName()] = slot.buffer;
        }

        std::map<juce::String, juce::String> entryFor;         // original path -> zip entry ("" = couldn't read)
        std::map<juce::String, juce::MemoryBlock> audio;       // zip entry -> FLAC
        juce::FlacAudioFormat flac;
        int skipped = 0;

        auto encode = [&] (const juce::String& path) -> juce::String
        {
            juce::AudioBuffer<float> buf;
            double rate = 44100.0;
            if (auto it = loaded.find (path); it != loaded.end())
            {
                buf.makeCopyOf (it->second->audio);
                rate = it->second->sourceSampleRate;
            }
            else
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (path)));
                if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) 48000 * 60 * 10) return {};
                buf.setSize ((int) juce::jlimit (1u, 2u, reader->numChannels), (int) reader->lengthInSamples);
                reader->read (&buf, 0, buf.getNumSamples(), 0, true, buf.getNumChannels() > 1);
                rate = reader->sampleRate;
            }
            if (buf.getNumSamples() == 0 || buf.getNumChannels() > 8 || rate <= 0.0 || rate > 655350.0) return {};

            juce::MemoryBlock block;
            {
                auto* out = new juce::MemoryOutputStream (block, false);
                std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (out, rate, (unsigned int) buf.getNumChannels(), 24, {}, 5));
                if (w == nullptr) { delete out; return {}; }
                w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
            }
            const auto md5 = hashOf (block);
            const auto entry = "audio/" + md5 + "/" + legalName (juce::File (path).getFileNameWithoutExtension()) + ".flac";
            if (audio.find (entry) == audio.end()) audio[entry] = std::move (block);
            return entry;
        };

        forEachPath (content, [&] (juce::ValueTree& n)
        {
            const auto path = n.getProperty ("path").toString();
            if (path.isEmpty() || path.startsWithChar ('@') || ! juce::File::isAbsolutePath (path)) return;
            auto it = entryFor.find (path);
            if (it == entryFor.end()) it = entryFor.emplace (path, encode (path)).first;
            if (it->second.isNotEmpty()) n.setProperty ("path", "@" + it->second, nullptr);
            else ++skipped;                                  // missing now: kept as a path (MISSING on load)
        });

        juce::XmlElement xml ("TBPRESET");
        xml.setAttribute ("format", kFormatVersion);
        xml.setAttribute ("type", typeTag (meta.type));
        xml.setAttribute ("name", meta.name);
        xml.setAttribute ("tags", meta.tags.joinIntoString (","));
        xml.setAttribute ("note", meta.note);
        xml.setAttribute ("bpm", meta.bpm);
        xml.setAttribute ("pads", meta.pads);
        xml.setAttribute ("created", juce::String (juce::Time::getCurrentTime().toMilliseconds()));
        xml.setAttribute ("pluginVersion", JucePlugin_VersionString);
        auto* holder = xml.createNewChildElement ("CONTENT");
        if (auto cx = content.createXml()) holder->addChildElement (cx.release());

        juce::ZipFile::Builder zip;
        const auto now = juce::Time::getCurrentTime();
        zip.addEntry (new juce::MemoryInputStream (juce::MemoryBlock (xml.toString().toRawUTF8(), xml.toString().getNumBytesAsUTF8()), true),
                      9, kXmlEntry, now);
        for (auto& [entry, block] : audio)
            zip.addEntry (new juce::MemoryInputStream (block, false), 0, entry, now);

        file.getParentDirectory().createDirectory();
        juce::TemporaryFile tmp (file);
        {
            auto out = tmp.getFile().createOutputStream();
            if (out == nullptr || ! zip.writeToStream (*out, nullptr))
                return juce::Result::fail ("Couldn't write " + file.getFullPathName());
            out->flush();
        }
        if (! tmp.overwriteTargetFileWithTemporary())
            return juce::Result::fail ("Couldn't save " + file.getFullPathName());
        listDirty = true;
        setStatus ("SAVED " + meta.name.toUpperCase() + (skipped > 0 ? " (" + juce::String (skipped) + " MISSING NOT EMBEDDED)" : juce::String()));
        return juce::Result::ok();
    }

    juce::Result PresetManager::saveKit (const juce::File& f, Meta m)
    {
        m.type = Type::kit;
        m.bpm = processor.getBridge().transport.bpm.load();
        m.pads = 0;
        for (int p = 0; p < kNumPads; ++p)
            if (processor.getSamples().getInfo (p).status != SampleManager::Status::empty) ++m.pads;
        auto r = writePreset (f, m, buildKitContent());
        if (r.wasOk()) { kitName = m.name; currentKit = f; markClean(); }
        return r;
    }

    juce::Result PresetManager::savePad (int pad, const juce::File& f, Meta m)
    {
        m.type = Type::pad;
        m.pads = processor.getSamples().getInfo (pad).status != SampleManager::Status::empty ? 1 : 0;
        m.bpm = processor.getBridge().transport.bpm.load();
        juce::ValueTree t ("PADPRESET");
        t.appendChild (PadOps::toTree (processor.getPadOps().capture (pad, PadOps::all)), nullptr);
        return writePreset (f, m, t);
    }

    juce::Result PresetManager::saveSequence (const juce::File& f, Meta m)
    {
        m.type = Type::sequence;
        m.bpm = processor.getBridge().transport.bpm.load();
        return writePreset (f, m, buildSequenceContent());
    }

    //==========================================================================
    //  Reading
    //==========================================================================
    static juce::Result parseXml (juce::ZipFile& zip, std::unique_ptr<juce::XmlElement>& out)
    {
        const int idx = zip.getIndexOfFileName (kXmlEntry);
        if (idx < 0) return juce::Result::fail ("Not a Twisted Breaks preset (no preset.xml)");
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (idx));
        if (in == nullptr) return juce::Result::fail ("The preset file is damaged");
        out = juce::parseXML (in->readEntireStreamAsString());
        if (out == nullptr || ! out->hasTagName ("TBPRESET")) return juce::Result::fail ("The preset file is damaged");
        return juce::Result::ok();
    }

    static void metaFrom (const juce::XmlElement& x, PresetManager::Meta& m)
    {
        const auto t = x.getStringAttribute ("type");
        m.type = t == "pad" ? PresetManager::Type::pad : t == "sequence" ? PresetManager::Type::sequence : PresetManager::Type::kit;
        m.name = x.getStringAttribute ("name");
        m.note = x.getStringAttribute ("note");
        m.tags = juce::StringArray::fromTokens (x.getStringAttribute ("tags"), ",", "");
        m.tags.trim(); m.tags.removeEmptyStrings();
        m.bpm  = x.getDoubleAttribute ("bpm");
        m.pads = x.getIntAttribute ("pads");
        m.created = juce::Time (x.getStringAttribute ("created").getLargeIntValue());
        m.formatVersion = x.getIntAttribute ("format", 1);
    }

    juce::Result PresetManager::readMeta (const juce::File& f, Meta& m)
    {
        if (! f.existsAsFile()) return juce::Result::fail ("File not found");
        juce::ZipFile zip (f);
        std::unique_ptr<juce::XmlElement> x;
        auto r = parseXml (zip, x);
        if (r.failed()) return r;
        metaFrom (*x, m);
        m.file = f;
        if (m.name.isEmpty()) m.name = f.getFileNameWithoutExtension();
        return juce::Result::ok();
    }

    juce::Result PresetManager::open (const juce::File& f, Opened& o, bool unpackAudio)
    {
        if (! f.existsAsFile()) return juce::Result::fail ("File not found: " + f.getFileName());
        juce::ZipFile zip (f);
        std::unique_ptr<juce::XmlElement> x;
        auto r = parseXml (zip, x);
        if (r.failed()) return r;
        metaFrom (*x, o.meta);
        o.meta.file = f;
        if (o.meta.name.isEmpty()) o.meta.name = f.getFileNameWithoutExtension();
        if (o.meta.formatVersion > kFormatVersion)
            return juce::Result::fail ("This preset was made by a newer Twisted Breaks (format " + juce::String (o.meta.formatVersion)
                                       + "). Update the plugin to open it.");
        auto* holder = x->getChildByName ("CONTENT");
        auto* cx = holder != nullptr ? holder->getFirstChildElement() : nullptr;
        if (cx == nullptr) return juce::Result::fail ("The preset file is damaged (no content)");
        o.content = juce::ValueTree::fromXml (*cx);
        const char* want = o.meta.type == Type::kit ? "KIT" : o.meta.type == Type::pad ? "PADPRESET" : "SEQUENCE";
        if (! o.content.hasType (want)) return juce::Result::fail ("The preset file is damaged (wrong content)");

        // Embedded audio -> the Library (once), paths point there.
        forEachPath (o.content, [&] (juce::ValueTree& n)
        {
            const auto path = n.getProperty ("path").toString();
            if (! path.startsWithChar ('@')) return;
            const auto entry = path.substring (1);
            const auto target = libraryFileFor (entry);
            if (unpackAudio && ! target.existsAsFile())
            {
                const int idx = zip.getIndexOfFileName (entry);
                if (idx >= 0)
                {
                    target.getParentDirectory().createDirectory();
                    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (idx));
                    juce::TemporaryFile tmp (target);
                    bool ok = false;
                    if (in != nullptr)
                        if (auto out = tmp.getFile().createOutputStream())
                        {
                            ok = out->writeFromInputStream (*in, -1) > 0;
                            out->flush();
                        }
                    if (ok) tmp.overwriteTargetFileWithTemporary();
                }
            }
            n.setProperty ("path", target.getFullPathName(), nullptr);     // not there: loads as MISSING, name kept
        });
        return juce::Result::ok();
    }

    juce::Result PresetManager::load (const juce::File& f, int pad)
    {
        if (loading) return juce::Result::fail ("A kit is still loading");
        Opened o;
        auto r = open (f, o, true);
        if (r.failed()) { setStatus (r.getErrorMessage().toUpperCase()); return r; }

        switch (o.meta.type)
        {
            case Type::kit:
                applyKit (o);
                break;
            case Type::pad:
            {
                if (pad < 0) pad = selectedPadForLoad >= 0 ? selectedPadForLoad : 0;
                auto c = PadOps::fromTree (o.content.getChildWithName ("PAD"));
                if (c.parts == 0) c.parts = PadOps::all;
                processor.getPadOps().load (pad, c, "LOAD PAD");
                setStatus ("LOADED " + o.meta.name.toUpperCase() + " ONTO PAD " + padLabel (pad));
                break;
            }
            case Type::sequence:
                applySequence (o);
                break;
        }
        return juce::Result::ok();
    }

    void PresetManager::applyKit (const Opened& o)
    {
        auto& ops = processor.getPadOps();
        ops.beginEdit();

        std::map<juce::String, float> stored;
        for (const auto& c : o.content.getChildWithName ("PARAMS"))
            stored[c.getProperty ("id").toString()] = (float) c.getProperty ("value");

        // What switches at the boundary goes in a snapshot-format tree; the
        // parameters a kit has no value for (newer ones) go back to default.
        juce::ValueTree snap ("SNAPSHOT");
        for (auto* prm : processor.getParameters().processor.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                const auto id = r->getParameterID();
                if (SnapshotManager::isExcluded (id)) continue;
                const auto it = stored.find (id);
                juce::ValueTree v ("PARAM");
                v.setProperty ("id", id, nullptr);
                v.setProperty ("value", it != stored.end() ? it->second : r->convertFrom0to1 (r->getDefaultValue()), nullptr);
                snap.appendChild (v, nullptr);
            }
        snap.appendChild (o.content.getChildWithName (SampleManager::padsId).createCopy(), nullptr);
        snap.appendChild (o.content.getChildWithName (SequencerData::seqId).createCopy(), nullptr);
        snap.appendChild (o.content.getChildWithName (SliceSeqData::sliceSeqId).createCopy(), nullptr);

        loading = true;
        setStatus ("LOADING " + o.meta.name.toUpperCase());
        const bool playing = processor.getBridge().transport.isPlaying.load();
        juce::WeakReference<PresetManager> weak (this);
        auto rest = o;
        processor.getSnapshots().recallTree (snap, playing ? params::switchBar : params::switchNow, [weak, rest, stored]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            auto& proc = self->processor;
            // The rest: master, bank and snapshot settings, the bank, the snapshots, FX locks.
            for (auto* prm : proc.getParameters().processor.getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                {
                    const auto id = r->getParameterID();
                    if (! SnapshotManager::isExcluded (id) || isMomentary (id)) continue;
                    const auto it = stored.find (id);
                    const float norm = it != stored.end() ? r->convertTo0to1 (it->second) : r->getDefaultValue();
                    if (std::abs (r->getValue() - norm) > 1.0e-7f)
                    {
                        r->beginChangeGesture();
                        r->setValueNotifyingHost (norm);
                        r->endChangeGesture();
                    }
                }
            proc.getBank().fromValueTree (rest.content.getChildWithName (BreakBank::bankId));
            proc.getSnapshots().fromValueTree (rest.content.getChildWithName (SnapshotManager::snapshotsId));
            if (auto fx = rest.content.getChildWithName ("FX"); fx.isValid())
            {
                proc.getEngine().setFxLockedMask ((uint32_t) (juce::int64) fx.getProperty ("lockedMask", (juce::int64) fx::kDefaultLockedMask));
                proc.getEngine().setFxTargetGroup ((int) fx.getProperty ("group", 0));
            }
            proc.markStateChanged();
            proc.getPadOps().endEdit ("LOAD KIT");
            self->kitName = rest.meta.name;
            self->currentKit = rest.meta.file;
            self->loading = false;
            self->markClean();
            self->setStatus ("LOADED " + rest.meta.name.toUpperCase());
        });
    }

    void PresetManager::applySequence (const Opened& o)
    {
        auto& ops = processor.getPadOps();
        ops.beginEdit();
        processor.getBridge().seq.fromValueTree (o.content.getChildWithName (SequencerData::seqId));
        processor.getBridge().sliceSeq.fromValueTree (o.content.getChildWithName (SliceSeqData::sliceSeqId));
        for (const auto& c : o.content.getChildWithName ("PARAMS"))
        {
            const auto id = c.getProperty ("id").toString();
            if (! isSequenceParam (id)) continue;
            if (auto* r = processor.getParameters().getParameter (id))
            {
                const float norm = r->convertTo0to1 ((float) c.getProperty ("value"));
                if (std::abs (r->getValue() - norm) > 1.0e-7f)
                {
                    r->beginChangeGesture();
                    r->setValueNotifyingHost (norm);
                    r->endChangeGesture();
                }
            }
        }
        processor.markStateChanged();
        ops.endEdit ("LOAD SEQUENCE", PadOps::steps | PadOps::settings);
        setStatus ("LOADED SEQUENCE " + o.meta.name.toUpperCase());
    }

    //==========================================================================
    juce::Result PresetManager::preview (const juce::File& f)
    {
        Opened o;
        auto r = open (f, o, true);
        if (r.failed()) return r;
        juce::String path;
        int best = 1 << 30;
        forEachPath (o.content, [&] (juce::ValueTree& n)
        {
            // A kit's lowest pad; a pad preset's sample.
            const auto parent = n.getParent();
            const bool isPad = parent.hasType (SampleManager::padsId) || n.hasType ("SAMPLE");
            if (! isPad) return;
            const int idx = n.hasType ("SAMPLE") ? 0 : (int) n.getProperty ("index", 999);
            if (idx < best && juce::File (n.getProperty ("path").toString()).existsAsFile())
            {
                best = idx;
                path = n.getProperty ("path").toString();
            }
        });
        if (path.isEmpty()) { stopPreview(); return juce::Result::fail ("Nothing to play"); }
        previewFile = juce::File (path);
        juce::WeakReference<PresetManager> weak (this);
        processor.getSamples().requestBuffer (previewFile, [weak] (SampleBuffer::Ptr buf)
        {
            if (auto* self = weak.get()) if (buf != nullptr) self->processor.getSamples().preview (buf);
        });
        return juce::Result::ok();
    }

    void PresetManager::stopPreview()
    {
        previewFile = juce::File();
        processor.getSamples().preview (nullptr);
    }

    //==========================================================================
    //  Library
    //==========================================================================
    juce::Result PresetManager::renamePreset (const juce::File& f, const juce::String& newName, juce::File* renamed)
    {
        Type t;
        if (! typeFromFile (f, t)) return juce::Result::fail ("Not a preset");
        juce::ZipFile zip (f);
        std::unique_ptr<juce::XmlElement> x;
        auto r = parseXml (zip, x);
        if (r.failed()) return r;
        x->setAttribute ("name", newName);

        juce::ZipFile::Builder b;
        const auto now = juce::Time::getCurrentTime();
        const auto xs = x->toString();
        b.addEntry (new juce::MemoryInputStream (juce::MemoryBlock (xs.toRawUTF8(), xs.getNumBytesAsUTF8()), true), 9, kXmlEntry, now);
        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto* e = zip.getEntry (i);
            if (e == nullptr || e->filename == kXmlEntry) continue;
            std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
            if (in == nullptr) return juce::Result::fail ("The preset file is damaged");
            juce::MemoryBlock mb;
            in->readIntoMemoryBlock (mb);
            b.addEntry (new juce::MemoryInputStream (mb, true), 0, e->filename, now);
        }
        const auto target = f.getParentDirectory().getChildFile (legalName (newName) + f.getFileExtension());
        const auto dest = target == f ? f : target.existsAsFile() ? f.getParentDirectory().getNonexistentChildFile (legalName (newName), f.getFileExtension(), false) : target;
        juce::TemporaryFile tmp (dest);
        {
            auto out = tmp.getFile().createOutputStream();
            if (out == nullptr || ! b.writeToStream (*out, nullptr)) return juce::Result::fail ("Couldn't write the preset");
            out->flush();
        }
        if (! tmp.overwriteTargetFileWithTemporary()) return juce::Result::fail ("Couldn't write the preset");
        const bool fav = isFavourite (f);
        if (dest != f)
        {
            f.deleteFile();
            if (fav) { setFavourite (f, false); setFavourite (dest, true); }
            if (currentKit == f) currentKit = dest;
        }
        if (renamed != nullptr) *renamed = dest;
        listDirty = true;
        ++version;
        return juce::Result::ok();
    }

    juce::Result PresetManager::removePreset (const juce::File& f)
    {
        if (! f.existsAsFile()) return juce::Result::fail ("File not found");
        if (! f.moveToTrash() && ! f.deleteFile()) return juce::Result::fail ("Couldn't move it to the Trash");
        setFavourite (f, false);
        listDirty = true;
        ++version;
        return juce::Result::ok();
    }

    juce::Result PresetManager::importPreset (const juce::File& f, juce::File* imported)
    {
        Meta m;
        auto r = readMeta (f, m);
        if (r.failed()) return r;
        if (m.formatVersion > kFormatVersion) return juce::Result::fail ("This preset was made by a newer Twisted Breaks");
        auto folder = presetsFolder (m.type);
        folder.createDirectory();
        auto dest = folder.getChildFile (f.getFileName());
        if (dest != f)
        {
            if (dest.existsAsFile()) dest = folder.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension(), false);
            if (! f.copyFileTo (dest)) return juce::Result::fail ("Couldn't copy the preset");
        }
        if (imported != nullptr) *imported = dest;
        listDirty = true;
        ++version;
        return juce::Result::ok();
    }

    void PresetManager::loadFavourites()
    {
        if (favouritesLoaded) return;
        favouritesLoaded = true;
        favourites.clear();
        const auto f = root.getChildFile ("Presets").getChildFile ("favourites.txt");
        if (f.existsAsFile()) favourites.addLines (f.loadFileAsString());
        favourites.removeEmptyStrings();
    }

    void PresetManager::saveFavourites() const
    {
        const auto f = root.getChildFile ("Presets").getChildFile ("favourites.txt");
        f.getParentDirectory().createDirectory();
        f.replaceWithText (favourites.joinIntoString ("\n"));
    }

    bool PresetManager::isFavourite (const juce::File& f) const
    {
        const_cast<PresetManager*> (this)->loadFavourites();
        return favourites.contains (f.getFullPathName());
    }

    void PresetManager::setFavourite (const juce::File& f, bool on)
    {
        loadFavourites();
        if (on == favourites.contains (f.getFullPathName())) return;
        if (on) favourites.add (f.getFullPathName());
        else    favourites.removeString (f.getFullPathName());
        saveFavourites();
        listDirty = true;
        ++version;
    }

    int PresetManager::collectSamples()
    {
        auto& sm = processor.getSamples();
        auto& bank = processor.getBank();
        const auto lib = audioLibrary();
        std::map<juce::String, juce::File> moved;

        auto collect = [&] (const juce::File& f) -> juce::File
        {
            if (f == juce::File() || ! f.existsAsFile() || f.isAChildOf (lib)) return {};
            const auto key = f.getFullPathName();
            if (auto it = moved.find (key); it != moved.end()) return it->second;
            const auto dest = lib.getChildFile (hashOfFile (f)).getChildFile (f.getFileName());
            if (! dest.existsAsFile())
            {
                dest.getParentDirectory().createDirectory();
                if (! f.copyFileTo (dest)) return {};
            }
            moved[key] = dest;
            return dest;
        };

        for (int p = 0; p < kNumPads; ++p)
            if (auto d = collect (sm.getInfo (p).file); d != juce::File()) sm.repointFile (p, d);
        for (int s = 0; s < BreakBank::kSlots; ++s)
            if (auto d = collect (bank.get (s).file); d != juce::File()) bank.repointFile (s, d);
        // Snapshots: their own paths (files no pad uses now included).
        auto snaps = processor.getSnapshots().toValueTree();
        forEachPath (snaps, [&] (juce::ValueTree& n) { collect (juce::File (n.getProperty ("path").toString())); });
        processor.getSnapshots().rewritePaths ([&moved] (const juce::String& p)
        {
            const auto it = moved.find (p);
            return it != moved.end() ? it->second.getFullPathName() : p;
        });
        processor.markStateChanged();
        setStatus ("COLLECTED " + juce::String ((int) moved.size()) + " FILES INTO THE LIBRARY");
        return (int) moved.size();
    }

    //==========================================================================
    //  Browser list
    //==========================================================================
    const juce::Array<PresetManager::Meta>& PresetManager::getList()
    {
        if (! listDirty) return list;
        listDirty = false;
        loadFavourites();
        list.clear();

        auto addFrom = [this] (const juce::File& folder, bool factory)
        {
            if (! folder.isDirectory()) return;
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.tbkit;*.tbpad;*.tbseq", juce::File::findFiles))
            {
                const auto f = entry.getFile();
                const auto key = f.getFullPathName();
                Meta m;
                auto it = metaCache.find (key);
                if (it != metaCache.end() && it->second.first == f.getLastModificationTime()) m = it->second.second;
                else
                {
                    if (readMeta (f, m).failed()) continue;
                    metaCache[key] = { f.getLastModificationTime(), m };
                }
                m.factory = factory;
                m.favourite = favourites.contains (key);
                switch (filter)
                {
                    case Filter::kits:       if (m.type != Type::kit) continue; break;
                    case Filter::pads:       if (m.type != Type::pad) continue; break;
                    case Filter::sequences:  if (m.type != Type::sequence) continue; break;
                    case Filter::favourites: if (! m.favourite) continue; break;
                    case Filter::all:        break;
                }
                if (search.trim().isNotEmpty())
                {
                    const auto hay = (m.name + " " + m.tags.joinIntoString (" ") + " " + m.note + " " + typeName (m.type)).toLowerCase();
                    bool all = true;
                    for (const auto& w : juce::StringArray::fromTokens (search.toLowerCase(), " ", ""))
                        if (w.isNotEmpty() && ! hay.contains (w)) all = false;
                    if (! all) continue;
                }
                list.add (m);
            }
        };
        addFrom (root.getChildFile ("Presets").getChildFile ("Kits"), false);
        addFrom (root.getChildFile ("Presets").getChildFile ("Pads"), false);
        addFrom (root.getChildFile ("Presets").getChildFile ("Sequences"), false);
        addFrom (factoryFolder(), true);

        struct ByName { static int compareElements (const Meta& a, const Meta& b)
        {
            if (a.factory != b.factory) return a.factory ? 1 : -1;            // yours first
            return a.name.compareNatural (b.name);
        } };
        struct ByDate { static int compareElements (const Meta& a, const Meta& b)
        {
            return a.created > b.created ? -1 : a.created < b.created ? 1 : a.name.compareNatural (b.name);
        } };
        if (sort == Sort::name) { ByName c; list.sort (c, true); }
        else                    { ByDate c; list.sort (c, true); }
        cursor = juce::jlimit (0, juce::jmax (0, list.size() - 1), cursor);
        ++version;
        return list;
    }

    void PresetManager::setCursor (int c)
    {
        const auto& l = getList();
        const int n = juce::jlimit (0, juce::jmax (0, l.size() - 1), c);
        if (n != cursor) { cursor = n; ++version; }
    }

    juce::Result PresetManager::stepKit (int delta)
    {
        // Within the kits on the list (yours + factory), from the current kit.
        const auto saved = filter;
        if (filter != Filter::kits && filter != Filter::favourites) { filter = Filter::kits; listDirty = true; }
        juce::Array<Meta> kits;
        for (const auto& m : getList()) if (m.type == Type::kit) kits.add (m);
        if (filter != saved) { filter = saved; listDirty = true; }
        if (kits.isEmpty()) return juce::Result::fail ("No kits");
        int at = -1;
        for (int i = 0; i < kits.size(); ++i) if (kits[i].file == currentKit) at = i;
        const int next = at < 0 ? (delta > 0 ? 0 : kits.size() - 1) : (at + delta + kits.size()) % kits.size();
        return load (kits[next].file);
    }

    //==========================================================================
    //  The current kit
    //==========================================================================
    juce::String PresetManager::contentHash() const
    {
        auto t = buildKitContent();
        t.removeChild (t.getChildWithName (SnapshotManager::snapshotsId), nullptr);   // ("active" moves on recall)
        return hashOf (t.toXmlString());
    }

    void PresetManager::markClean()
    {
        cleanHash = contentHash();
        modifiedCache = false;
        lastModifiedCheck = juce::Time::getMillisecondCounterHiRes();
    }

    bool PresetManager::isModified()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastModifiedCheck > 1000.0 && ! loading && ! processor.getSamples().isBusy())
        {
            lastModifiedCheck = now;
            if (cleanHash.isEmpty()) cleanHash = contentHash();
            modifiedCache = contentHash() != cleanHash;
        }
        return modifiedCache;
    }

    void PresetManager::timerCallback()
    {
        // A snapshot recalled while a kit was loading takes over: the kit load is dropped.
        if (loading && ! processor.getSnapshots().isRecallingTree())
        {
            loading = false;
            processor.getPadOps().endEdit ("LOAD KIT");
            setStatus ("KIT LOAD STOPPED: A SNAPSHOT WAS RECALLED");
        }
        // A project load: the name came with the state, the pads settle a moment later.
        if (cleanPending && ! loading && ! processor.getSamples().isBusy())
        {
            cleanPending = false;
            markClean();
            ++version;
        }
    }
}
