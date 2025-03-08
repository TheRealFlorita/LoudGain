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
#include <sstream>
#include <iostream>
#include <math.h>
#include <scan.hpp>
#include <tag.hpp>

#include <taglib/taglib.h>
#include <taglib/fileref.h>
#include <taglib/textidentificationframe.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/apetag.h>
#include <taglib/flacfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/oggflacfile.h>
#include <taglib/speexfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/mp4file.h>
#include <taglib/opusfile.h>
#include <taglib/asffile.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/wavpackfile.h>
#include <taglib/apefile.h>
#include <taglib/tpropertymap.h>
#include <set>

#define UNUSED(x) (void)x
#define TAGLIB_VERSION (TAGLIB_MAJOR_VERSION * 10000 + TAGLIB_MINOR_VERSION * 100 + TAGLIB_PATCH_VERSION)

void printFileError(fs::path path)
{
    std::stringstream err;
    err << "Cannot open or read: " << TagLib::String(path) << std::endl;
    std::cerr << err.str();
    return;
}

void printWriteError(fs::path path)
{
    std::stringstream err;
    err << "Cannot write to: " << TagLib::String(path) << std::endl;
    std::cerr << err.str();
    return;
}

void printFormatError(fs::path path)
{
    std::stringstream err;
    err << "Cannot determine file format: " << TagLib::String(path) << std::endl;
    std::cerr << err.str();
}

void printCodecError(const AudioFile *audio_file)
{
    std::stringstream err;
    err << "Codec " << audio_file->avCodecId << " in " << audio_file->avFormat << " not supported" << std::endl;
    std::cerr << err.str();
}

void printTypeError(const AudioFile *audio_file)
{
    std::stringstream err;
    err << "File type not supported: " << audio_file->avFormat << std::endl;
    std::cerr << err.str();
}

int str_compare(const TagLib::String &s1, const TagLib::String &s2)
{
    if (s1 == s2)
        return 0;
    else if (s1.upper() == s2.upper())
        return 1;
    return -1;
}

int str_compare(const TagLib::String &s1, const char* c2)
{
    const TagLib::String s2(c2);
    return str_compare(s1,s2);
}

// define possible replaygain tags
enum RG_ENUM {
    RG_TRACK_GAIN,
    RG_TRACK_PEAK,
    RG_TRACK_RANGE,
    RG_ALBUM_GAIN,
    RG_ALBUM_PEAK,
    RG_ALBUM_RANGE,
    RG_REFERENCE_LOUDNESS
};

static const char *RG_STRING_UPPER[] = {
    "REPLAYGAIN_TRACK_GAIN",
    "REPLAYGAIN_TRACK_PEAK",
    "REPLAYGAIN_TRACK_RANGE",
    "REPLAYGAIN_ALBUM_GAIN",
    "REPLAYGAIN_ALBUM_PEAK",
    "REPLAYGAIN_ALBUM_RANGE",
    "REPLAYGAIN_REFERENCE_LOUDNESS"
};

static const char *RG_STRING_LOWER[] = {
    "replaygain_track_gain",
    "replaygain_track_peak",
    "replaygain_track_range",
    "replaygain_album_gain",
    "replaygain_album_peak",
    "replaygain_album_range",
    "replaygain_reference_loudness"
};

// this is where we store the RG tags in MP4/M4A files
static const char *RG_ATOM = "----:com.apple.iTunes:";


/*** MP3 ****/
static void tag_add_txxx(TagLib::ID3v2::Tag *tag, const char *name, const char *value)
{
    TagLib::ID3v2::UserTextIdentificationFrame *frame = new TagLib::ID3v2::UserTextIdentificationFrame;
    frame->setDescription(name);
    frame->setText(value);
    tag->addFrame(frame);
}

bool tags_present_id3v2(TagLib::ID3v2::Tag *tag, bool do_album, char mode)
{
    bool extended = (mode == 'e');
    std::set<TagLib::String> rgtags;

    TagLib::ID3v2::FrameList::Iterator it;
    TagLib::ID3v2::FrameList frames = tag->frameList("TXXX");

    for (it = frames.begin(); it != frames.end(); ++it)
    {
        TagLib::ID3v2::UserTextIdentificationFrame *frame = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(*it);

        // this updates all variants of upper-/lower-/mixed-case tags
        if (frame && frame->fieldList().size() >= 2)
        {
            TagLib::String desc = frame->description().upper();
            if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                    (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                    (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                    (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
            {
                rgtags.insert(desc);
            }
        }
    }

    if (!do_album && !extended)
        return (rgtags.size() == 2);
    else if (!do_album && extended)
        return (rgtags.size() == 4);
    else if (do_album && !extended)
        return (rgtags.size() == 4);
    return (rgtags.size() == 7);
}

bool tag_update_txxx(TagLib::ID3v2::Tag *tag, const char* key, const char* value)
{
    bool updated = false;
    bool found = false;
    TagLib::ID3v2::FrameList::Iterator it;
    TagLib::ID3v2::FrameList frames = tag->frameList("TXXX");

    for (it = frames.begin(); it != frames.end(); ++it)
    {
        TagLib::ID3v2::UserTextIdentificationFrame *frame = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(*it);

        // this updates all variants of upper-/lower-/mixed-case tags
        if (frame && frame->fieldList().size() >= 2)
        {
            TagLib::String desc = frame->description();
            int cmp = str_compare(desc,key);

            if (cmp >= 0)
                found = true;

            if (cmp == 1)
            {
                updated = true;
                frame->setDescription(key);
                frame->setText(value);
            }
            else if ((cmp == 0) && ((frame->fieldList().size() != 2) || (frame->fieldList()[1] != value)))
            {
                updated = true;
                frame->setText(value);
            }
        }
    }


    if (!found)
    {
        updated = true;
        tag_add_txxx(tag, key, value);
    }
    return updated;
}

bool tag_remove_album_id3v2(TagLib::ID3v2::Tag *tag)
{
    bool rm = false;
    TagLib::ID3v2::FrameList::Iterator it;
    TagLib::ID3v2::FrameList frames = tag->frameList("TXXX");

    for (it = frames.begin(); it != frames.end(); ++it)
    {
        TagLib::ID3v2::UserTextIdentificationFrame *frame = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(*it);

        // this removes all variants of upper-/lower-/mixed-case tags
        if (frame && frame->fieldList().size() >= 2)
        {
            TagLib::String desc = frame->description().upper();

            if ((desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                    (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]))
            {
                rm = true;
                tag->removeFrame(frame);
            }
        }
    }
    return rm;
}

bool tag_remove_extra_id3v2(TagLib::ID3v2::Tag *tag)
{
    bool rm = false;
    TagLib::ID3v2::FrameList::Iterator it;
    TagLib::ID3v2::FrameList frames = tag->frameList("TXXX");

    for (it = frames.begin(); it != frames.end(); ++it)
    {
        TagLib::ID3v2::UserTextIdentificationFrame *frame = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(*it);

        // this removes all variants of upper-/lower-/mixed-case tags
        if (frame && frame->fieldList().size() >= 2)
        {
            TagLib::String desc = frame->description().upper();

            if ((desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                    (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
            {
                rm = true;
                tag->removeFrame(frame);
            }
        }
    }
    return rm;
}

bool tag_remove_id3v2(TagLib::ID3v2::Tag *tag)
{
    bool rm = false;
    TagLib::ID3v2::FrameList::Iterator it;
    TagLib::ID3v2::FrameList frames = tag->frameList("TXXX");

    for (it = frames.begin(); it != frames.end(); ++it)
    {
        TagLib::ID3v2::UserTextIdentificationFrame *frame = dynamic_cast<TagLib::ID3v2::UserTextIdentificationFrame*>(*it);

        // this removes all variants of upper-/lower-/mixed-case tags
        if (frame && frame->fieldList().size() >= 2)
        {
            TagLib::String desc = frame->description().upper();

            if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                    (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                    (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                    (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
            {
                rm = true;
                tag->removeFrame(frame);
            }
        }
    }
    return rm;
}

// Even if the ReplayGain 2 standard proposes replaygain tags to be uppercase,
// unfortunately some players only respect the lowercase variant (still).
// So we use the "lowercase" flag to switch.
bool tag_present_mp3(AudioFile *audio_file, bool do_album, char mode)
{
#ifdef __unix__
    TagLib::MPEG::File f(audio_file->filePath().c_str());
#else
    TagLib::MPEG::File f(audio_file->getTagLibFilePath());
#endif

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ID3v2::Tag *tag = f.ID3v2Tag(true);
    return tags_present_id3v2(tag, do_album, mode);
}

bool tag_write_mp3(AudioFile *audio_file, bool do_album, char mode, char *unit,
                   bool lowercase, bool strip, int id3v2version)
{
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    if (lowercase)
        RG_STRING = RG_STRING_LOWER;

    TagLib::MPEG::File f(audio_file->getTagLibFilePath());
    bool save = !f.hasID3v2Tag();
    TagLib::ID3v2::Tag *tag = f.ID3v2Tag(true);

    // remove old tags before writing new ones
    //tag_remove_mp3(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_GAIN], value) || save);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_id3v2(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_txxx(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_id3v2(tag) || save);

    // work around bug taglib/taglib#913: strip APE before ID3v1
    if (strip && f.hasAPETag())
    {
        f.strip(TagLib::MPEG::File::APE);
        save = true;
    }

#if TAGLIB_VERSION >= 11200
    if (save || (strip && f.hasID3v1Tag()))
        return f.save(TagLib::MPEG::File::ID3v2, strip ? TagLib::MPEG::File::StripOthers : TagLib::MPEG::File::StripNone, id3v2version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4);
#else
    if (save || (strip && f.hasID3v1Tag()))
        return f.save(TagLib::MPEG::File::ID3v2, strip, id3v2version);
#endif
    return true;
}

bool tag_clear_mp3(AudioFile *audio_file, bool strip, int id3v2version)
{
    TagLib::MPEG::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    bool save = !f.hasID3v2Tag();
    TagLib::ID3v2::Tag *tag = f.ID3v2Tag(true);

    save = (tag_remove_id3v2(tag) || save);

    // work around bug taglib/taglib#913: strip APE before ID3v1
    if (strip && f.hasAPETag())
    {
        f.strip(TagLib::MPEG::File::APE);
        save = true;
    }

#if TAGLIB_VERSION >= 11200
    if (save || (strip && f.hasID3v1Tag()))
        return f.save(TagLib::MPEG::File::ID3v2, strip ? TagLib::MPEG::File::StripOthers : TagLib::MPEG::File::StripNone, id3v2version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4);
#else
    if (save || (strip && f.hasID3v1Tag()))
        return f.save(TagLib::MPEG::File::ID3v2, strip, id3v2version);
#endif
    return true;
}


/*** WAV ****/
// Experimental WAV file tagging within an "ID3" chunk
bool tag_present_wav(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::RIFF::WAV::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ID3v2::Tag *tag = f.ID3v2Tag();
    return tags_present_id3v2(tag, do_album, mode);
}

bool tag_write_wav(AudioFile *audio_file, bool do_album, char mode, char *unit, bool lowercase, bool strip, int id3v2version)
{
    UNUSED(strip);
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    if (lowercase)
        RG_STRING = RG_STRING_LOWER;

    TagLib::RIFF::WAV::File f(audio_file->getTagLibFilePath());
    TagLib::ID3v2::Tag *tag = f.ID3v2Tag();

    // remove old tags before writing new ones
    //tag_remove_wav(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    bool save = tag_update_txxx(tag, RG_STRING[RG_TRACK_GAIN], value);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_id3v2(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_txxx(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_id3v2(tag) || save);

    // no stripping
#if TAGLIB_VERSION >= 11200
    if (save)
        return f.save(TagLib::RIFF::WAV::File::AllTags, TagLib::RIFF::WAV::File::StripNone, id3v2version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4);
#else
    if (save)
        return f.save(TagLib::RIFF::WAV::File::AllTags, false, id3v2version);
#endif
    return true;
}

bool tag_clear_wav(AudioFile *audio_file, bool strip, int id3v2version)
{
    UNUSED(strip);
    TagLib::RIFF::WAV::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ID3v2::Tag *tag = f.ID3v2Tag();

    if (tag_remove_id3v2(tag))
    {
        // no stripping
#if TAGLIB_VERSION >= 11200
        return f.save(TagLib::RIFF::WAV::File::AllTags, TagLib::RIFF::WAV::File::StripNone, id3v2version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4);
#else
        return f.save(TagLib::RIFF::WAV::File::AllTags, false, id3v2version);
#endif
    }
    return true;
}


/*** AIFF ****/
// id3v2version and strip currently unimplemented since no TagLib support
// Experimental AIFF file tagging within an "ID3 " chunk
bool tag_present_aiff(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::RIFF::AIFF::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ID3v2::Tag *tag = f.tag();
    return tags_present_id3v2(tag, do_album, mode);
}

bool tag_write_aiff(AudioFile *audio_file, bool do_album, char mode, char *unit, bool lowercase, bool strip, int id3v2version)
{
    UNUSED(strip);
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    if (lowercase)
        RG_STRING = RG_STRING_LOWER;

    TagLib::RIFF::AIFF::File f(audio_file->getTagLibFilePath());
    TagLib::ID3v2::Tag *tag = f.tag();

    // remove old tags before writing new ones
    //tag_remove_aiff(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    bool save = tag_update_txxx(tag, RG_STRING[RG_TRACK_GAIN], value);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_id3v2(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_txxx(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_txxx(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_txxx(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_id3v2(tag) || save);

    // no stripping
#if TAGLIB_VERSION >= 11200
    if (save)
        return f.save(id3v2version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4);
#else
    if (save)
        return f.save();
#endif
    return true;
}

bool tag_clear_aiff(AudioFile *audio_file, bool strip, int id3v2version)
{
    UNUSED(strip);
    TagLib::RIFF::AIFF::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ID3v2::Tag *tag = f.tag();

    if (tag_remove_id3v2(tag))
    {
        // no stripping
#if TAGLIB_VERSION >= 11200
        return f.save(id3v2version == 3 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4);
#else
        return f.save();
#endif
    }
    return true;
}


/*** FLAC ****/
bool tags_present_ogg(TagLib::Ogg::XiphComment *tag, bool do_album, char mode)
{
    bool extended = (mode == 'e');
    std::set<TagLib::String> rgtags;

    TagLib::Ogg::FieldListMap items = tag->fieldListMap();

    for(TagLib::Ogg::FieldListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rgtags.insert(desc);
        }
    }

    if (!do_album && !extended)
        return (rgtags.size() == 2);
    else if (!do_album && extended)
        return (rgtags.size() == 4);
    else if (do_album && !extended)
        return (rgtags.size() == 4);
    return (rgtags.size() == 7);
}

bool tag_update_flac(TagLib::Ogg::XiphComment *tag, const char* key, const char* value)
{
    bool found = false;
    bool updated = false;
    TagLib::StringList list;
    TagLib::Ogg::FieldListMap items = tag->fieldListMap();

    for(TagLib::Ogg::FieldListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first;
        int cmp = str_compare(desc, key);
        if (cmp == 0)
        {
            found = true;
            TagLib::StringList vals = item->second;

            if ((vals.size() != 1) || ((vals.size() == 1) && (vals.front() != value)))
            {
                updated = true;
                tag->addField(desc, TagLib::String(value), true);
            }
        }
        else if (cmp == 1)
            list.append(desc);
    }

    if (list.size() > 0)
    {
        updated = true;
        for (int i = 0; i < int(list.size()); ++i)
            tag->removeFields(list[i]);
        tag->addField(key, TagLib::String(value), true);
    }
    else if (!found)
    {
        updated = true;
        tag->addField(key, TagLib::String(value), true);
    }
    return updated;
}

bool tag_remove_album_flac(TagLib::Ogg::XiphComment *tag)
{
    if (tag->contains(RG_STRING_UPPER[RG_ALBUM_GAIN]) || tag->contains(RG_STRING_UPPER[RG_ALBUM_PEAK]) ||
            tag->contains(RG_STRING_UPPER[RG_ALBUM_RANGE]))
    {
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_GAIN]);
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_PEAK]);
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_RANGE]);
        return true;
    }
    return false;
}

bool tag_remove_extra_flac(TagLib::Ogg::XiphComment *tag)
{
    if (tag->contains(RG_STRING_UPPER[RG_TRACK_RANGE]) || tag->contains(RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
            tag->contains(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
    {
        tag->removeFields(RG_STRING_UPPER[RG_TRACK_RANGE]);
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_RANGE]);
        tag->removeFields(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]);
        return true;
    }
    return false;
}

bool tag_remove_flac(TagLib::Ogg::XiphComment *tag)
{
    if (tag->contains(RG_STRING_UPPER[RG_TRACK_GAIN]) || tag->contains(RG_STRING_UPPER[RG_TRACK_PEAK]) ||
            tag->contains(RG_STRING_UPPER[RG_TRACK_RANGE]) || tag->contains(RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
            tag->contains(RG_STRING_UPPER[RG_ALBUM_PEAK]) || tag->contains(RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
            tag->contains(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
    {
        tag->removeFields(RG_STRING_UPPER[RG_TRACK_GAIN]);
        tag->removeFields(RG_STRING_UPPER[RG_TRACK_PEAK]);
        tag->removeFields(RG_STRING_UPPER[RG_TRACK_RANGE]);
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_GAIN]);
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_PEAK]);
        tag->removeFields(RG_STRING_UPPER[RG_ALBUM_RANGE]);
        tag->removeFields(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]);
        return true;
    }
    return false;
}

bool tag_present_flac(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::FLAC::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.xiphComment(true);
    return tags_present_ogg(tag, do_album, mode);
}

bool tag_write_flac(AudioFile *audio_file, bool do_album, char mode, char *unit)
{
    char value[2048];

    TagLib::FLAC::File f(audio_file->getTagLibFilePath());
    bool save = !f.hasXiphComment();
    TagLib::Ogg::XiphComment *tag = f.xiphComment(true);

    // remove old tags before writing new ones
    //tag_remove_flac(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    save = (tag_update_flac(tag, RG_STRING_UPPER[RG_TRACK_GAIN], value) || save);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_flac(tag, RG_STRING_UPPER[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_flac(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_flac(tag, RG_STRING_UPPER[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_flac(tag) || save);

    if (save)
        return f.save();
    return true;
}

bool tag_clear_flac(AudioFile *audio_file)
{
    TagLib::FLAC::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    bool save = !f.hasXiphComment();
    TagLib::Ogg::XiphComment *tag = f.xiphComment(true);

    save = (tag_remove_flac(tag) || save);

    if (save)
        return f.save();
    return true;
}


/*** Ogg (Vorbis, FLAC, Speex, Opus) ****/
bool tag_make_ogg(AudioFile *audio_file, bool do_album, char mode, char *unit, TagLib::Ogg::XiphComment *tag)
{
    bool save = false;
    char value[2048];

    // remove old tags before writing new ones
    //tag_remove_flac(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    save = (tag_update_flac(tag, RG_STRING_UPPER[RG_TRACK_GAIN], value) || save);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_flac(tag, RG_STRING_UPPER[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_flac(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_flac(tag, RG_STRING_UPPER[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_flac(tag, RG_STRING_UPPER[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_flac(tag) || save);

    return save;
}

/*** Ogg: Ogg Vorbis ***/
bool tag_present_ogg_vorbis(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::Ogg::Vorbis::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();
    return tags_present_ogg(tag, do_album, mode);
}

bool tag_write_ogg_vorbis(AudioFile *audio_file, bool do_album, char mode, char *unit)
{
    TagLib::Ogg::Vorbis::File f(audio_file->getTagLibFilePath());
    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_make_ogg(audio_file, do_album, mode, unit, tag))
        return f.save();
    return true;
}

bool tag_clear_ogg_vorbis(AudioFile *audio_file)
{
    TagLib::Ogg::Vorbis::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_remove_flac(tag))
        return f.save();
    return true;
}

/*** Ogg: Ogg FLAC ***/
bool tag_present_ogg_flac(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::Ogg::FLAC::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();
    return tags_present_ogg(tag, do_album, mode);
}

bool tag_write_ogg_flac(AudioFile *audio_file, bool do_album, char mode, char *unit)
{
    TagLib::Ogg::FLAC::File f(audio_file->getTagLibFilePath());
    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_make_ogg(audio_file, do_album, mode, unit, tag))
        return f.save();
    return true;
}

bool tag_clear_ogg_flac(AudioFile *audio_file)
{
    TagLib::Ogg::FLAC::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_remove_flac(tag))
        return f.save();
    return true;
}

/*** Ogg: Ogg Speex ***/
bool tag_present_ogg_speex(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::Ogg::Speex::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();
    return tags_present_ogg(tag, do_album, mode);
}

bool tag_write_ogg_speex(AudioFile *audio_file, bool do_album, char mode, char *unit)
{
    TagLib::Ogg::Speex::File f(audio_file->getTagLibFilePath());
    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_make_ogg(audio_file, do_album, mode, unit, tag))
        return f.save();
    return true;
}

bool tag_clear_ogg_speex(AudioFile *audio_file)
{
    TagLib::Ogg::Speex::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_remove_flac(tag))
        return f.save();
    return true;
}

/*** Ogg: Opus ****/

// Opus Notes:
//
// 1. Opus ONLY uses R128_TRACK_GAIN and (optionally) R128_ALBUM_GAIN
//    as an ADDITIONAL offset to the header's 'output_gain'.
// 2. Encoders and muxes set 'output_gain' to zero, so a non-zero 'output_gain' in
//    the header i supposed to be a change AFTER encoding/muxing.
// 3. We assume that FFmpeg's avformat does already apply 'output_gain' (???)
//    so we get get pre-gained data and only have to calculate the difference.
// 4. Opus adheres to EBU-R128, so the loudness reference is ALWAYS -23 LUFS.
//    This means we have to adapt for possible `-d n` (`--pregain=n`) changes.
//    This also means players have to add an extra +5 dB to reach the loudness
//    ReplayGain 2.0 prescribes (-18 LUFS).
// 5. Opus R128_* tags use ASCII-encoded Q7.8 numbers with max. 6 places including
//    the minus sign, and no unit.
//    See https://en.wikipedia.org/wiki/Q_(number_format)
// 6. RFC 7845 states: "To avoid confusion with multiple normalization schemes, an
//    Opus comment header SHOULD NOT contain any of the REPLAYGAIN_TRACK_GAIN,
//    REPLAYGAIN_TRACK_PEAK, REPLAYGAIN_ALBUM_GAIN, or REPLAYGAIN_ALBUM_PEAK tags, […]"
//    So we remove REPLAYGAIN_* tags if any are present.
// 7. RFC 7845 states: "Peak normalizations are difficult to calculate reliably
//    for lossy codecs because of variation in excursion heights due to decoder
//    differences. In the authors' investigations, they were not applied
//    consistently or broadly enough to merit inclusion here."
//    So there are NO "Peak" type tags. The (oversampled) true peak levels that
//    libebur128 calculates for us are STILL used for clipping prevention if so
//    requested. They are also shown in the output, just not stored into tags.

int gain_to_q78num(double gain)
{
    // convert float to Q7.8 number: Q = round(f * 2^8)
    return (int) round(gain * 256.0);    // 2^8 = 256
}

bool tag_remove_ogg_opus(TagLib::Ogg::XiphComment *tag)
{
    // RFC 7845 states:
    // To avoid confusion with multiple normalization schemes, an Opus
    // comment header SHOULD NOT contain any of the REPLAYGAIN_TRACK_GAIN,
    // REPLAYGAIN_TRACK_PEAK, REPLAYGAIN_ALBUM_GAIN, or
    // REPLAYGAIN_ALBUM_PEAK tags, […]"
    // so we remove these if present
    bool save = tag_remove_flac(tag);

    if (tag->contains("R128_TRACK_GAIN") || tag->contains("R128_ALBUM_GAIN"))
    {
        save =true;
        tag->removeFields("R128_TRACK_GAIN");
        tag->removeFields("R128_ALBUM_GAIN");
    }
    return save;
}

bool tag_present_ogg_opus(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::Ogg::Opus::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();
    return tags_present_ogg(tag, do_album, mode);
}

bool tag_write_ogg_opus(AudioFile *audio_file, bool do_album, char mode, char *unit)
{
    UNUSED(mode); UNUSED(unit);
    char value[2048];

    TagLib::Ogg::Opus::File f(audio_file->getTagLibFilePath());
    TagLib::Ogg::XiphComment *tag = f.tag();

    // remove old tags before writing new ones
    //tag_remove_ogg_opus(tag);

    snprintf(value, sizeof(value), "%d", gain_to_q78num(audio_file->trackGain));
    bool save = tag_update_flac(tag, "R128_TRACK_GAIN", value);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%d", gain_to_q78num(audio_file->albumGain));
        save = (tag_update_flac(tag, "R128_ALBUM_GAIN", value) || save);
    }
    else
    {
        save = (tag_remove_album_flac(tag) || save);
        if (tag->contains("R128_ALBUM_GAIN"))
        {
            tag->removeFields("R128_ALBUM_GAIN");
            save = true;
        }
    }

    // extra tags mode -s e or -s l
    // no extra tags allowed in Opus
    save = (tag_remove_extra_flac(tag) || save);

    if (save)
        return f.save();
    return true;
}

bool tag_clear_ogg_opus(AudioFile *audio_file) {
    TagLib::Ogg::Opus::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::Ogg::XiphComment *tag = f.tag();

    if (tag_remove_ogg_opus(tag))
        return f.save();
    return true;
}


/*** MP4 ****/
// build tagging key from RG_ATOM and REPLAYGAIN_* string
TagLib::String tagname(TagLib::String key)
{
    TagLib::String res = RG_ATOM;
    return res.append(key);
}

bool tags_present_mp4(TagLib::MP4::Tag *tag, bool do_album, char mode)
{
    bool extended = (mode == 'e');
    std::set<TagLib::String> rgtags;

#if TAGLIB_VERSION >= 11200
    TagLib::MP4::ItemMap items = tag->itemMap();
    for(TagLib::MP4::ItemMap::Iterator item = items.begin(); item != items.end(); ++item)
#else
    TagLib::MP4::ItemListMap &items = tag->itemListMap();
    for(TagLib::MP4::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
#endif
    {
        TagLib::String desc = item->first.upper();
        if ((desc == tagname(RG_STRING_UPPER[RG_TRACK_GAIN]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_TRACK_PEAK]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_TRACK_RANGE]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_ALBUM_GAIN]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_ALBUM_PEAK]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_ALBUM_RANGE]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]).upper()))
        {
            rgtags.insert(desc);
        }
    }

    if (!do_album && !extended)
        return (rgtags.size() == 2);
    else if (!do_album && extended)
        return (rgtags.size() == 4);
    else if (do_album && !extended)
        return (rgtags.size() == 4);
    return (rgtags.size() == 7);
}


bool tag_update_mp4(TagLib::MP4::Tag *tag, const char* key, const char* value)
{
    bool found = false;
    bool updated = false;
    TagLib::StringList list;

#if TAGLIB_VERSION >= 11200
    TagLib::MP4::ItemMap items = tag->itemMap();
    for(TagLib::MP4::ItemMap::Iterator item = items.begin(); item != items.end(); ++item)
#else
    TagLib::MP4::ItemListMap &items = tag->itemListMap();
    for(TagLib::MP4::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
#endif
    {
        TagLib::String desc = item->first;
        int cmp = str_compare(desc, tagname(key));
        if (cmp == 0)
        {
            found = true;
            if (!tag->item(key).toStringList().contains(value))
            {
                updated = true;
                tag->setItem(key, TagLib::StringList(value));
            }
        }
        else if (cmp == 1)
            list.append(desc);
    }

    if (list.size() > 0)
    {
        updated = true;
        for (int i = 0; i < int(list.size()); ++i)
            tag->removeItem(list[i]);
        tag->setItem(tagname(key), TagLib::StringList(value));
    }
    else if (!found)
    {
        updated = true;
        tag->setItem(tagname(key), TagLib::StringList(value));
    }
    return updated;
}

bool tag_remove_album_mp4(TagLib::MP4::Tag *tag)
{
    bool rm = false;

#if TAGLIB_VERSION >= 11200
    TagLib::MP4::ItemMap items = tag->itemMap();
    for(TagLib::MP4::ItemMap::Iterator item = items.begin(); item != items.end(); ++item)
#else
    TagLib::MP4::ItemListMap &items = tag->itemListMap();
    for(TagLib::MP4::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
#endif
    {
        TagLib::String desc = item->first.upper();
        if ((desc == tagname(RG_STRING_UPPER[RG_ALBUM_GAIN]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_ALBUM_PEAK]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_ALBUM_RANGE]).upper()))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_remove_extra_mp4(TagLib::MP4::Tag *tag)
{
    bool rm = false;

#if TAGLIB_VERSION >= 11200
    TagLib::MP4::ItemMap items = tag->itemMap();
    for(TagLib::MP4::ItemMap::Iterator item = items.begin(); item != items.end(); ++item)
#else
    TagLib::MP4::ItemListMap &items = tag->itemListMap();
    for(TagLib::MP4::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
#endif
    {
        TagLib::String desc = item->first.upper();
        if ((desc == tagname(RG_STRING_UPPER[RG_TRACK_RANGE]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_ALBUM_RANGE]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]).upper()))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_remove_mp4(TagLib::MP4::Tag *tag)
{
    bool rm = false;

#if TAGLIB_VERSION >= 11200
    TagLib::MP4::ItemMap items = tag->itemMap();
    for(TagLib::MP4::ItemMap::Iterator item = items.begin(); item != items.end(); ++item)
#else
    TagLib::MP4::ItemListMap &items = tag->itemListMap();
    for(TagLib::MP4::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
#endif
    {
        TagLib::String desc = item->first.upper();
        if ((desc == tagname(RG_STRING_UPPER[RG_TRACK_GAIN]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_TRACK_PEAK]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_TRACK_RANGE]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_ALBUM_GAIN]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_ALBUM_PEAK]).upper()) || (desc == tagname(RG_STRING_UPPER[RG_ALBUM_RANGE]).upper()) ||
                (desc == tagname(RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]).upper()))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_present_mp4(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::MP4::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::MP4::Tag *tag = f.tag();
    return tags_present_mp4(tag, do_album, mode);
}

bool tag_write_mp4(AudioFile *audio_file, bool do_album, char mode, char *unit, bool lowercase)
{
    bool save = false;
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    if (lowercase)
        RG_STRING = RG_STRING_LOWER;

    TagLib::MP4::File f(audio_file->getTagLibFilePath());
    TagLib::MP4::Tag *tag = f.tag();

    // remove old tags before writing new ones
    //tag_remove_mp4(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    save = (tag_update_mp4(tag, RG_STRING[RG_TRACK_GAIN], value) || save);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_mp4(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_mp4(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_mp4(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_mp4(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_mp4(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_mp4(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_mp4(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_mp4(tag) || save);

    if (save)
        return f.save();
    return true;
}

bool tag_clear_mp4(AudioFile *audio_file) {
    TagLib::MP4::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::MP4::Tag *tag = f.tag();

    if (tag_remove_mp4(tag))
        return f.save();
    return true;
}


/*** ASF/WMA ****/
bool tags_present_asf(TagLib::ASF::Tag *tag, bool do_album, char mode)
{
    bool extended = (mode == 'e');
    std::set<TagLib::String> rgtags;

    TagLib::ASF::AttributeListMap &items = tag->attributeListMap();

    for(TagLib::ASF::AttributeListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rgtags.insert(desc);
        }
    }

    if (!do_album && !extended)
        return (rgtags.size() == 2);
    else if (!do_album && extended)
        return (rgtags.size() == 4);
    else if (do_album && !extended)
        return (rgtags.size() == 4);
    return (rgtags.size() == 7);
}

bool tag_update_asf(TagLib::ASF::Tag *tag, const char* key, const char* value)
{
    bool found = false;
    bool updated = false;
    TagLib::StringList list;
    TagLib::ASF::AttributeListMap &items = tag->attributeListMap();

    for(TagLib::ASF::AttributeListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first;
        int cmp = str_compare(desc, key);
        if (cmp == 0)
        {
            found = true;
            TagLib::ASF::AttributeList attr = item->second;

            if (!((attr.size() == 1) && (attr.front().toString() == value)))
            {
                updated = true;
                tag->setAttribute(desc, TagLib::String(value));
            }
        }
        else if (cmp == 1)
            list.append(desc);
    }

    if (list.size() > 0)
    {
        updated = true;
        for (int i = 0; i < int(list.size()); ++i)
            tag->removeItem(list[i]);
        tag->setAttribute(key, TagLib::String(value));
    }
    else if (!found)
    {
        updated = true;
        tag->setAttribute(key, TagLib::String(value));
    }
    return updated;
}

bool tag_remove_album_asf(TagLib::ASF::Tag *tag)
{
    bool rm = false;
    TagLib::ASF::AttributeListMap &items = tag->attributeListMap();

    for(TagLib::ASF::AttributeListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) || (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_remove_extra_asf(TagLib::ASF::Tag *tag)
{
    bool rm = false;
    TagLib::ASF::AttributeListMap &items = tag->attributeListMap();

    for(TagLib::ASF::AttributeListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_remove_asf(TagLib::ASF::Tag *tag)
{
    bool rm = false;
    TagLib::ASF::AttributeListMap &items = tag->attributeListMap();

    for(TagLib::ASF::AttributeListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_present_asf(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::ASF::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ASF::Tag *tag = f.tag();
    return tags_present_asf(tag, do_album, mode);
}

bool tag_write_asf(AudioFile *audio_file, bool do_album, char mode, char *unit, bool lowercase)
{
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    if (lowercase)
        RG_STRING = RG_STRING_LOWER;

    TagLib::ASF::File f(audio_file->getTagLibFilePath());
    TagLib::ASF::Tag *tag = f.tag();

    // remove old tags before writing new ones
    //tag_remove_asf(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    bool save = tag_update_asf(tag, RG_STRING[RG_TRACK_GAIN], value);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_asf(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_asf(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_asf(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_asf(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_asf(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_asf(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_asf(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_asf(tag) || save);

    if (save)
        return f.save();
    return true;
}

bool tag_clear_asf(AudioFile *audio_file)
{
    TagLib::ASF::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::ASF::Tag *tag = f.tag();

    if (tag_remove_asf(tag))
        return f.save();
    return true;
}


/*** APE (Monkey’s Audio) ***/
// We COULD also use ID3 tags, but we stick with APEv2 tags,
// since that is the native format.
// APEv2 tags can be mixed case, but they should be read case-insensitively,
// so we currently ignore -L (--lowercase) and only write uppercase tags.
// TagLib handles APE case-insensitively and uses only UPPERCASE keys.
// Existing ID3 tags can be removed by using -S (--strip).
bool tags_present_ape(TagLib::APE::Tag *tag, bool do_album, char mode)
{
    bool extended = (mode == 'e');
    std::set<TagLib::String> rgtags;

    TagLib::APE::ItemListMap items = tag->itemListMap();
    for(TagLib::APE::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rgtags.insert(desc);
        }
    }

    if (!do_album && !extended)
        return (rgtags.size() == 2);
    else if (!do_album && extended)
        return (rgtags.size() == 4);
    else if (do_album && !extended)
        return (rgtags.size() == 4);
    return (rgtags.size() == 7);
}

bool tag_update_ape(TagLib::APE::Tag *tag, const char* key, const char* value)
{
    bool updated = false;
    bool found = false;
    TagLib::StringList list;

    TagLib::APE::ItemListMap items = tag->itemListMap();
    for(TagLib::APE::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first;
        int cmp = str_compare(desc,key);
        if (cmp == 0)
        {
            found = true;
            if ((item->second.values().size() != 1) || (item->second.values().front() != value))
            {
                updated = true;
                tag->addValue(desc, TagLib::String(value), true);
            }
        }
        else if (cmp == 1)
            list.append(desc);
    }

    if (list.size() > 0)
    {
        updated = true;
        for (int i = 0; i < int(list.size()); ++i)
            tag->removeItem(list[i]);
        tag->addValue(key, TagLib::String(value), true);
    }
    else if (!found)
    {
        updated = true;
        tag->addValue(key, TagLib::String(value), true);
    }
    return updated;
}

bool tag_remove_album_ape(TagLib::APE::Tag *tag)
{
    bool rm = false;

    TagLib::APE::ItemListMap items = tag->itemListMap();
    for(TagLib::APE::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) || (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_remove_extra_ape(TagLib::APE::Tag *tag)
{
    bool rm = false;

    TagLib::APE::ItemListMap items = tag->itemListMap();
    for(TagLib::APE::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_remove_ape(TagLib::APE::Tag *tag)
{
    bool rm = false;

    TagLib::APE::ItemListMap items = tag->itemListMap();
    for(TagLib::APE::ItemListMap::Iterator item = items.begin(); item != items.end(); ++item)
    {
        TagLib::String desc = item->first.upper();
        if ((desc == RG_STRING_UPPER[RG_TRACK_GAIN]) || (desc == RG_STRING_UPPER[RG_TRACK_PEAK]) ||
                (desc == RG_STRING_UPPER[RG_TRACK_RANGE]) || (desc == RG_STRING_UPPER[RG_ALBUM_GAIN]) ||
                (desc == RG_STRING_UPPER[RG_ALBUM_PEAK]) || (desc == RG_STRING_UPPER[RG_ALBUM_RANGE]) ||
                (desc == RG_STRING_UPPER[RG_REFERENCE_LOUDNESS]))
        {
            rm = true;
            tag->removeItem(item->first);
        }
    }
    return rm;
}

bool tag_present_ape(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::APE::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::APE::Tag *tag = f.APETag(true);
    return tags_present_ape(tag, do_album, mode);
}

bool tag_write_ape(AudioFile *audio_file, bool do_album, char mode, char *unit, bool lowercase, bool strip)
{
    UNUSED(lowercase);
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    // ignore lowercase for now: CAN be written but keys should be read case-insensitively
    // if (lowercase)
    //   RG_STRING = RG_STRING_LOWER;

    TagLib::APE::File f(audio_file->getTagLibFilePath());
    bool save = !f.hasAPETag();
    TagLib::APE::Tag *tag = f.APETag(true); // create if none exists

    // remove old tags before writing new ones
    //tag_remove_ape(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    save = (tag_update_ape(tag, RG_STRING[RG_TRACK_GAIN], value) || save);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_ape(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_ape(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_ape(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_ape(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_ape(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_ape(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_ape(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_ape(tag) || save);

    if (strip && f.hasID3v1Tag())
    {
        save = true;
        f.strip(TagLib::APE::File::TagTypes::ID3v1);
    }

    if (save)
        return f.save();
    return true;
}

bool tag_clear_ape(AudioFile *audio_file, bool strip)
{
    TagLib::WavPack::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    bool save = !f.hasAPETag();
    TagLib::APE::Tag *tag = f.APETag(true); // create if none exists

    save = (tag_remove_ape(tag) || save);

    if (strip && f.hasID3v1Tag())
    {
        save = true;
        f.strip(TagLib::WavPack::File::TagTypes::ID3v1);
    }

    if (save)
        return f.save();
    return true;
}


/*** WavPack ***/
// We COULD also use ID3 tags, but we stick with APEv2 tags,
// since that is the native format.
// APEv2 tags can be mixed case, but they should be read case-insensitively,
// so we currently ignore -L (--lowercase) and only write uppercase tags.
// TagLib handles APE case-insensitively and uses only UPPERCASE keys.
// Existing ID3v2 tags can be removed by using -S (--strip).
bool tag_present_wavpack(AudioFile *audio_file, bool do_album, char mode)
{
    TagLib::WavPack::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    TagLib::APE::Tag *tag = f.APETag(true);
    return tags_present_ape(tag, do_album, mode);
}

bool tag_write_wavpack(AudioFile *audio_file, bool do_album, char mode, char *unit, bool lowercase, bool strip)
{
    UNUSED(lowercase);
    char value[2048];
    const char **RG_STRING = RG_STRING_UPPER;

    // ignore lowercase for now: CAN be written but keys should be read case-insensitively
    // if (lowercase)
    //   RG_STRING = RG_STRING_LOWER;

    TagLib::WavPack::File f(audio_file->getTagLibFilePath());
    bool save = !f.hasAPETag();
    TagLib::APE::Tag *tag = f.APETag(true); // create if none exists

    // remove old tags before writing new ones
    //tag_remove_wavpack(tag);

    snprintf(value, sizeof(value), "%.2f %s", audio_file->trackGain, unit);
    save = (tag_update_ape(tag, RG_STRING[RG_TRACK_GAIN], value) || save);

    snprintf(value, sizeof(value), "%.6f", audio_file->trackPeak);
    save = (tag_update_ape(tag, RG_STRING[RG_TRACK_PEAK], value) || save);

    // Only write album tags if in album mode (would be zero otherwise)
    if (do_album)
    {
        snprintf(value, sizeof(value), "%.2f %s", audio_file->albumGain, unit);
        save = (tag_update_ape(tag, RG_STRING[RG_ALBUM_GAIN], value) || save);

        snprintf(value, sizeof(value), "%.6f", audio_file->albumPeak);
        save = (tag_update_ape(tag, RG_STRING[RG_ALBUM_PEAK], value) || save);
    }
    else
        save = (tag_remove_album_ape(tag) || save);

    // extra tags mode -s e or -s l
    if (mode == 'e' || mode == 'l')
    {
        snprintf(value, sizeof(value), "%.2f LUFS", audio_file->loudnessReference);
        save = (tag_update_ape(tag, RG_STRING[RG_REFERENCE_LOUDNESS], value) || save);

        snprintf(value, sizeof(value), "%.2f %s", audio_file->trackLoudnessRange, unit);
        save = (tag_update_ape(tag, RG_STRING[RG_TRACK_RANGE], value) || save);

        if (do_album)
        {
            snprintf(value, sizeof(value), "%.2f %s", audio_file->albumLoudnessRange, unit);
            save = (tag_update_ape(tag, RG_STRING[RG_ALBUM_RANGE], value) || save);
        }
    }
    else
        save = (tag_remove_extra_ape(tag) || save);

    if (strip && f.hasID3v1Tag())
    {
        save = true;
        f.strip(TagLib::WavPack::File::TagTypes::ID3v1);
    }

    if (save)
        return f.save();
    return true;
}

bool tag_clear_wavpack(AudioFile *audio_file, bool strip)
{
    TagLib::WavPack::File f(audio_file->getTagLibFilePath());

    if (!f.isValid())
    {
        printFileError(audio_file->filePath());
        return true;
    }

    bool save = !f.hasAPETag();
    TagLib::APE::Tag *tag = f.APETag(true); // create if none exists

    save = (tag_remove_ape(tag) || save);

    if (strip && f.hasID3v1Tag())
    {
        save = true;
        f.strip(TagLib::WavPack::File::TagTypes::ID3v1);
    }

    if (save)
        return f.save();
    return true;
}



RGTagManager::RGTagManager() { };

RGTagManager::~RGTagManager() { };

int RGTagManager::avContainerNameToId(const std::string &str)
{
    if (str.length() == 0)
        return -1;

    for (int i = 0; i < int(av_container_names.size()); i++)
        if (av_container_names[i].find(str) != std::string::npos)
            return i;

    return -1;
}

bool RGTagManager::hasRGTags(const fs::path &path, bool do_album, char tagmode, bool &ok)
{
    AudioFile audio_file(path);
    return hasRGTags(&audio_file, do_album, tagmode, ok);
}

bool RGTagManager::hasRGTags(AudioFile *audio_file, bool do_album, char tagmode, bool &ok)
{
    bool rc = false;
    ok = audio_file->initFile();

    switch (avContainerNameToId(audio_file->avFormat))
    {
    case AV_CONTAINER_ID_MP3:
        rc = tag_present_mp3(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_FLAC:
        rc = tag_present_flac(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_OGG:
        // must separate because TagLib uses different file classes
        switch (audio_file->avCodecId)
        {
        // Opus needs special handling (different RG tags, -23 LUFS ref.)
        case AV_CODEC_ID_OPUS:
            rc = tag_present_ogg_opus(audio_file, do_album, tagmode);
            break;

        case AV_CODEC_ID_VORBIS:
            rc = tag_present_ogg_vorbis(audio_file, do_album, tagmode);
            break;

        case AV_CODEC_ID_FLAC:
            rc = tag_present_ogg_flac(audio_file, do_album, tagmode);
            break;

        case AV_CODEC_ID_SPEEX:
            rc = tag_present_ogg_speex(audio_file, do_album, tagmode);
            break;

        default:
            ok = false;
            break;
        }
        break;

    case AV_CONTAINER_ID_MP4:
        rc = tag_present_mp4(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_ASF:
        rc = tag_present_asf(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_WAV:
        rc = tag_present_wav(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_AIFF:
        rc = tag_present_aiff(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_WV:
        rc = tag_present_wavpack(audio_file, do_album, tagmode);
        break;

    case AV_CONTAINER_ID_APE:
        rc = tag_present_ape(audio_file, do_album, tagmode);
        break;

    default:
        ok = false;
        break;
    }
    return rc;
};

bool RGTagManager::writeRGTags(AudioFile *audio_file, bool do_album, char tagmode, char *unit, bool lowercase, bool strip, int id3v2version)
{
    bool rc = false;

    switch (avContainerNameToId(audio_file->avFormat))
    {
    case -1:
        printFormatError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_MP3:
        if (!(rc = tag_write_mp3(audio_file, do_album, tagmode, unit, lowercase, strip, id3v2version)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_FLAC:
        if (!(rc = tag_write_flac(audio_file, do_album, tagmode, unit)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_OGG:
        // must separate because TagLib uses different file classes
        switch (audio_file->avCodecId)
        {
        // Opus needs special handling (different RG tags, -23 LUFS ref.)
        case AV_CODEC_ID_OPUS:
            if (!(rc = tag_write_ogg_opus(audio_file, do_album, tagmode, unit)))
                printWriteError(audio_file->filePath());
            break;

        case AV_CODEC_ID_VORBIS:
            if (!(rc = tag_write_ogg_vorbis(audio_file, do_album, tagmode, unit)))
                printWriteError(audio_file->filePath());
            break;

        case AV_CODEC_ID_FLAC:
            if (!(rc = tag_write_ogg_flac(audio_file, do_album, tagmode, unit)))
                printWriteError(audio_file->filePath());
            break;

        case AV_CODEC_ID_SPEEX:
            if (!(rc = tag_write_ogg_speex(audio_file, do_album, tagmode, unit)))
                printWriteError(audio_file->filePath());
            break;

        default:
            printCodecError(audio_file);
            break;
        }
        break;

    case AV_CONTAINER_ID_MP4:
        if (!(rc = tag_write_mp4(audio_file, do_album, tagmode, unit, lowercase)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_ASF:
        if (!(rc = tag_write_asf(audio_file, do_album, tagmode, unit, lowercase)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_WAV:
        if (!(rc = tag_write_wav(audio_file, do_album, tagmode, unit, lowercase, strip, id3v2version)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_AIFF:
        if (!(rc = tag_write_aiff(audio_file, do_album, tagmode, unit, lowercase, strip, id3v2version)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_WV:
        if (!(rc = tag_write_wavpack(audio_file, do_album, tagmode, unit, lowercase, strip)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_APE:
        if (!(rc = tag_write_ape(audio_file, do_album, tagmode, unit, lowercase, strip)))
            printWriteError(audio_file->filePath());
        break;

    default:
        printTypeError(audio_file);
        break;
    }
    return rc;
};

bool RGTagManager::clearRGTags(AudioFile *audio_file, bool strip, int id3v2version)
{
    bool rc = false;
    switch (avContainerNameToId(audio_file->avFormat))
    {
    case -1:
        printFormatError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_MP3:
        if (!(rc = tag_clear_mp3(audio_file, strip, id3v2version)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_FLAC:
        if (!(rc = tag_clear_flac(audio_file)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_OGG:
        // must separate because TagLib uses fifferent File classes
        switch (audio_file->avCodecId)
        {
        // Opus needs special handling (different RG tags, -23 LUFS ref.)
        case AV_CODEC_ID_OPUS:
            if (!(rc = tag_clear_ogg_opus(audio_file)))
                printWriteError(audio_file->filePath());
            break;

        case AV_CODEC_ID_VORBIS:
            if (!(rc = tag_clear_ogg_vorbis(audio_file)))
                printWriteError(audio_file->filePath());
            break;

        case AV_CODEC_ID_FLAC:
            if (!(rc = tag_clear_ogg_flac(audio_file)))
                printWriteError(audio_file->filePath());
            break;

        case AV_CODEC_ID_SPEEX:
            if (!(rc = tag_clear_ogg_speex(audio_file)))
                printWriteError(audio_file->filePath());
            break;

        default:
            printCodecError(audio_file);
            break;
        }
        break;

    case AV_CONTAINER_ID_MP4:
        if (!(rc = tag_clear_mp4(audio_file)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_ASF:
        if (!(rc = tag_clear_asf(audio_file)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_WAV:
        if (!(rc = tag_clear_wav(audio_file, strip, id3v2version)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_AIFF:
        if (!(rc = tag_clear_aiff(audio_file, strip, id3v2version)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_WV:
        if (!(rc = tag_clear_wavpack(audio_file, strip)))
            printWriteError(audio_file->filePath());
        break;

    case AV_CONTAINER_ID_APE:
        if (!(rc = tag_clear_ape(audio_file, strip)))
            printWriteError(audio_file->filePath());
        break;

    default:
        printTypeError(audio_file);
        break;
    }

    return rc;
};
