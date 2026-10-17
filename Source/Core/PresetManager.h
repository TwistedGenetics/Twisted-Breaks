#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <map>

class TwistedBreaksProcessor;

namespace tb
{
    /*
        PresetManager (A11 part 3)
        --------------------------
        Kit / Pad / Sequence presets that carry their own audio.

        File: a zip (".tbkit", ".tbpad", ".tbseq") holding
            preset.xml              <TBPRESET format type name tags note bpm pads created ...>
                                      <CONTENT> the state tree </CONTENT>
            audio/<md5>/<name>.flac every sample the preset uses, 24-bit FLAC at its
                                    own rate, stored once however many pads use it
        Inside CONTENT every "path" property that pointed at audio is replaced by
        "@audio/<md5>/<name>.flac". Loading unpacks those into
        ~/Music/Twisted Breaks/Library/Audio/<md5>/<name>.flac (once) and points
        the pads there, so a preset opens on any Mac with nothing missing.

          KIT       every pad (samples, slices, settings), both sequencers, the
                    snapshots, the break bank, FX and mixer, master. Not the MIDI
                    map or controller settings. Lands like a snapshot recall:
                    decoded first (old sounds keep playing), switched NOW, or on the
                    next bar while Live plays.
          PAD       one pad: what Copy takes (PadOps::Content). Loads onto a pad.
          SEQUENCE  the patterns only: both sequencers + the sequencer settings.

        Everything that changes pads is one undo step (PadOps).
        Message thread only (the timer polls loads in flight).
    */
    class PresetManager final : private juce::Timer
    {
    public:
        enum class Type { kit = 0, pad = 1, sequence = 2 };
        static constexpr int kFormatVersion = 1;

        struct Meta
        {
            Type type = Type::kit;
            juce::String name, note;
            juce::StringArray tags;
            double bpm = 0.0;
            int pads = 0;                  // pads with audio
            juce::Time created;
            juce::File file;
            bool factory = false;
            bool favourite = false;
            int formatVersion = kFormatVersion;
        };

        explicit PresetManager (TwistedBreaksProcessor&);
        ~PresetManager() override;

        static const char* extensionFor (Type);
        static const char* typeName (Type);
        static bool typeFromFile (const juce::File&, Type&);

        // ---- Folders --------------------------------------------------------------
        static juce::File defaultRoot();                  // ~/Music/Twisted Breaks
        void setRootForTesting (const juce::File& r) { root = r; listDirty = true; }
        juce::File getRoot() const { return root; }
        juce::File presetsFolder (Type) const;            // Presets/Kits, Pads, Sequences
        juce::File factoryFolder() const;                 // Presets/Factory
        juce::File audioLibrary() const;                  // Library/Audio

        // ---- Save / load ----------------------------------------------------------
        juce::Result saveKit (const juce::File&, Meta);
        juce::Result savePad (int pad, const juce::File&, Meta);
        juce::Result saveSequence (const juce::File&, Meta);
        /** A path for a new preset of this type and name (no clash). */
        juce::File newFileFor (Type, const juce::String& name) const;

        /** Reads only preset.xml: for the browser. */
        static juce::Result readMeta (const juce::File&, Meta&);
        /** pad: target for a pad preset (-1 = the selected pad). */
        juce::Result load (const juce::File&, int pad = -1);
        bool isLoading() const noexcept { return loading; }

        /** Plays the preset's first sample (pad 1 of a kit, the pad of a pad preset). */
        juce::Result preview (const juce::File&);
        void stopPreview();

        // ---- Library ---------------------------------------------------------------
        juce::Result renamePreset (const juce::File&, const juce::String& newName, juce::File* renamed = nullptr);
        juce::Result removePreset (const juce::File&);          // to the Trash
        juce::Result importPreset (const juce::File&, juce::File* imported = nullptr);
        bool isFavourite (const juce::File&) const;
        void setFavourite (const juce::File&, bool);
        /** Copies every sample the set uses into the Library and points the pads,
            bank and snapshots at the copies. Returns how many files were copied. */
        int collectSamples();
        /** Makes the factory kits if the Factory folder has none. */
        void ensureFactoryContent();

        // ---- The browser list (shared by the window, Mikro and Push) ---------------
        enum class Filter { all = 0, kits, pads, sequences, favourites };
        enum class Sort { name = 0, date };
        void setFilter (Filter f)   { filter = f; listDirty = true; }
        void setSearch (const juce::String& s) { search = s; listDirty = true; }
        void setSort (Sort s)       { sort = s; listDirty = true; }
        Filter getFilter() const noexcept { return filter; }
        Sort   getSort() const noexcept   { return sort; }
        juce::String getSearch() const    { return search; }
        void rescan()               { listDirty = true; }
        const juce::Array<Meta>& getList();
        int  getCursor() const noexcept { return cursor; }
        void setCursor (int c);
        void moveCursor (int delta)     { setCursor (cursor + delta); }
        /** Loads the kit before / after the current one in the list (‹ ›). */
        juce::Result stepKit (int delta);
        uint32_t getVersion() const noexcept { return version; }

        // ---- The current kit ---------------------------------------------------------
        juce::String getKitName() const { return kitName; }
        void setKitName (const juce::String& n) { kitName = n; markClean(); ++version; }
        /** Project load: the kit name from the Live set (the pads are clean once they've loaded). */
        void restoreKitName (const juce::String& n, const juce::String& file) { kitName = n.isEmpty() ? juce::String ("INIT") : n; currentKit = juce::File (file); cleanPending = true; ++version; }
        juce::File getCurrentKitFile() const { return currentKit; }
        /** The pad a PAD preset lands on when none is given (the window / controller selection). */
        void setTargetPad (int p) noexcept { selectedPadForLoad = p; }
        bool isModified();
        void markClean();
        juce::String getStatus() const { return status; }

        /** The kit content (state minus controller settings); paths as they are. */
        juce::ValueTree buildKitContent() const;
        juce::ValueTree buildSequenceContent() const;

        /** Sends an audition buffer (tests: what preview would play). */
        juce::File lastPreviewFile() const { return previewFile; }

    private:
        void timerCallback() override;
        juce::Result writePreset (const juce::File&, const Meta&, juce::ValueTree content);
        struct Opened { Meta meta; juce::ValueTree content; };
        juce::Result open (const juce::File&, Opened&, bool unpackAudio);
        void applyKit (const Opened&);
        void applySequence (const Opened&);
        void loadFavourites();
        void saveFavourites() const;
        juce::String contentHash() const;
        void setStatus (const juce::String& s) { status = s; ++version; }
        juce::File libraryFileFor (const juce::String& entry) const;

        TwistedBreaksProcessor& processor;
        juce::File root;
        juce::AudioFormatManager formats;

        juce::String kitName { "INIT" };
        juce::File currentKit;
        bool cleanPending = true;
        int selectedPadForLoad = -1;
        std::map<juce::String, std::pair<juce::Time, Meta>> metaCache;
        juce::String cleanHash;
        double lastModifiedCheck = 0.0;
        bool modifiedCache = false;

        bool loading = false;
        juce::String status;
        juce::File previewFile;

        Filter filter = Filter::kits;
        Sort sort = Sort::name;
        juce::String search;
        juce::Array<Meta> list;
        bool listDirty = true;
        int cursor = 0;
        uint32_t version = 0;
        juce::StringArray favourites;
        bool favouritesLoaded = false;

        JUCE_DECLARE_WEAK_REFERENCEABLE (PresetManager)
    };
}
