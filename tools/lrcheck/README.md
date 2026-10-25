# Which channel is the left one?

A stereo decoder with a sign error still sounds like stereo - only mirrored - and nobody
notices. This folder is the test that settles it for the FM (analog) decoder in
`app/src/main/cpp/fmdemod.hpp`, without trusting anybody's ears.

    cd tools/lrcheck
    sh run.sh                    # the test signal only
    sh run.sh some_station.cu8   # ... and a real recording (nrsc5 -w file.cu8 95.5 0)

Needs python3 with numpy and scipy, g++ and clang++.

1. `gen_left.py` builds an FM signal straight from the standard's formula
   (ITU-R BS.450, pilot-tone system; 47 CFR 73.322 describes the same system):
   `MPX = M + S sin(2wt) + pilot sin(wt)`, `M = (L+R)/2`, `S = (L-R)/2` - with a 1 kHz tone on
   the LEFT only. A correct decoder must put it on the left.
2. The same file, and optionally a real recording, also goes through a second, independent
   decoder: ngsoftfm (the SoftFM family). `compare.py` lines the two decodes up and tells
   whether left/right and the polarity agree.

## What it found (2026-10-02)

| | test tone ends up | real KLOS 95.5 vs the reference |
|---|---|---|
| reference (ngsoftfm) | left (correct) | - |
| the app before the fix | **right** | L-R correlation **-0.996** = swapped |
| the app after the fix | left | L-R correlation +0.996, same polarity |

Gqrx (GNU Radio) uses the same sign as ngsoftfm. The app had the opposite sign because in
September 2026 its analog decoder was matched to the HD audio coming out of nrsc5 - and that
turned out to be the swapped one: with a standard-conforming analog decoder, nrsc5's HD audio
is `left = -(analog right)`, `right = -(analog left)` on all 20 Los Angeles stations measured.
A station sends one stereo program on both its analog and its HD side, and HD radios blend
between them, so the two must agree. The app now decodes the analog per the standard. The HD
audio was corrected where it arrived in the app (`fixHdAudio()` in `native-lib.cpp`) until
build 27; since build 28 our nrsc5 fork does it (`src/output.c`, branch `hidef` of
github.com/derek20la/nrsc5) - see "nrsc5 itself" below.

One more check that was needed for the polarity: in a KLOS 95.5 recording the station found
at +400 kHz in the samples is KAIA 95.9 (by its RDS), so the dongle's spectrum is the right
way up and "carrier up = positive audio" holds.

Why one earbud didn't settle it: on KLOS the left and right channels are 85-87 % alike, so
a mirrored stereo image is very hard to hear on ordinary music.

## nrsc5 itself (2026-10-24)

The same check on nrsc5's own command-line output (`nrsc5 -r recording.cu8 -o hd1.wav 0`,
HD1 vs ngsoftfm's analog of the same recording, `compare.py` after a WAV -> float32 step):

| | stock nrsc5 v3.2.0 | our fork, branch `hidef` |
|---|---|---|
| KLOS 95.5: mono / side correlation | **-0.973** / +0.953 = swapped + inverted | +0.973 / +0.953, same way round |
| KRTH 101.1: mono / side correlation | **-0.962** / +0.839 = swapped + inverted | +0.962 / +0.839, same way round |

Where it comes from: nrsc5 hands FAAD2's PCM straight on, so it is in the HDC decoding. FM
HD1 frames are a stereo pair with M/S stereo on some or all frequency bands; a test decoder
that took the bands coded as plain left / right the other way round matched the analog
clearly worse (side 0.733 instead of 0.953), so it is the pair's channel order plus an
overall sign - not only the sign of the mid channel. The fork corrects the 16-bit output
once, for every program (`src/output.c`, marked "HiDef Radio patch").
