#include "Engine/SampleLoader.h"

namespace tb::SampleLoader
{
    bool isSupportedFile (const juce::File& f)
    {
        return f.hasFileExtension ("wav;wave;aif;aiff;aifc;flac");
    }

    Result loadFile (const juce::File& file, juce::AudioFormatManager& formats)
    {
        Result r;

        if (! file.existsAsFile())
        {
            r.status = Status::missing;
            return r;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        {
            r.status = Status::unreadable;
            return r;
        }

        if ((double) reader->lengthInSamples > maxSeconds * reader->sampleRate)
        {
            r.status = Status::tooLong;
            return r;
        }

        const int length = (int) reader->lengthInSamples;
        if (length <= 0)
        {
            r.status = Status::empty;
            return r;
        }

        // Keep mono as mono (cheaper to play); anything wider uses the first two channels.
        const int channels = reader->numChannels == 1 ? 1 : 2;

        SampleBuffer::Ptr buf = new SampleBuffer();
        buf->audio.setSize (channels, length);
        buf->sourceSampleRate = reader->sampleRate;
        buf->file = file;
        buf->name = file.getFileNameWithoutExtension();
        buf->fileModTime = file.getLastModificationTime();
        buf->fileSize = file.getSize();

        if (! reader->read (&buf->audio, 0, length, 0, true, channels > 1))
        {
            r.status = Status::unreadable;
            return r;
        }

        buf->buildOverview();

        r.status = Status::ok;
        r.buffer = buf;
        return r;
    }
}
