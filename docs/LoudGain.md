LoudGain -- loudness normalizer based on the EBU R128 standard
=================================================================

## SYNOPSIS

`LoudGain [OPTIONS] FILES...`

## DESCRIPTION

**LoudGain** is a loudness normalizer that scans music files and calculates loudness-normalized gain and loudness peak values according to the EBU R128 standard, and can optionally write ReplayGain-compatible metadata.

LoudGain implements a subset of mp3gain's command-line options, which means that it can be used as a drop-in replacement in some situations.

LoudGain will _not_ modify the actual audio data, but instead just write ReplayGain _tags_ if so requested. It is up to the player to interpret these. (In some players, you need to enable this feature.)

LoudGain currently supports writing tags to the following file types:  
FLAC (.flac), Ogg (.ogg, .oga, .spx, .opus), MP2 (.mp2), MP3 (.mp3), MP4 (.mp4, .m4a), ASF/WMA (.asf, .wma), WavPack (.wv), APE (.ape).

Experimental, use with care: WAV (.wav), AIFF (.aiff, .aif, .snd).

## OPTIONS

* `-h, --help`:
  Show help information and exit.

* `-v, --version`:
  Show version number and exit.

* `-t, --track`:
  Calculate track gain only (default). 

* `-a, --album`:
  Calculate album gain (and track gain).

* `-i, --ignore-clipping`:
  Ignore clipping warning.

* `-p, --prevent-clipping`:
  Lower track/album gain to avoid clipping (<= -1 dBTP).

* `-P n, --max-true-peak-level=n`:
  Avoid clipping. Max true peak level = n dBTP.

* `-G n, --pre-gain=n`:
  Apply n dB/LU pre-gain value (-5 for -23 LUFS target). [default: 0]

* `-S d, --tagmode=d`:
  -S d: Delete ReplayGain tags from files
        -S i: Write ReplayGain 2.0 tags to files  
        -S e: Like '-S i', plus extra tags (reference, ranges)  
        -S s: Don't write ReplayGain tags

* `--skip-tagged-files`:
  Skip files with ReplayGain tags.

* `-l, --lowercase`:
  Force lowercase tags (MP2/MP3/MP4/WMA/WAV/AIFF).  
  This is non-standard, but sometimes needed.

* `-s, --striptags`:
  Strip tag types other than ID3v2 from MP2/MP3.
  Strip tag types other than APEv2 from WavPack/APE.

* `-I v, --id3v2version=v`:
  Write ID3v2.3 (v=3) or ID3v2.4 (v=4) tags to MP2/MP3/WAV/AIFF. [default: 4]

* `-M n, --multithread=n`:
  Set max number of threads (n). 0 = auto. Default is 0. [default: 0]

* `-o, --output-tab`:
  Database-friendly tab-delimited list output (mp3gain-compatible).

* `-O, --output-csv`:
  Database-friendly new format tab-delimited list output. Ideal for analysis
  of files if redirected to a CSV file.

* `-r, --recursive`:
  Recursive directory and file scan. Will look for supported audio files in all subfolders.

* `-E, --extensions`:
  Limit scan to specified extensions.

* `V, --verbosity`:
  Set vebosity level.  
    0: Only error messages are printed.  
    1: Minimal.  
    2: Print audio gain and peak values.  
    3: Print audio gain and peak values, as well as container and stream info.

* `-q, --quiet`:
  Low verbosity level. Equal to "-V 1".

## RECOMMENDATIONS

Use LoudGain on a »one album per folder« basis; Standard RG2 settings but
lowercase ReplayGain tags; clipping prevention on; strip obsolete tag types
from MP3 and WavPack files; use ID3v2.3 for MP3s; store extended tags:

    $ LoudGain -a -p -s e *.flac  
    $ LoudGain -a -P 2 --skip-tagged-files -r "D:\Music"  
    $ LoudGain -a -p -E ".flac, .mp3" -r "D:\Music"

## BUGS

**LoudGain** is maintained on GitHub. Please report all bugs to the issue tracker at https://github.com/TheRealFlorita/LoudGain/issues.

## COPYRIGHT

Copyright (C) 2025 Floor <the.real.florita@gmail.com> (versions > 1.0)
Copyright (C) 2019 Matthias C. Hormann <mhormann@gmx.de> (versions > 0.1)  
Copyright (C) 2014 Alessandro Ghedini <alessandro@ghedini.me> (v0.1)

This program is released under the 2 clause BSD license.
