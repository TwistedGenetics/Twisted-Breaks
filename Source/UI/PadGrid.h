#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "UI/PadComponent.h"
#include "Core/EngineBridge.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        PadGrid
        -------
        The 4 x 4 matrix of PadComponents in reading order: pad 1 is top-left,
        pad 16 bottom-right (MIDI 36 = pad 1 by default).

        Polls the EngineBridge on the UI timer for lamp brightness and keeps
        each pad's MIDI note label in sync with the engine's note map.
    */
    class PadGrid final : public juce::Component
    {
    public:
        explicit PadGrid (TwistedBreaksProcessor&);

        void resized() override;

        /** Called from MainView's timer (~30 Hz). */
        void refresh();

        int  getSelectedPad() const noexcept { return selectedPad; }
        void setSelectedPad (int pad);

        std::function<void (int)> onSelectionChanged;

    private:
        TwistedBreaksProcessor& processor;
        std::array<std::unique_ptr<PadComponent>, kNumPads> pads;
        std::array<int, kNumPads> shownNotes {};
        std::array<uint32_t, kNumPads> shownVersions {};
        std::array<juce::String, kNumPads> shownTags;
        int selectedPad = 0;
    };
}
