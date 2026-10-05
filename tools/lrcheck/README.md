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
between them, so the two must agree. The app now decodes the analog per the standard and
corrects the HD audio where it arrives (`fixHdAudio()` in `native-lib.cpp`).

One more check that was needed for the polarity: in a KLOS 95.5 recording the station found
at +400 kHz in the samples is KAIA 95.9 (by its RDS), so the dongle's spectrum is the right
way up and "carrier up = positive audio" holds.

Why one earbud didn't settle it: on KLOS the left and right channels are 85-87 % alike, so
a mirrored stereo image is very hard to hear on ordinary music.
