#include "UI/ResamplePanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        const char* const kCharacterBlurb[] = {
            "Float, 4-point Hermite: exactly as recorded",
            "26.04 kHz, 12 bit, no interpolation: steppy and gritty, more so pitched down",
            "40 kHz, 12 bit, linear, soft 15 kHz low-pass: punchy and warm",
            "31.25 kHz, 12 bit, linear, 9 kHz low-pass: dark and thick",
            "11 kHz, 8 bit, no interpolation: crunchy and aliased",
        };
        const Rectangle<float> kRecPlate { 18.0f, 10.0f, 124.0f, 22.0f };
        const Rectangle<float> kCharPlate { 18.0f, 122.0f, 124.0f, 22.0f };
    }

    ResamplePanel::ResamplePanel (TwistedBreaksProcessor& p) : processor (p)
    {
        auto& apvts = processor.getParameters();
        auto paramModel = [this] (const char* id, int max, std::function<String (int)> text)
        {
            DragReadout::Model m;
            m.get = [this, id] { return roundToInt (processor.getParameters().getRawParameterValue (id)->load()); };
            m.set = [this, id] (int v)
            {
                if (auto* prm = processor.getParameters().getParameter (id))
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) v));
                    prm->endChangeGesture();
                }
            };
            m.min = 0; m.max = max; m.def = 0; m.cycleOnClick = true; m.text = std::move (text);
            return m;
        };
        sourceRead.bind (paramModel (params::rsmpSource, 4, [] (int v) { return v == 0 ? String ("MASTER") : "GRP " + String::charToString ((juce_wchar) ('A' + v - 1)); }));
        barsRead.bind (paramModel (params::rsmpBars, 3, [] (int v) { return String (params::rsmpBarCounts[jlimit (0, 3, v)]) + " BAR"; }));
        startRead.bind (paramModel (params::rsmpStart, 1, [] (int v) { return v == 0 ? String ("NOW") : String ("BAR"); }));

        loopKey.setClickingTogglesState (true);
        sliceKey.setClickingTogglesState (true);
        loopAtt  = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::rsmpLoop, loopKey);
        sliceAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (apvts, params::rsmpSlice, sliceKey);

        recKey.onClick = [this]
        {
            auto& rs = processor.getResampler();
            if (rs.getState() == Resampler::State::idle) rs.record();
            refresh();
        };
        cancelKey.onClick = [this] { processor.getResampler().cancel(); refresh(); };

        for (int i = 0; i < PadVoice::kNumCharacters; ++i)
        {
            auto k = std::make_unique<PushKey> (PadVoice::character (i).name, String ("CHARACTER: ") + kCharacterBlurb[i], Colour (0xffffa928));
            k->setRadioGroupId (7301);
            k->onClick = [this, i] { setCharacter (selectedPad, i); refresh(); };
            addAndMakeVisible (*k);
            charKeys[(size_t) i] = std::move (k);
        }
        allKey.onClick = [this]
        {
            const int ch = characterOf (selectedPad);
            for (int pad = 0; pad < kNumPads; ++pad) setCharacter (pad, ch);
        };

        for (Component* comp : std::initializer_list<Component*> { &sourceRead, &barsRead, &startRead, &loopKey, &sliceKey, &recKey, &cancelKey, &allKey })
            addAndMakeVisible (comp);
        setSelectedPad (0);
    }

    ResamplePanel::~ResamplePanel()
    {
        loopAtt.reset();
        sliceAtt.reset();
    }

    int ResamplePanel::characterOf (int pad) const
    {
        return jlimit (0, PadVoice::kNumCharacters - 1,
                       roundToInt (processor.getParameters().getRawParameterValue (params::padId (pad, params::padCharacter))->load()));
    }

    void ResamplePanel::setCharacter (int pad, int ch)
    {
        if (auto* prm = processor.getParameters().getParameter (params::padId (pad, params::padCharacter)))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) ch));
            prm->endChangeGesture();
        }
    }

    void ResamplePanel::setSelectedPad (int pad)
    {
        selectedPad = jlimit (0, kNumPads - 1, pad);
        shownChar = -1;
        refresh();
        repaint();
    }

    String ResamplePanel::statusText() const
    {
        auto& rs = processor.getResampler();
        const String to = "PAD " + String (rs.getTargetPad() + 1).paddedLeft ('0', 2);
        switch (rs.getState())
        {
            case Resampler::State::armed:     return "WAITING FOR THE BAR LINE  ->  " + to;
            case Resampler::State::recording: return "RECORDING  " + String (roundToInt (rs.getProgress() * 100.0f)) + " %  ->  " + to;
            case Resampler::State::saving:    return "SAVING  ->  " + to;
            case Resampler::State::idle:      break;
        }
        if (rs.getCount() > 0) return "LAST: " + rs.getLastFile().getFileNameWithoutExtension().toUpperCase();
        return "READY  ->  " + to;
    }

    void ResamplePanel::refresh()
    {
        auto& rs = processor.getResampler();
        const auto st = rs.getState();
        recKey.setToggleState (st != Resampler::State::idle, dontSendNotification);
        recKey.setBlinking (st == Resampler::State::armed);
        recKey.tick();
        cancelKey.setEnabled (st == Resampler::State::armed || st == Resampler::State::recording);

        const int ch = characterOf (selectedPad);
        if (ch != shownChar)
        {
            shownChar = ch;
            for (int i = 0; i < PadVoice::kNumCharacters; ++i)
                charKeys[(size_t) i]->setToggleState (i == ch, dontSendNotification);
            repaint();
        }
        for (auto* r : { &sourceRead, &barsRead, &startRead }) r->repaint();

        const auto s = statusText();
        if (s != shownStatus || st == Resampler::State::recording)
        {
            shownStatus = s;
            repaint (0, 50, getWidth(), 64);
        }
    }

    void ResamplePanel::resized()
    {
        int x = 156;
        sourceRead.setBounds (x, 4, 78, 36);  x += 84;
        barsRead.setBounds   (x, 4, 64, 36);  x += 70;
        startRead.setBounds  (x, 4, 54, 36);  x += 62;
        loopKey.setBounds    (x, 10, 92, 30); x += 96;
        sliceKey.setBounds   (x, 10, jmin (110, getWidth() - x - 14), 30);

        recKey.setBounds (18, 58, 110, 48);
        cancelKey.setBounds (getWidth() - 104, 66, 86, 32);

        const int kw = 82, top = 152;
        for (int i = 0; i < PadVoice::kNumCharacters; ++i)
            charKeys[(size_t) i]->setBounds (18 + i * (kw + 6), top, kw, 34);
        allKey.setBounds (getWidth() - 104, top, 86, 34);
        background = {};
    }

    void ResamplePanel::paint (Graphics& g)
    {
        if (background.isNull())
        {
            background = Image (Image::RGB, getWidth() * 2, getHeight() * 2, true);
            Graphics bg (background);
            bg.addTransform (AffineTransform::scale (2.0f));
            Random rnd (1212);
            const auto all = getLocalBounds().toFloat();
            paint::plate (bg, all, c::steel, 4.0f);
            {
                Graphics::ScopedSaveState ss (bg);
                bg.reduceClipRegion (getLocalBounds());
                paint::ageArea (bg, all, rnd, 1.3f);
                paint::edgeWear (bg, all, rnd, 1.5f);
            }
            const auto r = all.reduced (9.0f);
            for (auto pt : { r.getTopLeft(), r.getTopRight(), r.getBottomLeft(), r.getBottomRight() })
                paint::rivet (bg, pt, 4.2f, rnd);
            paint::labelPlate (bg, kRecPlate, "RESAMPLE", {}, paint::PlateStyle::bakelite, rnd);
            paint::labelPlate (bg, kCharPlate, "CHARACTER", {}, paint::PlateStyle::bakelite, rnd);
        }
        g.drawImage (background, getLocalBounds().toFloat());

        // Recorder window: progress bar + status.
        auto& rs = processor.getResampler();
        const auto win = Rectangle<float> (138.0f, 60.0f, (float) getWidth() - 138.0f - 114.0f, 44.0f);
        paint::recess (g, win, 4.0f);
        const auto st = rs.getState();
        if (st == Resampler::State::recording || st == Resampler::State::saving)
        {
            g.setColour (c::red.withAlpha (0.35f));
            g.fillRect (win.reduced (3.0f).withWidth ((win.getWidth() - 6.0f) * rs.getProgress()));
        }
        g.setColour (st == Resampler::State::idle ? c::vfd : c::vfd.brighter (0.3f));
        g.setFont (monoFont (14.0f));
        g.drawFittedText (shownStatus, win.reduced (10.0f, 2.0f).toNearestInt(), Justification::centredLeft, 1, 0.7f);

        // CHARACTER: which pad, and what the mode does.
        const auto info = processor.getSamples().getInfo (selectedPad);
        g.setColour (c::plateText.withAlpha (0.9f));
        g.setFont (industrialFont (13.0f));
        g.drawText ("PAD " + String (selectedPad + 1).paddedLeft ('0', 2) + (info.name.isNotEmpty() ? "  -  " + info.name.toUpperCase() : String()),
                    Rectangle<float> (156.0f, 122.0f, (float) getWidth() - 170.0f, 22.0f), Justification::centredLeft, true);
        g.setColour (c::vfd.withAlpha (0.85f));
        g.setFont (monoFont (12.0f));
        g.drawText (kCharacterBlurb[jlimit (0, 4, shownChar)], Rectangle<float> (18.0f, 194.0f, (float) getWidth() - 36.0f, 20.0f),
                    Justification::centredLeft, true);
    }
}
