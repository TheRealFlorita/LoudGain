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
#ifndef SCAN_H
#define SCAN_H

#include <string>
#include <vector>
#include <memory>
#include <filesystem>

extern "C" {
#include <ebur128.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;

class AudioFile
{
public:
    enum SCANSTATUS
    {
        NONE,
        FAIL,
        SUCCESS
    };

    enum SCANSTATUS scanStatus = SCANSTATUS::NONE;
    enum AVCodecID avCodecId = AV_CODEC_ID_NONE;
    std::string avFormat = "";
    double trackGain = 0.0;
    double trackPeak = 0.0;
    double newTrackPeak = 0.0;
    double trackLoudness = 0.0;
    double trackLoudnessRange = 0.0;
    bool trackClips = false;
    double albumGain = 0.0;
    double albumPeak = 0.0;
    double newAlbumPeak = 0.0;
    double albumLoudness = 0.0;
    double albumLoudnessRange = 0.0;
    bool albumClips = false;
    double loudnessReference = 0.0;
    bool trackClipPrevention = false;
    bool albumClipPrevention = false;
    ebur128_state *eburState = NULL;

    AudioFile(const fs::path &path);
    ~AudioFile();

    bool destroyEbuR128State();
    fs::path filePath();
    fs::path fileName();
    fs::path directory();
    bool initFile();
    bool scanFile(double pregain, bool verbose);

#if defined(_WIN32) || defined(WIN32)
    wchar_t* getTagLibFilePath();
#else
    const char* getTagLibFilePath();
#endif

private:
    fs::path p;
    std::wstring fp;
    bool scanFrame(ebur128_state *ebur128, AVFrame *frame, SwrContext *swr);

};


class AudioFolder
{
private:
    fs::path dir;
    std::vector<std::shared_ptr<AudioFile>> audioFiles;

public:

    AudioFolder(const std::vector<fs::path> &files);
    ~AudioFolder();
    unsigned long long count();
    fs::path directory();
    std::shared_ptr<AudioFile> getAudioFile(unsigned long long i);
    bool hasDifferentContainers();
    bool hasDifferentCodecs();
    bool hasOpus();
    bool scanFile(unsigned long long i, double pregain, bool verbose);
    bool scanFolder(double pregain, bool verbose);
    bool processScanResults(double pregain);
};

#endif

