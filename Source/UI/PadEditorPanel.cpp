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
            { "PITCH", params::padPitch, 0.0, "Pitch in semitones, +/-12 (double-click: 0). Synced loops follow the tempo instead." },
            { "START", params::padStart, 0.0, "Where playback starts (also the S marker on the screen)" },
            { "END",   params::padEnd,   1.0, "Where playback ends (also the E marker on the screen)" },
        };

        constexpr int kHeaderH = 28;
        constexpr int kKnobW   = 86;
        constexpr int kCtlTop  = 52;     // top of controls (captions sit above)

        // PLAY page columns: x, width, caption
        struct Col { int x, w; const char* caption; };
        const Col kPlayCols[] = {
            {   0, 92, "MODE"  },
            {  94, 58, "SYNC"  },
            { 154, 92, "RATE"  },
            { 248, 92, "CHOKE" },
            { 342, 70, "VEL"   },
            { 414, 40, ""      },     // mute / solo keys
            { 458, 82, "NOTE"  },
        };

        File& lastFolder()
        {
            static File f = File::getSpecialLocation (File::userMusicDirectory);
            return f;
        }

        void styleKnob (Slider& k, int boxWidth)
        {
            k.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (Slider::TextBoxBelow, true, boxWidth, 16);
            k.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        }
    }

    PadEditorPanel::PadEditorPanel (TwistedBreaksProcessor& p) : processor (p)
    {
        for (int i = 0; i < kNumKnobs; ++i)
        {
            styleKnob (knobs[(size_t) i], kKnobW - 8);
            knobs[(size_t) i].setTooltip (kKnobs[i].tip);
            addChildComponent (knobs[(size_t) i]);
        }
        addChildComponent (reverseSwitch);

        styleKnob (velKnob, 62);
        velKnob.setTooltip ("Velocity sensitivity: 0% = every hit at full level, 100% = level follows how hard you play");
        modeSel.setTooltip ("ONE: plays once.  LOOP: repeats while the pad / note is held.  SLICE: arrives in Phase 4 (plays as ONE until then).");
        rateSel.setTooltip ("Loop speed: 1/4, 1/2, 1x or 2x");
        chokeSel.setTooltip ("Choke group: hitting a pad stops the other pads in the same group");
        muteKey.setClickingTogglesState (true);
        soloKey.setClickingTogglesState (true);

        for (Component* comp : std::initializer_list<Component*> { &modeSel, &syncSwitch, &rateSel, &chokeSel,
                                                                    &velKnob, &muteKey, &soloKey, &noteSel })
            addChildComponent (comp);

        noteSel.onNoteChanged = [this] (int n)
        {
            processor.getEngine().setPadNote (pad, n);
            processor.markStateChanged();
        };
        noteSel.onLearn = [this]
        {
            auto& e = processor.getEngine();
            e.startNoteLearn (e.getNoteLearnPad() == pad ? -1 : pad);   // press again to cancel
        };

        for (auto* k : { &soundTab, &playTab, &loadKey, &clearKey })
            addAndMakeVisible (k);

        soundTab.onClick = [this] { setPage (Page::sound); };
        playTab.onClick  = [this] { setPage (Page::play); };
        loadKey.onClick  = [this] { chooseFile(); };
        clearKey.onClick = [this] { processor.getSamples().clearPad (pad); };

        bindControls();
        setPage (Page::sound);
    }

    PadEditorPanel::~PadEditorPanel()
    {
        // Attachments must go before the controls they point at.
        for (auto& a : knobAttachments) a.reset();
        velAttachment.reset();
        reverseAttachment.reset(); syncAttachment.reset(); muteAttachment.reset(); soloAttachment.reset();
        modeSel.attach (nullptr); rateSel.attach (nullptr); chokeSel.attach (nullptr);
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

    void PadEditorPanel::setPage (Page p)
    {
        page = p;
        const bool sound = page == Page::sound;
        soundTab.setToggleState (sound, dontSendNotification);
        playTab.setToggleState (! sound, dontSendNotification);

        for (auto& k : knobs) k.setVisible (sound);
        reverseSwitch.setVisible (sound);
        for (Component* comp : std::initializer_list<Component*> { &modeSel, &syncSwitch, &rateSel, &chokeSel,
                                                                    &velKnob, &muteKey, &soloKey, &noteSel })
            comp->setVisible (! sound);
        repaint();
    }

    void PadEditorPanel::bindControls()
    {
        auto& apvts = processor.getParameters();
        auto id = [this] (const char* name) { return params::padId (pad, name); };

        for (int i = 0; i < kNumKnobs; ++i)
        {
            knobAttachments[(size_t) i].reset();
            knobAttachments[(size_t) i] = std::make_unique<SliderAttachment> (apvts, id (kKnobs[i].param), knobs[(size_t) i]);
            knobs[(size_t) i].setDoubleClickReturnValue (true, kKnobs[i].resetTo);
        }

        velAttachment.reset();
        velAttachment = std::make_unique<SliderAttachment> (apvts, id (params::padVelSens), velKnob);
        velKnob.setDoubleClickReturnValue (true, 1.0);

        reverseAttachment.reset(); syncAttachment.reset(); muteAttachment.reset(); soloAttachment.reset();
        reverseAttachment = std::make_unique<ButtonAttachment> (apvts, id (params::padReverse), reverseSwitch);
        syncAttachment    = std::make_unique<ButtonAttachment> (apvts, id (params::padSync),    syncSwitch);
        muteAttachment    = std::make_unique<ButtonAttachment> (apvts, id (params::padMute),    muteKey);
        soloAttachment    = std::make_unique<ButtonAttachment> (apvts, id (params::padSolo),    soloKey);

        modeSel.attach  (apvts.getParameter (id (params::padMode)));
        rateSel.attach  (apvts.getParameter (id (params::padRate)));
        chokeSel.attach (apvts.getParameter (id (params::padChoke)));

        noteSel.setNote (processor.getEngine().getPadNote (pad));
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

        auto& engine = processor.getEngine();
        noteSel.setNote (engine.getPadNote (pad));
        noteSel.setLearning (engine.getNoteLearnPad() == pad);
        noteSel.tick();

        // Rate only matters for loops; dim it otherwise.
        const bool loop = modeSel.getIndex() == params::modeLoop;
        rateSel.setAlpha (loop ? 1.0f : 0.45f);
        syncSwitch.setAlpha (loop ? 1.0f : 0.45f);
    }

    //==========================================================================
    Rectangle<int> PadEditorPanel::knobCell (int index) const
    {
        return { index * kKnobW, kCtlTop, kKnobW, getHeight() - kCtlTop };
    }

    void PadEditorPanel::resized()
    {
        const int w = getWidth();
        soundTab.setBounds (82,  0, 68, kHeaderH);
        playTab.setBounds  (154, 0, 62, kHeaderH);
        loadKey.setBounds  (w - 158, 0, 76, kHeaderH);
        clearKey.setBounds (w - 78,  0, 78, kHeaderH);

        for (int i = 0; i < kNumKnobs; ++i)
            knobs[(size_t) i].setBounds (knobCell (i).reduced (4, 0));
        reverseSwitch.setBounds (kNumKnobs * kKnobW + 12, kCtlTop - 10, 66, getHeight() - kCtlTop + 10);

        const int h = getHeight() - kCtlTop;
        auto col = [&] (int i) { return Rectangle<int> (kPlayCols[i].x, kCtlTop, kPlayCols[i].w, h); };
        modeSel.setBounds   (col (0).withTrimmedTop (-6));
        syncSwitch.setBounds (col (1).withTrimmedTop (-10));
        rateSel.setBounds   (col (2).withTrimmedTop (-6));
        chokeSel.setBounds  (col (3).withTrimmedTop (-6));
        velKnob.setBounds   (col (4));
        auto ms = col (5).withTrimmedTop (-14);
        muteKey.setBounds (ms.removeFromTop (ms.getHeight() / 2).reduced (0, 2));
        soloKey.setBounds (ms.reduced (0, 2));
        noteSel.setBounds (col (6).withTrimmedTop (2).withTrimmedBottom (6));
    }

    void PadEditorPanel::paint (Graphics& g)
    {
        Random rnd (pad + 77);
        paint::labelPlate (g, { 0.0f, 0.0f, 76.0f, (float) kHeaderH },
                           "PAD " + String (pad + 1).paddedLeft ('0', 2), {}, paint::PlateStyle::black, rnd);

        // Sample readout in a recess between the tabs and LOAD.
        auto r = Rectangle<float> (222.0f, 0.0f, (float) (getWidth() - 222 - 164), (float) kHeaderH);
        paint::recess (g, r, 3.0f);
        r.reduce (8.0f, 0.0f);

        const auto info = processor.getSamples().getInfo (pad);
        String text;
        Colour col = c::vfd;
        switch (info.status)
        {
            case SampleManager::Status::empty:   text = "EMPTY - LOAD OR DROP"; col = c::vfd.withAlpha (0.55f); break;
            case SampleManager::Status::loading: text = "LOADING " + info.name.toUpperCase(); break;
            case SampleManager::Status::ready:
                text = info.name.toUpperCase() + "  " + String (info.buffer->getLengthSeconds(), 2) + "s";
                break;
            case SampleManager::Status::missing:
            case SampleManager::Status::unreadable:
            case SampleManager::Status::tooLong:
                text = SampleManager::statusText (info.status) + ": " + info.file.getFileName();
                col  = c::red;
                break;
        }
        g.setColour (col);
        g.setFont (monoFont (13.0f));
        g.drawText (text, r, Justification::centredLeft, true);

        // Captions for the current page.
        g.setColour (c::bakelite.withAlpha (0.85f));
        g.setFont (industrialFont (13.0f));
        if (page == Page::sound)
        {
            for (int i = 0; i < kNumKnobs; ++i)
                g.drawText (kKnobs[i].caption, knobCell (i).withY (kCtlTop - 16).withHeight (14).toFloat(),
                            Justification::centred, false);
        }
        else
        {
            for (const auto& cdef : kPlayCols)
                if (*cdef.caption != 0 && String (cdef.caption) != "SYNC")
                    g.drawText (cdef.caption, Rectangle<float> ((float) cdef.x, (float) kCtlTop - 18.0f, (float) cdef.w, 14.0f),
                                Justification::centred, false);
        }
    }
}
