#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "UI/Controls.h"
#include "Core/MacroRecipes.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        MacroEditorPanel  (A12 part 4)
        ----------------
            MACRO EDITOR  [1][2][3][4][5][6][7][8]
            name ......  [HOLD TO HEAR]  [RESET][RESET ALL][SAVE][LOAD]
            [MUTE][LPF][HPF][BPF]       LPF - LOW PASS FILTER   [IN THIS MACRO]
            [CRSH][RATE][DRV][TAPE]     CUTOFF  [SET]   (MILD)  (HARD)
            ...                          RES     [LEAVE] ...
        Lit effect = in the macro. Click an effect to edit it.
    */
    class MacroEditorPanel final : public juce::Component
    {
    public:
        explicit MacroEditorPanel (TwistedBreaksProcessor&);
        ~MacroEditorPanel() override;

        void setMacro (int m);
        int  getMacro() const noexcept { return macro; }
        void setFx (int f);
        int  getFx() const noexcept { return fxSel; }
        void refresh();

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

        juce::Rectangle<float> fxChip (int f) const;

    private:
        void rebuild();
        void hear (bool on);

        TwistedBreaksProcessor& processor;
        MacroRecipes& recipes;
        int macro = 0, fxSel = 0;
        uint32_t shownVersion = 0xffffffff;
        bool hearing = false;

        std::array<std::unique_ptr<PushKey>, MacroRecipes::kMacros> macroKeys;
        juce::TextEditor nameBox;
        PushKey hearKey   { "HOLD TO HEAR", "Holds this macro on while the mouse is down (or toggles it with LATCH)", juce::Colour (0xff7dff5a) };
        PushKey resetKey  { "RESET", "This macro back to its factory recipe" };
        PushKey resetAll  { "RESET ALL", "Every macro back to the factory" };
        PushKey saveKey   { "SAVE", "Save this macro as a .tbmacro file to share" };
        PushKey loadKey   { "LOAD", "Load a .tbmacro file into this macro" };
        PushKey useKey    { "IN THIS MACRO", "Whether this macro switches this effect on", juce::Colour (0xffffa928) };
        std::array<PushKey, 3> setKeys { PushKey { "SET", "Set this setting (MILD .. HARD), or leave it as it is" },
                                         PushKey { "SET", "Set this setting (MILD .. HARD), or leave it as it is" },
                                         PushKey { "SET", "Set this setting (MILD .. HARD), or leave it as it is" } };
        std::array<juce::Slider, 3> mildKnobs, hardKnobs;
        std::unique_ptr<juce::FileChooser> chooser;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MacroEditorPanel)
    };
}
