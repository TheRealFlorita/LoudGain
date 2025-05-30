LoudGain
========

**LoudGain** is a loudness normalizer that scans music files and calculates loudness-normalized gain and loudness peak values according to the EBU R128 standard, and can optionally write ReplayGain-compatible metadata.

[EBU R128] [EBU] is a set of recommendations regarding loudness normalisation based on the algorithms to measure audio loudness and true-peak audio level defined in the [ITU BS.1770] [ITU] standard, and is used in the (currently under construction) ReplayGain 2.0 specification.

LoudGain implements a subset of mp3gain's command-line options, which means that it can be used as a drop-in replacement in some situations.

[EBU]: https://tech.ebu.ch/loudness
[ITU]: http://www.itu.int/rec/R-REC-BS.1770/en

## CHANGELOG

**2025-03-08** forked from https://github.com/ghedo/loudgain.git.
  * Includes all code changes from https://github.com/Moonbase59/loudgain.git up to version v0.6.8 (march 2025). I did not fork directly from this repository because of its size (due to including static builds in commits).
  * Applied a major refactor/rewrite of the above code.
  * Added Windows compatibility.
  * Added multi-threading support.
  * If the correct ReplayGain tags are already present, the program doesn't overwrite the file after scanning.
  * Subfolders can now be included in the scan.

## GETTING STARTED

Here are a few examples to get started:

```bash
$ LoudGain -a -p -s e *.flac                          # scan & tag albums of FLAC files
$ LoudGain -a -P 2 --skip-tagged-files -r "D:\Music"  # scan & tag albums in all subfolders of the music library, allow a clipping up to 2 dBTP, skip files which already have RG tags
$ LoudGain -a -p -E ".flac, .mp3" -r "D:\Music"       # scan & tag albums in all subfolders of the music library, skip any files that are not FLAC or MP3
```

See the [man page](docs/LoudGain.md) for more information.  

## DEPENDENCIES

 * `libavcodec`
 * `libavformat`
 * `libavutil`
 * `libebur128`
 * `libtag`

On Ubuntu you can install the needed libraries using:

```bash
$ sudo apt install build-essential cmake pkg-config git libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libebur128-dev libtag1-dev
```

LoudGain also makes use of the following libraries (header files already included in this project):  

 * `argparse` (https://github.com/p-ranav/argparse.git)
 * `mvThreadPool` (https://github.com/klimentyev/mvThreadPool.git)

## BUILDING

Build and install LoudGain on Ubuntu with:

```bash
$ git clone https://github.com/TheRealFlorita/LoudGain.git
$ cd LoudGain
$ mkdir build && cd build
$ cmake ..
$ make
$ sudo make install
```

For Windows 64-bit release files are provided.

## AUTHORS

Alessandro Ghedini <alessandro@ghedini.me>  
Matthias C. Hormann <mhormann@gmx.de>  
Floor <the.real.florita@gmail.com>

## COPYRIGHT

Copyright (C) 2014 Alessandro Ghedini <alessandro@ghedini.me>  
Everything after v0.1: Copyright (C) 2019 Matthias C. Hormann <mhormann@gmx.de>  
Everything from v1.0.0: Copyright (C) 2025 Floor <the.real.florita@gmail.com>

See COPYING for the license.
