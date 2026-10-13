#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "Core/EngineBridge.h"
#include "Core/SampleManager.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        WaveformEditor
        --------------
        The green-phosphor CRT. Shows the SELECTED pad's sample:

          * waveform (min/max per pixel column, phosphor glow)
          * START / END markers — drag them; they ARE the pad's start/end
            parameters, so knobs, automation and markers always agree
          * dimmed area outside the playable region
          * live playback cursor (runs backwards when REVERSE is on)
          * zoom: mouse wheel / pinch around the pointer, drag empty space to
            scroll, double-click to zoom back out
          * drop a WAV / AIFF on the screen to load it into the selected pad
          * SLICES (SLICE page, or a pad in SLICE mode): numbered markers;
            drag to move, double-click empty space to add, double-click a
            marker to delete, click a slice to select it (and hear it)

        When the pad is empty (or its file is missing) the screen falls back
        to a live oscilloscope of the master output, with a message.

        Editing is non-destructive: only parameters change, never audio.
    */
    class WaveformEditor final : public juce::Component,
                                 public juce::FileDragAndDropTarget,
                                 public juce::SettableTooltipClient
    {
    public:
        explicit WaveformEditor (TwistedBreaksProcessor&);

        void setPad (int pad);
        void refresh();                 // ~30 Hz from MainView

        /** SLICE page open: show + edit slice markers even when the pad isn't in SLICE mode. */
        void setSliceView (bool);
        void fitView();

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void fileDragEnter (const juce::StringArray&, int, int) override;
        void fileDragExit (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray& files, int, int) override;

        /** Visible span as fractions of the whole sample (exposed for tests). */
        double getViewStart()  const noexcept { return viewStart; }
        double getViewLength() const noexcept { return viewLength; }

    private:
        enum class Marker { none, start, end };

        juce::Rectangle<float> screenArea() const;   // the CRT glass
        juce::Rectangle<float> plotArea() const;     // where the waveform is drawn

        float  xForFraction (double f) const;
        double fractionForX (float x) const;
        Marker markerAt (float x) const;
        bool   editingSlices() const;
        int    sliceMarkerAt (float x) const;      // -1 if none
        int    sampleAt (float x) const;
        void   syncInfo();

        float  paramValue (const char* name) const;
        juce::RangedAudioParameter* param (const char* name) const;

        void zoomAround (float x, double factor);
        void scrollBy (double fraction);
        void clampView();

        void rebuildLayers();
        void rebuildWaveImage();
        void paintScope (juce::Graphics&, juce::Rectangle<float>);
        void paintSlices (juce::Graphics&, juce::Rectangle<float> area);
        void paintWave (juce::Graphics&, juce::Rectangle<float>);
        void paintLegend (juce::Graphics&);

        TwistedBreaksProcessor& processor;
        EngineBridge& bridge;

        int pad = 0;
        SampleManager::PadInfo info;
        uint32_t shownVersion = 0xffffffff;

        double viewStart = 0.0, viewLength = 1.0;

        // Last values drawn, to repaint only when something changes.
        float lastStart = -1.0f, lastEnd = -1.0f, lastHead = -2.0f;
        bool  lastReverse = false;
        int   lastHow = -1;

        Marker dragging = Marker::none;
        bool   draggingView = false;
        double dragAnchorFraction = 0.0, dragAnchorViewStart = 0.0;
        bool   fileHover = false;
        int    draggingSlice = -1;
        bool   auditioning = false;
        bool   sliceView = false;
        int    lastSlice = -1;

        // Scope fallback
        static constexpr int historySize = 2048;
        std::array<float, historySize> history {};
        int  writePos = 0;
        std::array<float, 1024> scratch {};
        bool hadSignal = false;

        juce::Image under, over, wave;
        bool waveDirty = true;
    };
}
