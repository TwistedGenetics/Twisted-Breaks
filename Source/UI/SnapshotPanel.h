#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "UI/Controls.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        SnapshotPanel  (SNAPSHOTS view, Phase 8)
        -------------
            [SNAPSHOTS]  SWITCH [NOW][BEAT][BAR]            [STORE]
            [ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ][ 7 ][ 8 ]   click: recall, STORE then click: store
            [BREAK BANK] SWITCH [NOW][BEAT][BAR]  [FROM PAD 01] [ALL SLICE PADS FOLLOW]
            [AMEN][THINK][ ... ]                      click: make active; drop a file to load
            [FIND MISSING (2)]          MIDI PROGRAM 1-8 RECALLS SNAPSHOTS

        Right-click a snapshot: store / clear. Right-click a bank slot: load a
        file / copy from the selected pad / clear.
    */
    class SnapshotPanel final : public juce::Component,
                                public juce::FileDragAndDropTarget
    {
    public:
        explicit SnapshotPanel (TwistedBreaksProcessor&);

        void setSelectedPad (int pad);
        void refresh();     // ~30 Hz

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

        bool isInterestedInFileDrag (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray&, int x, int y) override;

        juce::Rectangle<float> snapBounds (int slot) const;
        juce::Rectangle<float> bankBounds (int slot) const;

    private:
        void setChoice (const char* paramId, int index);
        void chooseBankFile (int slot);

        TwistedBreaksProcessor& processor;
        int selectedPad = 0;

        std::array<PushKey, 3> snapAt { PushKey { "NOW", "Snapshots switch at once" }, PushKey { "BEAT", "Snapshots switch on the next beat" },
                                        PushKey { "BAR", "Snapshots switch on the next bar" } };
        std::array<PushKey, 3> bankAt { PushKey { "NOW", "The bank switches at once" }, PushKey { "BEAT", "The bank switches on the next beat" },
                                        PushKey { "BAR", "The bank switches on the next bar" } };
        PushKey storeKey   { "STORE", "Then click a slot to store everything into it", juce::Colour (0xffff3a1c) };
        PushKey fromPadKey { "FROM PAD 01", "Then click a bank slot to copy the selected pad's break (and markers) into it" };
        PushKey followKey  { "ALL SLICE PADS FOLLOW", "Every pad in SLICE mode (or SLICE SEQ) plays the active bank break" };
        PushKey findKey    { "FIND MISSING", "Pick a folder: every missing sample is looked for in it (and below) and reloaded" };

        std::unique_ptr<juce::FileChooser> chooser;
        uint32_t shownSnap = 0xffffffff, shownBank = 0xffffffff;
        int shownActiveBank = -1, shownMissing = -1;
        bool blink = false;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnapshotPanel)
    };
}
