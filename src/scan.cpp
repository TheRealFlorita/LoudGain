/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * 2019-06-30 - v0.2.1 - Matthias C. Hormann
 *  - Added version
 *  - Added writing tags to Ogg Vorbis files (now supports MP3, FLAC, Ogg Vorbis)
 *  - Always remove REPLAYGAIN_REFERENCE_LOUDNESS, wrong value might confuse players
 *  - Added notice in help on which file types can be written
 *  - Added album summary
 * 2019-07-07 - v0.2.2 - Matthias C. Hormann
 *  - Fixed album peak calculation.
 *  - Write REPLAYGAIN_ALBUM_* tags only if in album mode
 *  - Better versioning (CMakeLists.txt → config.h)
 *  - TODO: clipping calculation still wrong
 * 2019-07-08 - v0.2.4 - Matthias C. Hormann
 *  - add "-s e" mode, writes extra tags (REPLAYGAIN_REFERENCE_LOUDNESS,
 *    REPLAYGAIN_TRACK_RANGE and REPLAYGAIN_ALBUM_RANGE)
 *  - add "-s l" mode (like "-s e" but uses LU/LUFS instead of dB)
 * 2019-07-08 - v0.2.5 - Matthias C. Hormann
 *  - Clipping warning & prevention (-k) now works correctly, both track & album
 * 2019-07-09 - v0.2.6 - Matthias C. Hormann
 *  - Add "-L" mode to force lowercase tags in MP3/ID3v2.
 * 2019-07-10 - v0.2.7 - Matthias C. Hormann
 *  - Add "-S" mode to strip ID3v1/APEv2 tags from MP3 files.
 *  - Add "-I 3"/"-I 4" modes to select ID3v2 version to write.
 *  - First step to implement a new tab-delimited list format: "-O" mode.
 * 2019-07-13 - v0.2.8 - Matthias C. Hormann
 *  - new -O output format: re-ordered, now shows peak before/after gain applied
 *  - -k now defaults to clipping prevention at -1 dBTP (as EBU recommends)
 *  - New -K: Allows clippping prevention with settable dBTP level,
 *     i.e. -K 0 (old-style) or -K -2 (to compensate for post-processing losses)
 * 2019-08-06 - v0.5.3 - Matthias C. Hormann
 *  - Add support for Opus (.opus) files.
 * 2019-08-16 - v0.6.0 - Matthias C. Hormann
 *  - Rework for new FFmpeg API (get rid of deprecated calls)
 *
 * 2025-03-08 - v1.0.0 - Floor
 * - Refactor/rewrite of code to add windows compatibility and multi-threading capability
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <iostream>
#include <sstream>
#include <loudgain.hpp>
#include <scan.hpp>


#define LUFS_TO_RG(L) (-18 - L)

AudioFile::AudioFile(const fs::path &path)
{
    if (!fs::is_regular_file(path))
    {
        std::stringstream err;
        err << "[" << path << "] Audio file does not exist!" << std::endl;
        std::cerr << err.str();
        exit(EXIT_FAILURE);
    }

    p = path;
}


AudioFile::~AudioFile()
{
    destroyEbuR128State();
}

bool AudioFile::destroyEbuR128State()
{
    if (eburState != NULL)
    {
        ebur128_destroy(&eburState);
        free(eburState);
        eburState = NULL;
        return true;
    }
    return false;
}

fs::path AudioFile::filePath()
{
    return p;
}

#if defined(_WIN32) || defined(WIN32)
    wchar_t* AudioFile::getTagLibFilePath()
    {
        fp = filePath().wstring();
        return fp.data();
    }
#else
    const char* AudioFile::getTagLibFilePath()
    {
        return filePath().c_str();
    }
#endif

fs::path AudioFile::fileName()
{
    return p.filename();
}

fs::path AudioFile::directory()
{
    return p.parent_path();
}

bool AudioFile::initFile()
{
    destroyEbuR128State();
    avCodecId = AV_CODEC_ID_NONE;
    avFormat = "";

    AVFormatContext *container = NULL;
    int rc = avformat_open_input(&container, filePath().string().c_str(), NULL, NULL);
    if (rc < 0)
    {
        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not open input: " << errbuf << std::endl;
        std::cerr << err.str();
        return false;
    }
    avFormat = container->iformat->name;

    rc = avformat_find_stream_info(container, NULL);
    if (rc < 0)
    {
        avformat_close_input(&container);

        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not find stream info: " << errbuf << std::endl;
        std::cerr << err.str();
        return false;
    }

    /* Select the audio stream */
    AVCodec *codec;
    int stream_id;

#if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(59,0,100) )
    stream_id = av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
#else
    stream_id = av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, const_cast<const AVCodec**>(&codec), 0);
#endif

    if (stream_id < 0)
    {
        avformat_close_input(&container);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not find audio stream!" << std::endl;
        std::cerr << err.str();
        return false;
    }

    /* Create decoding context */
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        avformat_close_input(&container);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not allocate audio codec context!" << std::endl;
        std::cerr << err.str();
        return false;
    }

    avcodec_parameters_to_context(ctx, container->streams[stream_id]->codecpar);

    /* Init the audio decoder */
    rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0)
    {
        avcodec_free_context(&ctx);
        avformat_close_input(&container);

        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not open codec: " << errbuf << std::endl;
        std::cerr << err.str();
        return false;
    }

    avCodecId = codec->id;

    avcodec_free_context(&ctx);
    avformat_close_input(&container);
    return true;
}

bool AudioFile::scanFile(double pregain, bool verbose)
{
    AVFormatContext *container = NULL;
    int rc = avformat_open_input(&container, filePath().string().c_str(), NULL, NULL);
    if (rc < 0)
    {
        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not open input: " << errbuf << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }
    avFormat = container->iformat->name;

    if (verbose)
    {
        std::stringstream msg;
        msg << "[" << fileName() << "] " << "Container: " << container->iformat->long_name << " [" << avFormat  << "]" << std::endl;
        std::cout << msg.str();
    }

    rc = avformat_find_stream_info(container, NULL);
    if (rc < 0)
    {
        avformat_close_input(&container);

        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not find stream info: " << errbuf << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    /* Select the audio stream */
    AVCodec *codec;
    int stream_id;

#if (LIBAVUTIL_VERSION_MAJOR < 58)
    stream_id = av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
#else
    stream_id = av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, const_cast<const AVCodec**>(&codec), 0);
#endif

    if (stream_id < 0)
    {
        avformat_close_input(&container);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not find audio stream!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    /* Create decoding context */
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        avformat_close_input(&container);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not allocate audio codec context!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    avcodec_parameters_to_context(ctx, container->streams[stream_id]->codecpar);

    /* Init the audio decoder */
    rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0)
    {
        avcodec_free_context(&ctx);
        avformat_close_input(&container);

        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not open codec: " << errbuf << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    /* Try to get default channel layout (they aren’t specified in .wav files) */
#if (LIBAVUTIL_VERSION_MAJOR < 58)
    if (!ctx->channel_layout)
        ctx->channel_layout = av_get_default_channel_layout(ctx->channels);
#else
    av_channel_layout_default(&ctx->ch_layout, ctx->ch_layout.nb_channels);
#endif

    /* Show some information about the file, only show bits/sample where it makes sense */
    char infotext[20];
    infotext[0] = '\0';

    if (ctx->bits_per_raw_sample > 0 || ctx->bits_per_coded_sample > 0)
        snprintf(infotext, sizeof(infotext), "%d bit, ", ctx->bits_per_raw_sample > 0 ? ctx->bits_per_raw_sample : ctx->bits_per_coded_sample);

    char infobuf[512];
#if (LIBAVUTIL_VERSION_MAJOR < 58)
    av_get_channel_layout_string(infobuf, sizeof(infobuf), -1, ctx->channel_layout);
#else
    av_channel_layout_describe(&ctx->ch_layout, infobuf, sizeof(infobuf));
#endif

    if (verbose)
    {
        std::stringstream msg;
#if (LIBAVUTIL_VERSION_MAJOR < 58)
        msg << "[" << fileName() << "] " << "Stream #" << stream_id << ": " << codec->long_name << ", " << infotext << " " << ctx->sample_rate << " Hz, " << ctx->channels << " ch, " << infobuf << std::endl;
#else
        msg << "[" << fileName() << "] " << "Stream #" << stream_id << ": " << codec->long_name << ", " << infotext << " " << ctx->sample_rate << " Hz, " << ctx->ch_layout.nb_channels << " ch, " << infobuf << std::endl;
#endif
        std::cout << msg.str();
    }

    avCodecId = codec->id;

    destroyEbuR128State();

#if (LIBAVUTIL_VERSION_MAJOR < 58)
    eburState = ebur128_init(ctx->channels, ctx->sample_rate, EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_TRUE_PEAK);
#else
    eburState = ebur128_init(ctx->ch_layout.nb_channels, ctx->sample_rate, EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_TRUE_PEAK);
#endif

    if (eburState == NULL)
    {
        avcodec_free_context(&ctx);
        avformat_close_input(&container);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not initialize EBU R128 scanner!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    AVFrame *frame = av_frame_alloc();

    if (frame == NULL)
    {
        avformat_close_input(&container);
        avcodec_free_context(&ctx);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not allocate frame!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    SwrContext *swr = swr_alloc();
    AVPacket packet;
    bool ok = true;
    while (av_read_frame(container, &packet) >= 0 && ok)
    {
        if (packet.stream_index == stream_id)
        {
            rc = avcodec_send_packet(ctx, &packet);
            if (rc < 0)
            {
                std::stringstream err;
                err << "[" << fileName() << "] " << "Error while sending a packet to the decoder!" << std::endl;
                std::cerr << err.str();
                ok = false;
                break;
            }

            while (rc >= 0 && ok)
            {
                rc = avcodec_receive_frame(ctx, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                    break;
                else if (rc < 0)
                {
                    std::stringstream err;
                    err << "[" << fileName() << "] " << "Error while receiving a frame from the decoder!" << std::endl;
                    std::cerr << err.str();
                    ok = false;
                    break;
                }

                if (!scanFrame(eburState, frame, swr))
                {
                    std::stringstream err;
                    err << "[" << fileName() << "] " << "Error while scanning frame!" << std::endl;
                    std::cerr << err.str();
                    ok = false;
                    break;
                }
            }

            av_frame_unref(frame);
        }

        av_packet_unref(&packet);
    }

    /* Free */
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&container);

    if (!ok)
    {
        std::stringstream err;
        err << "[" << fileName() << "] " << "Oops!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    /* Save results */
    double global_loudness;
    if (ebur128_loudness_global(eburState, &global_loudness) != EBUR128_SUCCESS)
    {
        std::stringstream err;
        err << "[" << fileName() << "] " << "Error while calculating loudness!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    double loudness_range;
    if (ebur128_loudness_range(eburState, &loudness_range) != EBUR128_SUCCESS)
    {
        std::stringstream err;
        err << "[" << fileName() << "] " << "Error while calculating loudness range!" << std::endl;
        std::cerr << err.str();
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    double peak = 0.0;
    for (unsigned ch = 0; ch < eburState->channels; ch++)
    {
        double tmp;

        if (ebur128_true_peak(eburState, ch, &tmp) == EBUR128_SUCCESS)
            peak = std::max<double>(peak, tmp);
    }

    scanStatus = SCANSTATUS::SUCCESS;

    /* Opus is always based on -23 LUFS, we have to adapt */
    if (avCodecId == AV_CODEC_ID_OPUS)
        pregain -= 5.0;

    trackGain = LUFS_TO_RG(global_loudness) + pregain;
    trackPeak = peak;
    trackLoudness = global_loudness;
    trackLoudnessRange = loudness_range;
    loudnessReference = LUFS_TO_RG(-pregain);

    return true;
}

bool AudioFile::scanFrame(ebur128_state *ebur128, AVFrame *frame, SwrContext *swr)
{

#if (LIBAVUTIL_VERSION_MAJOR < 58)
    int ret = 0;
    swr_alloc_set_opts(swr,
                       frame->channel_layout,  AV_SAMPLE_FMT_S16,  frame->sample_rate, // out_channel
                       frame->channel_layout, (AVSampleFormat) frame->format, frame->sample_rate, // in_channel
                       0, NULL); // log_offset, log_ctx
#else
    int ret = swr_alloc_set_opts2(&swr,
                                  &frame->ch_layout,  AV_SAMPLE_FMT_S16,  frame->sample_rate, // out_channel
                                  &frame->ch_layout, (AVSampleFormat) frame->format, frame->sample_rate, // in_channel
                                  0, NULL); // log_offset, log_ctx
#endif

    int rc = swr_init(swr);
    if (rc < 0 || ret < 0)
    {
        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Could not open SWResample: " << errbuf << std::endl;
        std::cerr << err.str();
        return false;
    }

    int out_linesize;
#if (LIBAVUTIL_VERSION_MAJOR < 58)
    size_t out_size = av_samples_get_buffer_size(&out_linesize, frame->channels, frame->nb_samples, AV_SAMPLE_FMT_S16, 0);
#else
    size_t out_size = av_samples_get_buffer_size(&out_linesize, frame->ch_layout.nb_channels, frame -> nb_samples, AV_SAMPLE_FMT_S16, 0);
#endif
    uint8_t *out_data = (uint8_t *) av_malloc(out_size);

    if (swr_convert(swr, (uint8_t**) &out_data, frame -> nb_samples, (const uint8_t**) frame -> data, frame -> nb_samples) < 0)
    {
        swr_close(swr);
        av_free(out_data);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Cannot convert" <<  std::endl;
        std::cerr << err.str();
        return false;
    }

    rc = ebur128_add_frames_short(ebur128, (short *) out_data, frame -> nb_samples);

    if (rc != EBUR128_SUCCESS)
    {
        swr_close(swr);
        av_free(out_data);

        std::stringstream err;
        err << "[" << fileName() << "] " << "Error filtering" << std::endl;
        std::cerr << err.str();
        return false;
    }

    swr_close(swr);
    av_free(out_data);
    return true;
}



AudioFolder::AudioFolder(const std::vector<fs::path> &files)
{
    if (files.size() == 0)
        exit(EXIT_FAILURE);

    audioFiles.reserve(files.size());

    for (const fs::path &file : files)
        audioFiles.push_back(std::make_shared<AudioFile>(AudioFile(file)));

    if (audioFiles.size() == 0)
    {
        std::stringstream err;
        err << "Empty audio folder!" << std::endl;
        std::cerr << err.str();
        exit(EXIT_FAILURE);
    }
    else
        dir = audioFiles[0]->directory();
}

AudioFolder::~AudioFolder()
{ }

unsigned long long AudioFolder::count()
{
    return audioFiles.size();
}

fs::path AudioFolder::directory()
{
    return dir;
}

std::shared_ptr<AudioFile> AudioFolder::getAudioFile(unsigned long long i)
{
    return audioFiles[i];
}

bool AudioFolder::hasDifferentContainers()
{
    for (unsigned long long i = 1; i < audioFiles.size(); i++)
        if (audioFiles[0]->avFormat.compare(audioFiles[i]->avFormat) != 0)
            return true;
    return false;
}

bool AudioFolder::hasDifferentCodecs()
{
    for (unsigned long long i = 1; i < audioFiles.size(); i++)
        if (audioFiles[0]->avCodecId != audioFiles[i]->avCodecId)
            return true;
    return false;
}

bool AudioFolder::hasOpus()
{
    for (unsigned long long i = 0; i < audioFiles.size(); i++)
        if (audioFiles[i]->avCodecId == AV_CODEC_ID_OPUS)
            return true;
    return false;
}

bool AudioFolder::scanFile(unsigned long long i, double pregain, bool verbose)
{
    if (i < audioFiles.size())
        return audioFiles[i]->scanFile(pregain, verbose);
    return false;
}

bool AudioFolder::scanFolder(double pregain, bool verbose)
{
    for (unsigned long long i = 0; i < audioFiles.size(); i++)
        if (!audioFiles[i]->scanFile(pregain, verbose))
            return false;

    return processScanResults(pregain);
}

bool AudioFolder::processScanResults(double pregain)
{
    for (unsigned long long i = 0; i < audioFiles.size(); i++)
        if (audioFiles[i]->scanStatus != AudioFile::SCANSTATUS::SUCCESS)
            return false;

    unsigned long long nb = audioFiles.size();
    ebur128_state **ebuR128States = (ebur128_state **) malloc(sizeof(ebur128_state *) * nb);

    for (unsigned long long i = 0; i < audioFiles.size(); i++)
        ebuR128States[i] = audioFiles[i]->eburState;

    double global_loudness;
    if (ebur128_loudness_global_multiple(ebuR128States, nb, &global_loudness) != EBUR128_SUCCESS)
    {
        free(ebuR128States);

        std::stringstream err;
        err << "[" << directory() << "] Error while calculating album loudness!" << std::endl;
        std::cerr << err.str();
        return false;
    }

    double loudness_range;
    if (ebur128_loudness_range_multiple(ebuR128States, nb, &loudness_range) != EBUR128_SUCCESS)
    {
        free(ebuR128States);

        std::stringstream err;
        err << "[" << directory() << "] Error while calculating album loudness range!" << std::endl;
        std::cerr << err.str();
        return false;
    }

    free(ebuR128States);

    /* Check for different file (codec) types in an album and warn (including Opus might mess up album gain) */
    if (hasDifferentContainers() || hasDifferentCodecs())
    {
        if (hasOpus())
        {
            std::stringstream err;
            err << "[" << directory() << "] Cannot calculate correct album gain when mixing Opus and non-Opus files!" << std::endl;
            std::cerr << err.str();
            return false;
        }
        else
        {
            std::stringstream err;
            err << "[" << directory() << "] You have different file types in the same album!" << std::endl;
            std::cerr << err.str();
        }
    }

    /* Opus is always based on -23 LUFS, we have to adapt. When we arrive here, it’s already verified that the album
    does NOT mix Opus and non-Opus tracks, so we can safely reduce the pre-gain to arrive at -23 LUFS. */
    if (hasOpus())
        pregain -= 5.0;

    double album_peak = 0.0;
    for (unsigned long long i = 0; i < audioFiles.size(); i++)
        album_peak = std::max<double>(album_peak, audioFiles[i]->trackPeak);

    for (unsigned long long i = 0; i < audioFiles.size(); i++)
    {
        AudioFile &audio = *audioFiles[i].get();
        audio.albumGain = LUFS_TO_RG(global_loudness) + pregain;
        audio.albumPeak = album_peak;
        audio.albumLoudness = global_loudness;
        audio.albumLoudnessRange = loudness_range;
    }
    return true;
}
