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
#include <locale>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <thread>
#include <loudgain.hpp>
#include <tag.hpp>
#include <config.h>
#include <taglib/taglib.h>
#include <mvthreadpool/mvThreadPool.h>


std::string num2str(double val, int precision, char decimal)
{
    std::stringstream stream;
    stream.imbue(std::locale("C"));
    stream << std::fixed << std::setprecision(precision) << val;
    std::string s = stream.str();

    if (decimal != '.')
        std::replace( s.begin(), s.end(), '.', decimal);
    return s;
}

void scan_av_log(void *avcl, int level, const char *fmt, va_list args)
{
    (void)avcl; (void)level; (void)fmt; (void)args;
}


LoudGain::LoudGain()
{
    userExtensions = supportedExtensions;

#if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100) )
    av_register_all();
#endif

    av_log_set_callback(scan_av_log);

#if defined(_OS_WIN32_)
    TCHAR szSep[8];
    GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, szSep, 8);
    dc = char(szSep[0]);
#else
    std::stringstream str;
    str.imbue(std::locale(""));
    dc = std::use_facet< std::numpunct<char> >(str.getloc()).decimal_point();
#endif

    if (dc == ',')
        sp = ';';
}

LoudGain::~LoudGain()
{
    closeCsvFile();
}

void LoudGain::version()
{
    /* Libebur128 version */
    int ebur128_v_major = 0, ebur128_v_minor = 0, ebur128_v_patch = 0;
    char ebur128_version[15] = "";
    ebur128_get_version(&ebur128_v_major, &ebur128_v_minor, &ebur128_v_patch);
    snprintf(ebur128_version, sizeof(ebur128_version), "%d.%d.%d", ebur128_v_major, ebur128_v_minor, ebur128_v_patch);

    /* Libavformat version */
    unsigned lavf_ver = 0;
    char lavf_version[15] = "";
    lavf_ver = avformat_version();
    snprintf(lavf_version, sizeof(lavf_version), "%u.%u.%u", lavf_ver>>16, lavf_ver>>8&0xff, lavf_ver&0xff);

    /* Libswresample version */
    unsigned swr_ver = 0;
    char swr_version[15] = "";
    swr_ver = swresample_version();
    snprintf(swr_version, sizeof(swr_version), "%u.%u.%u", swr_ver>>16, swr_ver>>8&0xff, swr_ver&0xff);

    /* Taglib version */
    char tlib_version[15] = "";
    snprintf(tlib_version, sizeof(tlib_version), "%d.%d.%d", TAGLIB_MAJOR_VERSION, TAGLIB_MINOR_VERSION, TAGLIB_PATCH_VERSION);

    printf("%s %s - using:\n", PROJECT_NAME, PROJECT_VER);
    printf("  %s %s\n", "libebur128", ebur128_version);
    printf("  %s %s\n", "libavformat", lavf_version);
    printf("  %s %s\n", "libswresample", swr_version);
    printf("  %s %s\n", "taglib", tlib_version);
}

void LoudGain::setTagMode(const char tagmode)
{
    std::string valid_modes = "dies";
    if (valid_modes.find(tagmode) == std::string::npos)
    {
        std::cerr << "Invalid tag mode: " << tagmode  << std::endl;
        exit(EXIT_FAILURE);
    }
    tagMode = tagmode;
}

void LoudGain::setUnitToLUFS(bool enable)
{
    if (enable)
        snprintf(unit, sizeof(unit), "LU"); //strcpy(unit, "LU");
    else
        snprintf(unit, sizeof(unit), "dB"); //strcpy(unit, "dB");
}

void LoudGain::setVerbosity(int level)
{
    verbosity = level;
}

void LoudGain::setAlbumScanMode(bool enable)
{
    scanAlbum = enable;
}

void LoudGain::setSkipTaggedFiles(bool skip)
{
    skipTaggedFiles = skip;
}

void LoudGain::setPregain(double gain)
{
    pregain = std::clamp<double>(gain, -32.0, 32.0);
}

void LoudGain::setWarnClipping(bool enable)
{
    warnClipping = enable;
}

void LoudGain::setPreventClipping(bool enable)
{
    preventClipping = enable;
}

void LoudGain::setMaxTruePeakLevel(double mtpl)
{
    preventClipping = true;
    maxTruePeakLevel = std::clamp<double>(mtpl, -32.0, 32.0);
}

void LoudGain::setID3v2Version(int version)
{
    id3v2Version = std::clamp<int>(version, 3, 4);
}

void LoudGain::setForceLowerCaseTags(bool enable)
{
    lowerCaseTags = enable;
}

void LoudGain::setStripTags(bool enable)
{
    stripTags = enable;
}

void LoudGain::setTabOutput(bool enable)
{
    tabOutput = enable;
}

void LoudGain::openCsvFile(const fs::path &file)
{
    closeCsvFile();

    csvfile.open(file);

    if (!csvfile.is_open())
    {
        std::cerr << "Failed to open file: '" << file << "'" << std::endl;
        exit(EXIT_FAILURE);
    }

    /* Set default number format */
    csvfile << std::fixed << std::setprecision(2);

    /* Write headers */
    csvfile << "Type" << sp << "Location" << sp << "Loudness [LUFs]" << sp << "Range [" << unit << "]" << sp
            << "True Peak" << sp << "True Peak [dBTP]" << sp << "Reference [LUFs]" << sp << "Will clip" << sp
            << "Clip prevent" << sp << "Gain [" << unit << "]" << sp << "New Peak" << sp << "New Peak [dBTP]" << std::endl;
}

void LoudGain::closeCsvFile()
{
    if (csvfile.is_open())
    {
        csvfile.flush();
        csvfile.close();
    }
}

void LoudGain::setNumberOfThreads(unsigned n)
{
    unsigned mthrds = std::thread::hardware_concurrency();

    if (n == 0)
        numberOfThreads = mthrds - 1;
    else
        numberOfThreads = std::min<unsigned>(n, mthrds);
}

void LoudGain::setLibraryPaths(const std::vector<std::string> &paths)
{
    libraryPaths.clear();
    for (const std::string &path : paths)
        libraryPaths.push_back(fs::path(path).make_preferred());
}

void LoudGain::setRecursiveDirectoryScan(bool enable)
{
    recursive = enable;
}

void LoudGain::setUserExtensions(const std::string &extensions)
{
    std::vector<std::string> exts;
    std::istringstream f(extensions);
    std::string s;
    while (getline(f, s, ','))
    {
        if (s.length() >= 2 && s[0] != '.')
            exts.push_back("."+s);
        else if (s.length() >= 2 && s[0] == '.')
            exts.push_back(s);
    }

    userExtensions.clear();

    for(const std::string& ext: exts)
        if (std::find(supportedExtensions.begin(), supportedExtensions.end(), ext) != supportedExtensions.end())
            userExtensions.push_back(ext);
}

bool LoudGain::isOnlyDirectories(const std::vector<fs::path> &paths)
{
    for(const fs::path& path: paths)
    {
        std::error_code ec;
        if (!fs::is_directory(path, ec))
            return false;
    }
    return true;
}

bool LoudGain::isSupportedAudioFile(const fs::path &path)
{
    return (fs::is_regular_file(path) && (std::find(userExtensions.begin(), userExtensions.end(), path.extension()) != userExtensions.end()));
}

std::set<fs::path> LoudGain::getSupportedAudioFiles()
{
    std::set<fs::path> audio_files;

    if (isOnlyDirectories(libraryPaths))
    {
        if (verbosity > 0 && !recursive)
            std::cout << "Scanning directories for audio files..." << std::endl;
        else if (verbosity > 0 && recursive)
            std::cout << "Scanning directories recursively for audio files..." << std::endl;

        for (const fs::path& path : libraryPaths)
        {
            if (recursive)
            {
                fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied);
                for(const fs::directory_entry &entry : it)
                    if (isSupportedAudioFile(entry.path()))
                        audio_files.insert(entry.path());
            }
            else
            {
                fs::directory_iterator it(path, fs::directory_options::skip_permission_denied);
                for(const fs::directory_entry &entry : it)
                    if (isSupportedAudioFile(entry.path()))
                        audio_files.insert(entry.path());
            }
        }
    }
    else
    {
        if (verbosity > 0)
            std::cout << "Scanning audio files..." << std::endl;

        for (const fs::path& path : libraryPaths)
            if (isSupportedAudioFile(path))
                audio_files.insert(path);
    }

    if (skipTaggedFiles && tagMode != 'd')
    {
        if (verbosity > 0)
            std::cout << "Scanning audio files for ReplayGain tags..." << std::endl;

        bool ok;
        std::set<fs::path> scan_files;
        for (const fs::path &path : audio_files)
            if (!tagManager.hasRGTags(path, scanAlbum, tagMode, ok))
                scan_files.insert(path);

        return scan_files;
    }

    return audio_files;
}

std::map<std::string, std::shared_ptr<std::vector<fs::path>>> LoudGain::getSupportedAudioFilesSortedByFolder()
{
    std::set<fs::path> files = getSupportedAudioFiles();
    std::map<std::string, std::shared_ptr<std::vector<fs::path>>> sorted;

    for (const fs::path &file : files)
    {
        const std::string dir = fs::path(file).parent_path().string();

        if ((sorted.find(dir) == sorted.end()))
        {
            std::shared_ptr<std::vector<fs::path>> shared_ptr_dir = std::make_shared<std::vector<fs::path>>(std::vector<fs::path>());
            shared_ptr_dir->reserve(16);
            sorted.insert(std::make_pair(dir, std::move(shared_ptr_dir)));
        }

        sorted[dir]->push_back(file);
    }

    return sorted;
}

void LoudGain::processLibrary()
{
    unsigned nthreads = std::max<unsigned>(1, numberOfThreads);

    if (tagMode == 'd')
    {
        std::set<fs::path> files = getSupportedAudioFiles();

        if (verbosity > 0 && files.size() > 0)
            std::cout << "Deleting ReplayGain tags..." << std::endl;
        else if (verbosity > 0 && files.size() == 0)
            std::cout << "No audio files found" << std::endl;

        Marvel::mvThreadPool threadpool = Marvel::mvThreadPool(nthreads);
        for (const fs::path &file : files)
            threadpool.submit(std::bind(&LoudGain::removeReplayGainTags, this, file));
        threadpool.wait_for_finished();
    }
    else
    {
        if (scanAlbum)
        {
            std::map<std::string, std::shared_ptr<std::vector<fs::path>>> sorted_audio_files = getSupportedAudioFilesSortedByFolder();

            if (verbosity > 0 && sorted_audio_files.size() > 0)
                std::cout << "Analysing audio files..." << std::endl;
            else if (verbosity > 0 && sorted_audio_files.size() == 0)
                std::cout << "No audio files to analyse" << std::endl;

            if (tabOutput)
                std::cout << "File\tLoudness\tRange\tTrue_Peak\tTrue_Peak_dBTP\tReference\tWill_clip\tClip_prevent\tGain\tNew_Peak\tNew_Peak_dBTP" << std::endl;

            if (sorted_audio_files.size() > 5*nthreads)
            {
                std::vector<std::shared_ptr<AudioFolder>> audio_folders;

                Marvel::mvThreadPool threadpool = Marvel::mvThreadPool(nthreads);
                std::map<std::string, std::shared_ptr<std::vector<fs::path>>>::iterator it;
                for (it = sorted_audio_files.begin(); it != sorted_audio_files.end(); it++)
                {
                    if (it->second->size() <= 1000)
                        threadpool.submit(std::bind(static_cast<void(LoudGain::*)(std::vector<fs::path>&)>(&LoudGain::processAudioFolder), this, *it->second.get()));
                    else
                    {
                        std::shared_ptr<AudioFolder> audio_folder = std::make_shared<AudioFolder>(AudioFolder(*it->second));
                        for (unsigned long long i = 0; i < audio_folder->count(); i++)
                            threadpool.submit(std::bind(&AudioFolder::scanFile, audio_folder, i, pregain, (verbosity >= 3)));
                        threadpool.wait_for_idle();
                        processAudioFolder(*audio_folder.get());
                    }
                }
                threadpool.wait_for_finished();
            }
            else
            {
                std::vector<std::shared_ptr<AudioFolder>> audio_folders;
                audio_folders.reserve(sorted_audio_files.size());

                Marvel::mvThreadPool threadpool = Marvel::mvThreadPool(nthreads);
                std::map<std::string, std::shared_ptr<std::vector<fs::path>>>::iterator it;
                unsigned long long tr_count = 0;
                for (it = sorted_audio_files.begin(); it != sorted_audio_files.end(); it++)
                {
                    std::shared_ptr<AudioFolder> audio_folder = std::make_shared<AudioFolder>(AudioFolder(*it->second));
                    tr_count += audio_folder->count();
                    if (tr_count >= 2000)
                    {
                        threadpool.wait_for_idle();
                        for (unsigned long long i = 0; i < audio_folders.size(); ++i)
                            processAudioFolder(*audio_folders[i].get());
                        audio_folders.clear();
                        tr_count = audio_folder->count();
                    }

                    audio_folders.push_back(audio_folder);
                    for (unsigned long long i = 0; i < audio_folder->count(); i++)
                        threadpool.submit(std::bind(&AudioFolder::scanFile, audio_folder, i, pregain, (verbosity >= 3)));
                }
                threadpool.wait_for_finished();

                for (unsigned long long i = 0; i < audio_folders.size(); ++i)
                    processAudioFolder(*audio_folders[i].get());
            }
        }
        else
        {
            std::set<fs::path> files = getSupportedAudioFiles();

            if (verbosity > 0 && files.size() > 0)
                std::cout << "Analysing audio files..." << std::endl;
            else if (verbosity > 0 && files.size() == 0)
                std::cout << "No audio files to analyse" << std::endl;

            if (tabOutput)
                std::cout << "File\tLoudness\tRange\tTrue_Peak\tTrue_Peak_dBTP\tReference\tWill_clip\tClip_prevent\tGain\tNew_Peak\tNew_Peak_dBTP" << std::endl;

            Marvel::mvThreadPool threadpool = Marvel::mvThreadPool(nthreads);
            for (const fs::path &file : files)
                threadpool.submit(std::bind(static_cast<void(LoudGain::*)(const fs::path&)>(&LoudGain::processAudioFile), this, file));
            threadpool.wait_for_finished();
        }
    }
}

void LoudGain::processAudioFile(const fs::path &path)
{
    AudioFile audio_file = AudioFile(path);
    audio_file.scanFile(pregain, (verbosity >= 3));
    processAudioFile(audio_file);
}

void LoudGain::processAudioFile(AudioFile &audio_file)
{
    /* Check if scan was successfull */
    if (audio_file.scanStatus != AudioFile::SCANSTATUS::SUCCESS)
    {
        std::stringstream err;
        err << "File scan failed [" << audio_file.fileName() <<"]!" << std::endl;
        std::cerr << err.str();
        return;
    }

    double tgain    = 1.0; // "gained" track peak
    double tpeak    = pow(10.0, maxTruePeakLevel / 20.0); // track peak limit
    double again    = 1.0; // "gained" album peak
    double apeak    = pow(10.0, maxTruePeakLevel / 20.0); // album peak limit

    // track peak after gain
    tgain = pow(10.0, audio_file.trackGain / 20.0) * audio_file.trackPeak;
    if (tgain > tpeak)
        audio_file.trackClips = true;

    // album peak after gain
    if (scanAlbum)
    {
        again = pow(10.0, audio_file.albumGain / 20.0) * audio_file.albumPeak;
        if (again > apeak)
            audio_file.albumClips = true;
    }

    // prevent clipping
    if ((audio_file.trackClips || audio_file.albumClips) && preventClipping)
    {
        if (audio_file.trackClips)
        {
            // set new track peak = minimum of peak after gain and peak limit
            audio_file.trackGain = audio_file.trackGain - (log10(tgain/tpeak) * 20.0);
            audio_file.trackClips = false;
            audio_file.trackClipPrevention = true;
        }

        if (scanAlbum && audio_file.albumClips)
        {
            audio_file.albumGain = audio_file.albumGain - (log10(again/apeak) * 20.0);
            audio_file.albumClips = false;
            audio_file.albumClipPrevention = true;
        }
    }

    audio_file.newTrackPeak = pow(10.0, audio_file.trackGain / 20.0) * audio_file.trackPeak;
    if (scanAlbum)
        audio_file.newAlbumPeak = pow(10.0, audio_file.albumGain / 20.0) * audio_file.albumPeak;

    if (tagMode == 'i' || tagMode == 'e')
        tagManager.writeRGTags(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags, stripTags, id3v2Version);

    if (csvfile.is_open())
    {
        std::stringstream line;
        line    << "File" << sp << "\"" << audio_file.filePath() << "\"" << sp
                << num2str(audio_file.trackLoudness, 2, dc) << sp
                << num2str(audio_file.trackLoudnessRange, 2, dc) << sp
                << num2str(audio_file.trackPeak, 6, dc) << sp
                << num2str(20.0 * log10(audio_file.trackPeak), 2, dc) << sp
                << num2str(audio_file.loudnessReference, 2, dc)  << sp
                << (audio_file.trackClips ? "Y" : "N") << sp
                << (audio_file.trackClipPrevention ? "Y" : "N") << sp
                << num2str(audio_file.trackGain, 2, dc) << sp
                << num2str(audio_file.newTrackPeak, 6, dc) << sp
                << num2str(20.0 * log10(audio_file.newTrackPeak), 2, dc) << std::endl;

        csvfile << line.str();
    }

    if (tabOutput)
    {
        // output new style list: File;Loudness;Range;Gain;Reference;Peak;Peak dBTP;Clipping;Clip-prevent
        {
            std::stringstream line;
            line << audio_file.filePath() << "\t"
                 << std::fixed << std::setprecision(2) << audio_file.trackLoudness << " LUFS\t"
                 << audio_file.trackLoudnessRange << " " << unit << "\t"
                 << std::setprecision(6) << audio_file.trackPeak << "\t"
                 << std::setprecision(2) << 20.0 * log10(audio_file.trackPeak) << " dBTP\t"
                 << audio_file.loudnessReference << " LUFS\t"
                 << (audio_file.trackClips ? "Y\t" : "N\t")
                 << (audio_file.trackClipPrevention ? "Y\t" : "N\t")
                 << audio_file.trackGain << " " << unit << "\t"
                 << std::setprecision(6) << audio_file.newTrackPeak << "\t"
                 << std::setprecision(2) << 20.0 * log10(audio_file.newTrackPeak) << " dBTP" << std::endl;
            std::cout << line.str();
        }
    }
    else if (verbosity >= 2)
    {
        // output something human-readable
        std::stringstream msg;
        msg << "\nTrack: "   << audio_file.filePath() << "\n"
            << " Loudness: " << std::fixed << std::setprecision(2) << audio_file.trackLoudness << " LUFS\n"
            << " Range:    " << audio_file.trackLoudnessRange << " dB\n"
            << " Peak:     " << std::setprecision(6) << audio_file.newTrackPeak << std::setprecision(2) << " (" << 20.0 * log10(audio_file.newTrackPeak) << " dBTP)\n";

        if (audio_file.avCodecId == AV_CODEC_ID_OPUS)
            msg << " Gain:     " << std::setprecision(2) << audio_file.trackGain << " dB ("  << gain_to_q78num(audio_file.trackGain) << ")" << (audio_file.trackClipPrevention ? " (corrected to prevent clipping)" : "") << std::endl;
        else
            msg << " Gain:     " << std::setprecision(2) << audio_file.trackGain <<  " dB" << (audio_file.trackClipPrevention ? " (corrected to prevent clipping)" : "") << std::endl;

        std::cout << msg.str();
    }
}

void LoudGain::processAudioFolder(std::vector<fs::path> &paths)
{
    AudioFolder audio_folder = AudioFolder(paths);
    audio_folder.scanFolder(pregain, (verbosity >= 3));
    processAudioFolder(audio_folder);
}

void LoudGain::processAudioFolder(AudioFolder &audio_folder)
{
    /* Check if scan results have been processed successfully */
    if (!audio_folder.processScanResults(pregain))
    {
        std::stringstream err;
        err << "Album scan failed [" << audio_folder.directory() <<"]!" << std::endl;

        for (unsigned long long i = 0; i < audio_folder.count(); i++)
            if (audio_folder.getAudioFile(i)->scanStatus != AudioFile::SCANSTATUS::SUCCESS)
                err << "\tFile scan failed [" << audio_folder.getAudioFile(i).get()->fileName() <<"]!" << std::endl;

        std::cerr << err.str();
        return;
    }

    for (unsigned long long i = 0; i < audio_folder.count(); i++)
    {
        AudioFile &audio_file = *(audio_folder.getAudioFile(i).get());
        processAudioFile(audio_file);

        if (i == (audio_folder.count() - 1) && scanAlbum)
        {
            if (csvfile.is_open())
            {
                std::stringstream line;
                line    << "Album" << sp << "\"" << audio_file.directory() << "\"" << sp
                        << num2str(audio_file.albumLoudness, 2, dc) << sp
                        << num2str(audio_file.albumLoudnessRange, 2, dc) << sp
                        << num2str(audio_file.albumPeak, 6, dc) << sp
                        << num2str(20.0 * log10(audio_file.albumPeak), 2, dc) << sp
                        << num2str(audio_file.loudnessReference , 2, dc) << sp
                        << (audio_file.albumClips ? "Y" : "N") << sp
                        << (audio_file.albumClipPrevention ? "Y" : "N") << sp
                        << num2str(audio_file.albumGain, 2, dc) << sp
                        << num2str(audio_file.newAlbumPeak, 6, dc) << sp
                        << num2str(20.0 * log10(audio_file.newAlbumPeak), 2, dc) << std::endl;
                csvfile << line.str();
            }

            if (tabOutput)
            {
                std::stringstream line;
                line << "Album\t"
                     << std::fixed << std::setprecision(2) << audio_file.albumLoudness << " LUFS\t"
                     << audio_file.albumLoudnessRange << " " << unit << "\t"
                     << std::setprecision(6) << audio_file.albumPeak << "\t"
                     << std::setprecision(2) << 20.0 * log10(audio_file.albumPeak) << " dBTP\t"
                     << audio_file.loudnessReference << " LUFS\t"
                     << (audio_file.albumClips ? "Y\t" : "N\t")
                     << (audio_file.albumClipPrevention ? "Y\t" : "N\t")
                     << audio_file.albumGain << " " << unit << "\t"
                     << std::setprecision(6) << audio_file.newAlbumPeak << "\t"
                     << std::setprecision(2) << 20.0 * log10(audio_file.newAlbumPeak) << " dBTP" << std::endl;
                std::cout << line.str();
            }
            else  if (verbosity >= 2)
            {
                // output something human-readable
                std::stringstream msg;
                msg << "\nAlbum: "   << audio_file.directory() << "\n"
                    << " Loudness: " << std::fixed << std::setprecision(2) << audio_file.albumLoudness << " LUFS\n"
                    << " Range:    " << audio_file.albumLoudnessRange << " dB\n"
                    << " Peak:     " << std::setprecision(6) << audio_file.newAlbumPeak << std::setprecision(2) << " (" << 20.0 * log10(audio_file.newAlbumPeak) << " dBTP)\n"
                    << " Gain:     " << std::setprecision(2) << audio_file.albumGain <<  " dB" << (audio_file.albumClipPrevention ? " (corrected to prevent clipping)" : "") << std::endl;

                std::cout << msg.str();
            }
        }
    }
}

void LoudGain::removeReplayGainTags(const fs::path &path)
{
    if (tagMode == 'd')
        return;

    AudioFile audio_file = AudioFile(path);
    if (audio_file.initFile())
        tagManager.clearRGTags(&audio_file, stripTags, id3v2Version);
}
