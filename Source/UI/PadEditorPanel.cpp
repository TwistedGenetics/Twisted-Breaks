#include "UI/PadEditorPanel.h"
#include "UI/PanelPainter.h"
#include "UI/Palette.h"
#include "PluginProcessor.h"

namespace tb::ui
{
    using namespace juce;
    namespace c = colours;

    namespace
    {
        struct KnobDef { const char* caption; const char* param; double resetTo; const char* tip; };
        const KnobDef kKnobs[] = {
            { "GAIN",  params::padGain,  0.0, "Pad level (double-click: 0 dB)" },
            { "PAN",   params::padPan,   0.0, "Pad pan (double-click: centre)" },
            { "PITCH", params::padPitch, 0.0, "Pitch in semitones, +/-12 (double-click: 0)" },
            { "START", params::padStart, 0.0, "Where playback starts (also the S marker on the screen)" },
            { "END",   params::padEnd,   1.0, "Where playback ends (also the E marker on the screen)" },
        };

        constexpr int kHeaderH = 28;
        constexpr int kKnobW   = 86;
        constexpr int kKnobTop = 52;

        // Remembered between choosers so LOAD reopens where you last were.
        File& lastFolder()
        {
            static File f = File::getSpecialLocation (File::userMusicDirectory);
            return f;
        }
    }

    PadEditorPanel::PadEditorPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        for (int i = 0; i < kNumKnobs; ++i)
        {
            auto& k = knobs[(size_t) i];
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::TextBoxBelow, true, kKnobW - 8, 16);
            k.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            k.setTooltip (kKnobs[i].tip);
            addAndMakeVisible (k);
        }

        addAndMakeVisible (reverseSwitch);
        addAndMakeVisible (loadKey);
        addAndMakeVisible (clearKey);

        loadKey.onClick  = [this] { chooseFile(); };
        clearKey.onClick = [this] { processor.getSamples().clearPad (pad); };

        bindControls();
    }

    PadEditorPanel::~PadEditorPanel()
    {
        // Attachments must go before the controls they point at.
        for (auto& a : knobAttachments) a.reset();
        reverseAttachment.reset();
    }

    void PadEditorPanel::setPad (int newPad)
    {
        newPad = jlimit (0, kNumPads - 1, newPad);
        if (newPad == pad) return;
        pad = newPad;
        bindControls();
        shownVersion = 0xffffffff;
        refresh();
        repaint();
    }

    void PadEditorPanel::bindControls()
    {
        auto& apvts = processor.getParameters();

        for (int i = 0; i < kNumKnobs; ++i)
        {
            knobAttachments[(size_t) i].reset();
            knobAttachments[(size_t) i] = std::make_unique<SliderAttachment> (apvts, params::padId (pad, kKnobs[i].param),
                                                                               knobs[(size_t) i]);
            knobs[(size_t) i].setDoubleClickReturnValue (true, kKnobs[i].resetTo);
        }

        reverseAttachment.reset();
        reverseAttachment = std::make_unique<ButtonAttachment> (apvts, params::padId (pad, params::padReverse), reverseSwitch);
    }

    void PadEditorPanel::chooseFile()
    {
        const int target = pad;
        chooser = std::make_unique<FileChooser> ("Load a sample into pad " + String (target + 1),
                                                 lastFolder(), "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac");

        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this, target] (const FileChooser& fc)
                              {
                                  const auto file = fc.getResult();
                                  if (file.existsAsFile())
                                  {
                                      lastFolder() = file.getParentDirectory();
                                      processor.getSamples().loadFile (target, file);
                                  }
                              });
    }

    void PadEditorPanel::refresh()
    {
        const auto v = processor.getSamples().getVersion (pad);
        if (v != shownVersion)
        {
            shownVersion = v;
            clearKey.setEnabled (processor.getSamples().getInfo (pad).status != SampleManager::Status::empty);
            repaint();
        }
    }

    //==========================================================================
    Rectangle<int> PadEditorPanel::knobCell (int index) const
    {
        return { index * kKnobW, kKnobTop, kKnobW, getHeight() - kKnobTop };
    }

    void PadEditorPanel::resized()
    {
        const int w = getWidth();
        loadKey.setBounds  (w - 160, 0, 76, kHeaderH);
        clearKey.setBounds (w - 78,  0, 78, kHeaderH);

        for (int i = 0; i < kNumKnobs; ++i)
            knobs[(size_t) i].setBounds (knobCell (i).reduced (4, 0));

        reverseSwitch.setBounds (kNumKnobs * kKnobW + 12, kKnobTop - 10, 66, getHeight() - kKnobTop + 10);

        // Static art: recess behind the header readout and the knob captions.
        const float s = 2.0f;
        background = Image (Image::ARGB, roundToInt (getWidth() * s), roundToInt (getHeight() * s), true);
        Graphics g (background);
        g.addTransform (AffineTransform::scale (s));

        paint::recess (g, Rectangle<float> (84.0f, 0.0f, (float) (w - 84 - 168), (float) kHeaderH), 3.0f);

        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (13.0f));
        for (int i = 0; i < kNumKnobs; ++i)
            g.drawText (kKnobs[i].caption, knobCell (i).withY (kKnobTop - 16).withHeight (14).toFloat(),
                        Justification::centred, false);
    }

    void PadEditorPanel::paint (Graphics& g)
    {
        // Pad number plate (black, engraved) — dynamic, so not in the cache.
        Random rnd (pad + 77);
        paint::labelPlate (g, { 0.0f, 0.0f, 76.0f, (float) kHeaderH },
                           "PAD " + String (pad + 1).paddedLeft ('0', 2), {}, paint::PlateStyle::black, rnd);

        g.drawImage (background, getLocalBounds().toFloat());

        // Sample readout in the recess.
        const auto info = processor.getSamples().getInfo (pad);
        auto r = Rectangle<float> (92.0f, 0.0f, (float) (getWidth() - 84 - 168 - 16), (float) kHeaderH);

        String text;
        Colour col = c::vfd;
        switch (info.status)
        {
            case SampleManager::Status::empty:   text = "EMPTY - LOAD OR DROP A WAV / AIFF"; col = c::vfd.withAlpha (0.55f); break;
            case SampleManager::Status::loading: text = "LOADING " + info.name.toUpperCase(); break;
            case SampleManager::Status::ready:
                text = info.name.toUpperCase() + "   " + String (info.buffer->getLengthSeconds(), 2) + " s";
                break;
            case SampleManager::Status::missing:
            case SampleManager::Status::unreadable:
            case SampleManager::Status::tooLong:
                text = SampleManager::statusText (info.status) + ": " + info.file.getFileName();
                col  = c::red;
                break;
        }

        g.setColour (col);
        g.setFont (monoFont (14.0f));
        g.drawText (text, r, Justification::centredLeft, true);
    }
}
