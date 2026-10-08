# The desktop harness

Runs the app's **whole native engine** (`app/src/main/cpp/native-lib.cpp` with the FM
demodulator, the aligner, the blend, RDS and libnrsc5) on a PC against an IQ recording -
no phone, no dongle. It is how every DSP step of HiDef Radio was tested, and it is a small
example of driving the engine outside Android.

    cd tools/harness
    sh build.sh                      # Linux / macOS: git, cmake, gcc, clang++, a JDK (for jni.h)
    IQFILE=klos.cu8 IQPACE=1 WAVOUT=out.wav build/apptest 95500000 2 34

- `IQFILE`  a `.cu8` recording at 1,488,375 samples/s - exactly what `nrsc5 -w file.cu8 95.5 0` writes.
- `IQPACE=1` plays it in real time (needed for anything that depends on timing: Auto, the blend).
  Without it the file runs as fast as possible.
- `WAVOUT`  saves the audio the app would play (16-bit stereo, 44.1 kHz).
- `CALLS`   the call letters licensed on that frequency, as the app's FCC list gives them
  (`CALLS="KGGI KJNY KSQL"`), for the rules that turn an RDS code into a station name.
  Not set = no list.
- `GAIN=auto` starts with the app's auto-gain search (default: a manual gain of 30 dB).
- `DONGLE=V3` the fake dongle says it is an RTL-SDR Blog V3 (R820T tuner): on AM it reports
  direct sampling, and the app must then leave the tuner gain alone.
- `IQSTEP=1` "virtual time": the recording is played as fast as the PC can, and everything
  the app does on a timer (the alignment watcher, the audio player) is driven by the
  recording's own time. The same run gives the same numbers every time, and a minute takes
  seconds. (`IQPACE` is the real thing, threads racing as on the phone.) `PRINT=1` = one
  status line per second.
- `DRIFT=off` switches the clock correction (`drift.hpp`) off. The output is then bit for bit
  what build 5 played - the proof that the correction changed nothing else.
- `SIGDUMP=20` prints the app's whole status text once, 20 s in.
- arguments: frequency in Hz, source (`0` Digital only, `1` Analog only, `2` Auto), seconds,
  then optional events `t:prog:N` (switch to program N at t seconds; 0 = HD1) and `t:src:N`.

It prints one status line every 0.25 s (sync, alignment, blend state) plus the app's own log lines.

How it works: `rtl_stub.c` is a fake librtlsdr that "receives" the file, `fftw3.h` is a tiny
stand-in for FFTW, `apptest.cpp` includes `native-lib.cpp` and calls its JNI functions with a
fake `JNIEnv`. nrsc5, FAAD2 and the librtlsdr headers are downloaded into `build/`.

`blendtest` (`build/blendtest`) tests `blend.hpp` alone with made-up audio.

## The clock correction: mkdrift and drifttest

A dongle with a plain crystal runs 30-100 ppm off. Then the analog audio (paced by the
dongle) drifts against the HD audio (paced by the station) by frames per second, and before
build 6 the blend never started. Two tools for that:

    build/mkdrift krth.cu8 krth+57.cu8 57 101100000
    IQFILE=krth+57.cu8 IQSTEP=1 PRINT=1 build/apptest 101100000 2 60

`mkdrift` turns a recording made with a good dongle into what a dongle whose crystal runs
57 ppm fast would have recorded: the station 5.8 kHz below the centre, and the samples taken
57 ppm too fast. Run it through `apptest` with `DRIFT=off` to see the old behaviour (every
alignment measurement accepted, none confirmed, analog for ever), and without to see the fix.

    build/drifttest               # all scenarios, PASS / FAIL (about 8 minutes)
    build/drifttest 57 120 v      # one run: 57 ppm, 120 s, every measurement printed
    build/drifttest 57 120 none   # ... without the first guess from the tuning error
    build/drifttest 57 120 off    # ... without the correction (must fail)
    build/drifttest 58 180 wander=12 noise=0.05 v   # a station whose analog and HD audio match only loosely
    build/drifttest tracker v     # only the tracker, fed with measurements written down by hand

`drifttest` needs no recording and no nrsc5: it feeds `drift.hpp`, `aligner.hpp` and
`blend.hpp` one made-up program as "HD" and as "analog" with a crystal error of your choice,
in made-up time (an hour takes half a minute). Its scenarios: 0 to +-150 ppm, a first guess
that is wrong, none at all, a crystal that warms up, nrsc5 re-timing its output, lost samples,
a station whose analog and HD audio are processed differently, and an hour of listening at
57 and 100 ppm.

The `tracker` tests need no audio either: the tracker (`drift::Tracker`) gets the tuning
offset and a list of alignment measurements written down by hand. That is how a fault seen
once in the field becomes a test: the first one is a tester's screen video (a dongle 58 ppm
fast, two measurements that agree and a third 14 frames off - build 7 answered
"+103 ppm (measured)"), the second the same thing found on a KKLQ recording.

`apptest`'s status line shows what the tracker decided: `clock 58.2 ppm (guess | measured,
in use ..., tuning says ..., scatter x1.0)`, plus `NOT USED: audio 103.2` when a measurement
of the audio was refused, or `NOT USED: tuning -57.0` when the audio proved the tuning
offset wrong.
