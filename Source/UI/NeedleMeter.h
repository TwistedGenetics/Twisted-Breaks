#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Core/EngineBridge.h"

namespace tb::ui
{
    /*
        NeedleMeter
        -----------
        Master peak meter as a moving-coil panel meter: yellowed paper scale
        in dBFS, black needle with mechanical-feeling ballistics, red zone
        from 0 dB, plus two lamps:
            LIMIT — the master limiter is reducing gain
            CLIP  — a sample hit 0 dBFS (latched ~1.5 s)
    */
    class NeedleMeter final : public juce::Component
    {
    public:
        explicit NeedleMeter (EngineBridge& bridge);

        void refresh();   // ~30 Hz from MainView
        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        float angleFor (float db) const noexcept;
        void  rebuildFace();

        EngineBridge& bridge;
        float needleDb      = -60.0f;
        bool  limiting      = false;
        float overloadHold  = 0.0f;     // frames remaining
        uint32_t lastClip   = 0;
        bool  overloadShown = false;
        juce::Image face;
    };
}
