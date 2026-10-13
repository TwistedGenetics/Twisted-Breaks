#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine/SampleBuffer.h"

namespace tb
{
    /*
        SampleLoader
        ------------
        Pure decode step, run ONLY on the background loader thread: file ->
        fully built SampleBuffer (audio + waveform overview). Never called
        from the audio thread; never touches engine state.
    */
    namespace SampleLoader
    {
        enum class Status { ok, missing, unreadable, tooLong, empty };

        struct Result
        {
            Status            status = Status::unreadable;
            SampleBuffer::Ptr buffer;
        };

        /** Five minutes is plenty for breaks and keeps memory sane (~110 MB stereo @ 48 kHz). */
        inline constexpr double maxSeconds = 300.0;

        /** WAV and AIFF are the target formats; FLAC is accepted as a bonus. */
        bool isSupportedFile (const juce::File&);

        Result loadFile (const juce::File&, juce::AudioFormatManager&);
    }
}
