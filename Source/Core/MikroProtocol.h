#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

/*
    MikroProtocol
    -------------
    Native Instruments Maschine Mikro MK3 over USB HID (vendor 0x17CC,
    product 0x1700), without NI's software. Not documented by NI: the layout
    below is the one two open-source drivers agree on
    (github.com/r00tman/maschine-mikro-mk3-driver, MIT;
     github.com/flokapi/pymikro).

    IN  report 0x01  buttons: bytes 1..6 = bits, button i*8+j (Buttons below);
                     byte 7 = encoder position 0..15 (wraps);
                     byte 10 = touch strip 0 (not touched) or 1..200
    IN  report 0x02  pads: from byte 1, triplets (pad, event | value >> 8, value & 0xff),
                     value 0..4095; a zero triplet after the first ends the list.
                     Pad index 0 = bottom left, row by row upwards (Maschine's
                     pad 1); events 0x00 press on, 0x10 note on, 0x20 press off,
                     0x30 note off, 0x40 pressure.
    OUT report 0x80  lights, 80 bytes after the id: 0..38 buttons (brightness),
                     39..54 pads (colour << 2 | level 0..3), 55..79 touch strip LEDs
    OUT report 0xE0  screen, 128 x 32 1-bit, sent as two halves of 256 bytes,
                     each after a 9-byte header; buffer[page * 128 + x], bit
                     (y % 8) of page y / 8, a CLEAR bit = pixel lit.
*/
namespace tb::mikro
{
    inline constexpr int kVendorId = 0x17cc, kProductId = 0x1700;
    inline constexpr int kNumPads = 16, kNumStripLeds = 25;
    inline constexpr int kScreenW = 128, kScreenH = 32;

    enum Button : int
    {
        maschine = 0, star, browse, volume, swing, tempo, plugin, sampling,
        left, right, pitch, mod, perform, notes, group, autoButton,
        lock, noteRepeat, restart, erase, tap, follow, play, rec,
        stop, shift, fixedVel, padMode, keyboard, chords, step, scene,
        pattern, events, variation, duplicate, select, solo, mute,
        encoderPress, encoderTouch,
        kNumButtons
    };
    inline constexpr int kNumLitButtons = 39;     // encoderPress / encoderTouch have no light

    inline const char* buttonName (int b) noexcept
    {
        static const char* const names[] = {
            "MASCHINE", "STAR", "BROWSE", "VOLUME", "SWING", "TEMPO", "PLUG-IN", "SAMPLING",
            "LEFT", "RIGHT", "PITCH", "MOD", "PERFORM", "NOTES", "GROUP", "AUTO",
            "LOCK", "NOTE REPEAT", "RESTART", "ERASE", "TAP", "FOLLOW", "PLAY", "REC",
            "STOP", "SHIFT", "FIXED VEL", "PAD MODE", "KEYBOARD", "CHORDS", "STEP", "SCENE",
            "PATTERN", "EVENTS", "VARIATION", "DUPLICATE", "SELECT", "SOLO", "MUTE",
            "ENCODER PUSH", "ENCODER TOUCH" };
        return b >= 0 && b < kNumButtons ? names[b] : "?";
    }

    // Light levels (the driver's measured values) and pad colours.
    enum Level : uint8_t { levelOff = 0x00, levelDim = 0x7c, levelNormal = 0x7e, levelBright = 0x7f };
    enum PadColour : int { off = 0, red, orange, lightOrange, warmYellow, yellow, lime, green, mint, cyan,
                           turquoise, blue, plum, violet, purple, magenta, fuchsia, white, kNumPadColours };
    /** Pad light byte: colour + level 0 (off) .. 3 (bright). */
    inline constexpr uint8_t padByte (int colour, int level) noexcept
    {
        return level <= 0 || colour <= 0 ? 0 : (uint8_t) ((colour << 2) | (level & 3));
    }

    // ---- Input ------------------------------------------------------------------------
    struct Event
    {
        enum Kind : uint8_t { button, pad, encoder, strip } kind = button;
        int  index = 0;       // button / pad (0..15) / -
        int  value = 0;       // pad 0..4095, encoder ticks (+/-), strip 0..200 (0 = let go)
        bool down = false;    // button / pad pressed
        int  padEvent = 0;    // raw pad event (0x00, 0x10, 0x20, 0x30, 0x40)
    };

    /** Turns input reports into events, remembering what it saw last. */
    class InputParser
    {
    public:
        /** Appends the events in one report (report id in data[0]). */
        void parse (const uint8_t* data, int size, std::vector<Event>& out)
        {
            if (size < 1) return;
            if (data[0] == 0x01 && size >= 11)
            {
                for (int b = 0; b < kNumButtons; ++b)
                {
                    const bool down = (data[1 + b / 8] >> (b % 8)) & 1;
                    if (down != buttons[(size_t) b]) { buttons[(size_t) b] = down; out.push_back ({ Event::button, b, 0, down, 0 }); }
                }
                const int enc = data[7] & 0x0f;
                if (encoder >= 0 && enc != encoder)
                {
                    const int d = ((enc - encoder + 8) & 15) - 8;
                    if (d != 0) out.push_back ({ Event::encoder, 0, d, false, 0 });
                }
                encoder = enc;
                const int s = data[10];
                if (s != strip) { strip = s; out.push_back ({ Event::strip, 0, s, s > 0, 0 }); }
            }
            else if (data[0] == 0x02)
            {
                for (int i = 1; i + 2 < size; i += 3)
                {
                    const int idx = data[i], ev = data[i + 1] & 0xf0, val = ((data[i + 1] & 0x0f) << 8) | data[i + 2];
                    if (i > 1 && idx == 0 && ev == 0 && val == 0) break;
                    if (idx >= kNumPads) continue;
                    const bool down = ev == 0x00 || ev == 0x10 || (ev == 0x40 && val > 0);
                    out.push_back ({ Event::pad, idx, val, down, ev });
                }
            }
        }
        bool isDown (int b) const noexcept { return b >= 0 && b < kNumButtons && buttons[(size_t) b]; }

    private:
        std::array<bool, kNumButtons> buttons {};
        int encoder = -1, strip = 0;
    };

    // ---- Lights --------------------------------------------------------------------------
    struct Lights
    {
        std::array<uint8_t, 80> state {};
        void setButton (int b, uint8_t level) noexcept { if (b >= 0 && b < kNumLitButtons) state[(size_t) b] = level; }
        void setPad (int pad, int colour, int level) noexcept { if (pad >= 0 && pad < kNumPads) state[(size_t) (39 + pad)] = padByte (colour, level); }
        void setStrip (int led, uint8_t level) noexcept { if (led >= 0 && led < kNumStripLeds) state[(size_t) (55 + led)] = level; }
        void clear() noexcept { state.fill (0); }
        /** The output report: 0x80 + the 80 bytes. */
        std::vector<uint8_t> report() const
        {
            std::vector<uint8_t> r (1 + state.size());
            r[0] = 0x80;
            std::memcpy (r.data() + 1, state.data(), state.size());
            return r;
        }
    };

    // ---- Screen ---------------------------------------------------------------------------
    /** Classic 5x7 font, ' '..'_' (lower case is drawn upper case). Column bytes, bit 0 = top. */
    inline const uint8_t* glyph (char c) noexcept
    {
        static const uint8_t font[][5] = {
            {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
            {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
            {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
            {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
            {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
            {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
            {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
            {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},
            {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
            {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
            {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
            {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
            {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
            {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
            {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
            {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40} };
        if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
        if (c < ' ' || c > '_') c = '?';
        return font[c - ' '];
    }

    class Screen
    {
    public:
        Screen() { clear(); }
        void clear() noexcept { buffer.fill (0xff); }
        void set (int x, int y, bool on) noexcept
        {
            if (x < 0 || x >= kScreenW || y < 0 || y >= kScreenH) return;
            auto& b = buffer[(size_t) ((y / 8) * kScreenW + x)];
            const uint8_t mask = (uint8_t) (1u << (y % 8));
            if (on) b = (uint8_t) (b & ~mask); else b = (uint8_t) (b | mask);
        }
        bool get (int x, int y) const noexcept
        {
            if (x < 0 || x >= kScreenW || y < 0 || y >= kScreenH) return false;
            return (buffer[(size_t) ((y / 8) * kScreenW + x)] & (1u << (y % 8))) == 0;
        }
        /** Text at (x, y), 5x7 glyphs scaled by `scale`, 1 column apart. Returns the x after it. */
        int text (int x, int y, const char* s, int scale = 1) noexcept
        {
            for (; s != nullptr && *s != 0; ++s)
            {
                const uint8_t* g = glyph (*s);
                for (int cx = 0; cx < 5; ++cx)
                    for (int cy = 0; cy < 7; ++cy)
                        if ((g[cx] >> cy) & 1)
                            for (int sx = 0; sx < scale; ++sx)
                                for (int sy = 0; sy < scale; ++sy)
                                    set (x + cx * scale + sx, y + cy * scale + sy, true);
                x += 6 * scale;
            }
            return x;
        }
        /** The two output reports (0xE0 + header + half the screen each). */
        std::array<std::vector<uint8_t>, 2> reports() const
        {
            static const uint8_t hi[9] = { 0xe0, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x02, 0x00 };
            static const uint8_t lo[9] = { 0xe0, 0x00, 0x00, 0x02, 0x00, 0x80, 0x00, 0x02, 0x00 };
            std::array<std::vector<uint8_t>, 2> r;
            for (int h = 0; h < 2; ++h)
            {
                r[(size_t) h].resize (9 + 256);
                std::memcpy (r[(size_t) h].data(), h == 0 ? hi : lo, 9);
                std::memcpy (r[(size_t) h].data() + 9, buffer.data() + h * 256, 256);
            }
            return r;
        }
        const std::array<uint8_t, 512>& raw() const noexcept { return buffer; }

    private:
        std::array<uint8_t, 512> buffer {};
    };
}
