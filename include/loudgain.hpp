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
#ifndef LOUDGAIN_H
#define LOUDGAIN_H

#include <string>
#include <fstream>
#include <vector>
#include <set>
#include <map>
#include <scan.hpp>
#include <tag.hpp>


class LoudGain
{
public:
    int  verbosity = 1;
    bool scanAlbum = false;
    bool tabOutput = false;
    bool preventClipping = true;
    bool stripTags = false;
    bool lowerCaseTags = false;
    bool warnClipping = true;
    bool skipTaggedFiles = false;
    int id3v2Version = 4;
    double maxTruePeakLevel = -1.0;
    double pregain = 0.0;
    char tagMode = 's';
    char scanMode = 'u';
    char unit[3] = "dB";
    unsigned numberOfThreads = 1;
    std::ofstream csvfile;
    char dc = '.';
    char sp = ',';
    bool recursive = false;
    std::vector<fs::path> libraryPaths;
    const std::vector<std::string> supportedExtensions = {".mp3", ".flac", ".ogg", ".mov", ".mp4", ".m4a", ".alac", ".aac", ".3gp", ".3g2", ".mj2", ".asf", ".wma", ".wav", ".wv", ".aif" , ".aiff", ".ape"};
    std::vector<std::string> userExtensions;
    RGTagManager tagManager;

    LoudGain();
    ~LoudGain();

    static void version();
    void setAlbumScanMode(bool enable);
    void setVerbosity(int level);
    void setTagMode(const char tagmode);
    void setSkipTaggedFiles(bool skip);
    void setUnitToLUFS(bool enable);
    void setPregain(double gain);
    void setWarnClipping(bool enable);
    void setPreventClipping(bool enable);
    void setMaxTruePeakLevel(double mtpl);
    void setForceLowerCaseTags(bool enable);
    void setStripTags(bool enable);
    void setID3v2Version(int version);
    void setTabOutput(bool enable);
    void openCsvFile(const fs::path &file);
    void closeCsvFile();
    void setNumberOfThreads(unsigned n);

    void setLibraryPaths(const std::vector<std::string> &paths);
    void setRecursiveDirectoryScan(bool enable);
    void setUserExtensions(const std::string &extensions);
    bool isOnlyDirectories(const std::vector<fs::path> &paths);
    bool isSupportedAudioFile(const fs::path &path);
    std::set<fs::path> getSupportedAudioFiles();
    std::map<std::string, std::shared_ptr<std::vector<fs::path>>> getSupportedAudioFilesSortedByFolder();

    void removeReplayGainTags(const fs::path &path);
    void processLibrary();
    void processAudioFile(const fs::path &path);
    void processAudioFile(AudioFile &audio_file);
    void processAudioFolder(std::vector<fs::path> &paths);
    void processAudioFolder(AudioFolder &audio_folder);
};

#endif
