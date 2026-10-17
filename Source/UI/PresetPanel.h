#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "UI/Controls.h"
#include "Core/PresetManager.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        PresetPanel  (PRESETS view, A11)
        -----------
            [PRESETS] [KITS][PADS][SEQS][ALL][★]          [A-Z|NEW]
            [search ......................]   current kit name *
            ★ TYPE  NAME                      BPM  PADS  TAGS
            ...                                        (click: select + listen, double-click: load)
            [LOAD][SAVE][SAVE AS][RENAME][DELETE][FINDER]
            [IMPORT][COLLECT SAMPLES][LISTEN]      status

        Right-click a row: load / load onto pad / favourite / rename / delete / show.
        Drop a preset file on the panel to import and load it.
    */
    class PresetPanel final : public juce::Component,
                              public juce::FileDragAndDropTarget
    {
    public:
        explicit PresetPanel (TwistedBreaksProcessor&);

        void setSelectedPad (int pad);
        void refresh();     // ~30 Hz

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        bool keyPressed (const juce::KeyPress&) override;

        bool isInterestedInFileDrag (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray&, int x, int y) override;

        /** Save As: name, tags, note and what to save. */
        void showSaveDialog (PresetManager::Type initialType);

    private:
        juce::Rectangle<int> listArea() const;
        int  rowAt (juce::Point<float>) const;
        void loadRow (int row);
        void showRowMenu (int row);
        void renameRow (int row);
        void ensureVisible();
        void message (const juce::String&);
        void showExportMenu();

        TwistedBreaksProcessor& processor;
        PresetManager& pm;
        int selectedPad = 0;

        std::array<PushKey, 5> filterKeys { PushKey { "KITS", "Whole kits: every pad, both sequencers, snapshots, bank, FX and mixer" },
                                             PushKey { "PADS", "Pad presets: one pad's sound, settings and steps" },
                                             PushKey { "SEQS", "Sequence presets: the patterns only" },
                                             PushKey { "ALL", "Everything" },
                                             PushKey { "FAV", "Your favourites (click the star on a row)" } };
        PushKey sortKey    { "A-Z", "Sort by name, or newest first" };
        PushKey loadKey    { "LOAD", "Load the highlighted preset (a pad preset loads onto the selected pad)" };
        PushKey saveKey    { "SAVE", "Save the current kit (over its file; a new kit asks for a name)" };
        PushKey saveAsKey  { "SAVE AS", "Save a kit, the selected pad, or the sequences, with a name, tags and a note" };
        PushKey renameKey  { "RENAME", "Rename the highlighted preset" };
        PushKey deleteKey  { "DELETE", "Move the highlighted preset to the Trash", juce::Colour (0xffff3a1c) };
        PushKey finderKey  { "FINDER", "Show the highlighted preset in the Finder" };
        PushKey importKey  { "IMPORT", "Copy a preset file into your presets" };
        PushKey collectKey { "COLLECT SAMPLES", "Copy every sample this set uses into the Twisted Breaks Library and point the pads at the copies" };
        PushKey listenKey  { "LISTEN", "Play a preset's first sound when you click it" };
        PushKey exportKey  { "EXPORT", "Render the selected pad or the whole kit to WAV, or the sequence to MIDI (N bars at Live's tempo)", juce::Colour (0xff7dff5a) };
        PushKey dragKey    { "DRAG", "Drag the last export (WAV or MIDI) into Live" };
        juce::TextEditor searchBox;

        std::unique_ptr<juce::FileChooser> chooser;
        int scroll = 0;
        uint32_t shownVersion = 0xffffffff;
        juce::String shownKit;
        bool shownModified = false, shownLoading = false;
        juce::String note;                    // the bottom line (status / errors)
        double noteUntil = 0.0;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetPanel)
    };

    /*
        KitBar: the current kit in the header — [‹] NAME* [›]. Click the name for PRESETS.
    */
    class KitBar final : public juce::Component,
                         public juce::SettableTooltipClient
    {
    public:
        explicit KitBar (TwistedBreaksProcessor&);
        void refresh();
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        std::function<void()> onOpen;

    private:
        TwistedBreaksProcessor& processor;
        PushKey prevKey { "<", "Load the previous kit" }, nextKey { ">", "Load the next kit" };
        juce::String shown;
        bool shownMod = false, shownLoading = false;
    };
}
