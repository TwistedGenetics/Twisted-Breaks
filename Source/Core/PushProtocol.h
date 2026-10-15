#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cstdint>

/*
    PushProtocol
    ------------
    Ableton Push 3 in User mode, as far as Twisted Breaks uses it. Push 3
    keeps Push 2's MIDI protocol (Ableton's "Push 2 MIDI and Display
    Interface Manual", github.com/Ableton/push-interface); the numbers below
    come from it. Push 3 only adds controls (the jog wheel, CC 70).

      Pads          notes 36..99, 36 = bottom left, row-major (36 + row * 8 + col)
      Buttons       CC, 127 pressed / 0 released (table below)
      Encoders      CC 71..78 (the 8 over the display), 79 master,
                    relative two's complement (1..63 clockwise, 127..64 anticlockwise);
                    touching one sends note 0..7 (8 = master) vel 127, letting go vel 0
      Touch strip   note 12 touch / release, position as pitch bend (0..16383)
      Pressure      polyphonic key pressure per pad (Push's Expression: Poly Aftertouch),
                    or channel pressure
      LEDs          note on (pads) / CC (buttons) back to Push on the User port,
                    channel 1, value = colour palette index (RGB controls) or
                    white brightness (white-only buttons)
      Sysex         F0 00 21 1D <dev> <model> <cmd> ... F7 (Push 2: dev 01, model 01)
                    0x0A set MIDI mode (0 Live, 1 User, 2 Dual): Push echoes it
                    on both ports, so the User port tells us when User mode is on
                    0x04 get LED palette entry i -> reply i rL rH gL gH bL bH wL wH

    In Live mode Push ignores non-sysex MIDI arriving on the User port, so
    sending lights there is harmless when it isn't in User mode.
*/
namespace tb::push
{
    // ---- Pads -------------------------------------------------------------------
    inline constexpr int kFirstPadNote = 36;
    inline constexpr int padNote (int row, int col) noexcept { return kFirstPadNote + row * 8 + col; }   // row 0 = bottom
    inline bool noteToCell (int note, int& row, int& col) noexcept
    {
        if (note < kFirstPadNote || note > kFirstPadNote + 63) return false;
        row = (note - kFirstPadNote) / 8;
        col = (note - kFirstPadNote) % 8;
        return true;
    }

    // ---- Buttons (CC) ----------------------------------------------------------------
    enum Button : int
    {
        tapTempo = 3, metronome = 9,
        lowerRow1 = 20,                   // 20..27, under the display
        master = 28, stopClip = 29, setup = 30, layout = 31, convert = 35,
        sceneBottom = 36, sceneTop = 43,  // the column right of the pads: 36 bottom .. 43 top
        left = 44, right = 45, up = 46, down = 47,
        select = 48, shift = 49, note = 50, session = 51,
        addDevice = 52, addTrack = 53, octaveDown = 54, octaveUp = 55,
        repeat = 56, accent = 57, scale = 58, user = 59, mute = 60, solo = 61,
        pageLeft = 62, pageRight = 63,
        jogWheel = 70,                    // Push 3: relative, like an encoder
        play = 85, record = 86, newButton = 87, duplicate = 88, automate = 89, fixedLength = 90,
        upperRow1 = 102,                  // 102..109, over the display
        device = 110, browse = 111, mix = 112, clip = 113, quantize = 116, doubleLoop = 117,
        deleteButton = 118, undo = 119
    };
    inline constexpr bool isLowerRow (int cc) noexcept { return cc >= lowerRow1 && cc < lowerRow1 + 8; }
    inline constexpr bool isUpperRow (int cc) noexcept { return cc >= upperRow1 && cc < upperRow1 + 8; }
    inline constexpr bool isSceneButton (int cc) noexcept { return cc >= sceneBottom && cc <= sceneTop; }
    /** Buttons with an RGB LED (palette index); the rest take a white brightness. */
    inline constexpr bool isRgbButton (int cc) noexcept
    {
        return isLowerRow (cc) || isUpperRow (cc) || isSceneButton (cc) || cc == mute || cc == solo || cc == stopClip;
    }

    // White-button brightness (white palette).
    inline constexpr int kWhiteOff = 0, kWhiteDim = 16, kWhiteHalf = 48, kWhiteFull = 127;

    // ---- Encoders, touch -----------------------------------------------------------------
    inline constexpr int kFirstEncoderCc = 71;     // 71..78
    inline constexpr int kMasterEncoderCc = 79;
    inline constexpr int kMasterTouchNote = 8;
    inline constexpr int kStripTouchNote = 12;
    inline constexpr bool isEncoderCc (int cc) noexcept { return cc >= kFirstEncoderCc && cc < kFirstEncoderCc + 8; }
    /** Relative encoder value -> signed ticks. */
    inline constexpr int relativeDelta (int value) noexcept { return value < 64 ? value : value - 128; }

    // ---- Sysex -------------------------------------------------------------------------
    inline constexpr uint8_t kAbleton[] = { 0x00, 0x21, 0x1D };
    inline constexpr uint8_t kCmdGetPalette = 0x04, kCmdSetMidiMode = 0x0A;

    /** Header bytes (device, model) of a Push sysex message, or false. `data` excludes F0 / F7. */
    inline bool parseSysexHeader (const uint8_t* data, int size, uint8_t& dev, uint8_t& model, uint8_t& cmd) noexcept
    {
        if (size < 6 || data[0] != kAbleton[0] || data[1] != kAbleton[1] || data[2] != kAbleton[2]) return false;
        dev = data[3]; model = data[4]; cmd = data[5];
        return true;
    }

    inline juce::MidiMessage sysex (uint8_t dev, uint8_t model, std::initializer_list<uint8_t> body)
    {
        uint8_t buf[32];
        int n = 0;
        for (auto b : kAbleton) buf[n++] = b;
        buf[n++] = dev; buf[n++] = model;
        for (auto b : body) if (n < (int) sizeof (buf)) buf[n++] = b;
        return juce::MidiMessage::createSysExMessage (buf, n);
    }

    /** RGB of a palette reply (cmd 0x04), with `index`; false if it isn't one. */
    inline bool parsePaletteReply (const uint8_t* data, int size, int& index, juce::uint8& r, juce::uint8& g, juce::uint8& b) noexcept
    {
        uint8_t dev, model, cmd;
        if (! parseSysexHeader (data, size, dev, model, cmd) || cmd != kCmdGetPalette || size < 6 + 9) return false;
        auto v = [data] (int k) { return (juce::uint8) ((data[k] & 0x7f) | ((data[k + 1] & 0x01) << 7)); };
        index = data[6];
        r = v (7); g = v (9); b = v (11);
        return true;
    }

    // ---- Colours -----------------------------------------------------------------------
    /** What a light should show; mapped to palette indices once Push's palette is known. */
    enum Colour : int { off = 0, amber, amberDim, white, green, greenDim, red, redDim, blue, dark, kNumColours };

    inline juce::Colour rgbOf (int c)
    {
        switch (c)
        {
            case amber:    return juce::Colour (0xffffa020);
            case amberDim: return juce::Colour (0xff4a2c06);
            case white:    return juce::Colour (0xffffffff);
            case green:    return juce::Colour (0xff30ff30);
            case greenDim: return juce::Colour (0xff0a400a);
            case red:      return juce::Colour (0xffff2010);
            case redDim:   return juce::Colour (0xff400804);
            case blue:     return juce::Colour (0xff2040ff);
            case dark:     return juce::Colour (0xff181818);
            default:       return juce::Colour (0xff000000);
        }
    }

    /** Before Push's palette has been read back: entries the manual documents. */
    inline int fallbackIndex (int c) noexcept
    {
        switch (c)
        {
            case amber:    return 122;   // (204,204,204)
            case amberDim: return 124;   // (20,20,20)
            case white:    return 122;
            case green:    return 126;
            case greenDim: return 126;
            case red:      return 127;
            case redDim:   return 127;
            case blue:     return 125;
            case dark:     return 124;
            default:       return 0;
        }
    }

    /** Nearest palette entry (1..127) to each colour, given Push's palette (0xRRGGBB, < 0 = unknown). */
    inline std::array<int, kNumColours> mapColours (const std::array<int, 128>& palette)
    {
        std::array<int, kNumColours> out {};
        for (int c = 0; c < kNumColours; ++c)
        {
            if (c == off) { out[(size_t) c] = 0; continue; }
            const auto want = rgbOf (c);
            int best = fallbackIndex (c);
            double bestD = 1.0e30;
            for (int i = 1; i < 128; ++i)
            {
                const int rgb = palette[(size_t) i];
                if (rgb < 0) continue;
                const double dr = ((rgb >> 16) & 0xff) - want.getRed();
                const double dg = ((rgb >> 8) & 0xff) - want.getGreen();
                const double db = (rgb & 0xff) - want.getBlue();
                const double d = 0.30 * dr * dr + 0.59 * dg * dg + 0.11 * db * db;
                if (d < bestD) { bestD = d; best = i; }
            }
            out[(size_t) c] = best;
        }
        return out;
    }
}
