#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/*
    Palette & typography for the TWISTED BREAKS skin.

    Visual brief: a Soviet-era nuclear plant control room left running for
    forty years. Oxidised blue-green enamel over steel, worn through to red
    primer and bare metal at the edges; rust blooms and drips under every
    rivet; yellowed bakelite label plates with engraved Cyrillic; amber
    incandescent lamps; a green-phosphor CRT; an AZ-5 style emergency button.

    Everything stays high-contrast enough to read on a dark stage.
*/
namespace tb::ui
{
    namespace colours
    {
        // Enamelled steel
        inline const juce::Colour steelDark   { 0xff1c2523 };
        inline const juce::Colour steel       { 0xff2f3d39 };
        inline const juce::Colour steelLight  { 0xff42524c };
        inline const juce::Colour steelEdge   { 0xff5d6c64 };
        inline const juce::Colour bareMetal   { 0xff8d8f86 };
        inline const juce::Colour primer      { 0xff6e3424 };
        inline const juce::Colour grime       { 0xff0e1211 };

        // Corrosion
        inline const juce::Colour rustDark    { 0xff4a2412 };
        inline const juce::Colour rust        { 0xff7c3d1a };
        inline const juce::Colour rustBright  { 0xffa55a24 };

        // Bakelite label plates
        inline const juce::Colour bakelite    { 0xffcfc3a0 };
        inline const juce::Colour bakeliteOld { 0xffa99a74 };
        inline const juce::Colour engraving   { 0xff1b1a16 };
        inline const juce::Colour plateBlack  { 0xff141413 };
        inline const juce::Colour plateText   { 0xffd9d2bd };

        // Lamps
        inline const juce::Colour amber       { 0xffffa928 };
        inline const juce::Colour amberDim    { 0xff4a3317 };
        inline const juce::Colour red         { 0xffff3a1c };
        inline const juce::Colour redDim      { 0xff3d1610 };
        inline const juce::Colour green       { 0xff7dff5a };
        inline const juce::Colour greenDim    { 0xff1d3517 };

        // Displays
        inline const juce::Colour vfd         { 0xffff8f2e };   // segment displays
        inline const juce::Colour vfdGhost    { 0x22ff8f2e };
        inline const juce::Colour phosphor    { 0xff73ff8f };   // CRT trace
        inline const juce::Colour crtGlass    { 0xff07130b };

        // Hazard
        inline const juce::Colour hazardYellow { 0xffd9a400 };
        inline const juce::Colour paperTag     { 0xffd8cb98 };
    }

    /** Cyrillic / UTF-8 literal helper (source files are UTF-8; MSVC builds with /utf-8). */
    inline juce::String cyr (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    /** Condensed industrial face; falls back to the system sans if absent. */
    inline juce::Font industrialFont (float height, bool bold = true)
    {
       #if JUCE_MAC
        const juce::String name = "DIN Condensed";
       #else
        const juce::String name = juce::Font::getDefaultSansSerifFontName();
       #endif
        auto f = juce::Font (juce::FontOptions (name, height, bold ? juce::Font::bold : juce::Font::plain));
       #if ! JUCE_MAC
        f.setHorizontalScale (0.86f);
       #endif
        return f;
    }

    /** Mono face for small technical readouts (serials, note names). */
    inline juce::Font monoFont (float height)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::bold));
    }
}
