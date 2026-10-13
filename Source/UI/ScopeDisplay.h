#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include "Core/EngineBridge.h"

namespace tb::ui
{
    /*
        ScopeDisplay
        ------------
        Green-phosphor CRT in a heavy steel bezel.

        Phase 1: a live oscilloscope of the master output (triggered on a
        rising zero crossing so drum hits sit still on the screen).
        Phase 2: the same screen becomes the WaveformEditor — waveform,
        start/end markers, slice markers and playback cursor — reusing this
        bezel/phosphor rendering.
    */
    class ScopeDisplay final : public juce::Component
    {
    public:
        explicit ScopeDisplay (EngineBridge& bridge);

        void refresh();
        void paint (juce::Graphics&) override;
        void resized() override;

        juce::Rectangle<float> screenArea() const;

    private:
        void rebuildLayers();

        EngineBridge& bridge;
        static constexpr int historySize = 2048;
        std::array<float, historySize> history {};
        int  writePos = 0;
        std::array<float, 1024> scratch {};
        bool hadSignal = false;

        juce::Image under, over;
    };
}
