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
        The 4 x 4 matrix of PadComponents laid out like a hardware pad
        controller (MPC, Maschine, Push, a Drum Rack): pad 1 bottom-left,
        pad 16 top-right (MIDI 36 = pad 1 by default). Changed in 0.16.1;
        before that pad 1 was top-left. It shows one scene
        at a time (A = pads 1-16, B = pads B01-B16, engine pads 16-31).

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

        /** The right-click COPY / PASTE / SWAP / CLEAR menu of a pad (A11). */
        void showPadMenu (int pad);

        /** Scenes: which 16 pads the tiles show (0 = A, 1 = B). */
        void setScene (int scene);
        int  getScene() const noexcept { return scene; }

    private:
        TwistedBreaksProcessor& processor;
        std::array<std::unique_ptr<PadComponent>, kPadsPerScene> pads;
        std::array<int, kPadsPerScene> shownNotes {};
        std::array<uint32_t, kPadsPerScene> shownVersions {};
        std::array<juce::String, kPadsPerScene> shownTags;
        int selectedPad = 0, scene = 0;
    };
}
