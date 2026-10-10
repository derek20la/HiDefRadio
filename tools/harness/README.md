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
- `IQLOOP=1` plays the recording round and round, so a ten-second file can stand in for ten
  minutes of listening (the seam is a click, like a retune without one).
- `HDREST=off` the HD search never rests (`hdsearch.hpp`). The output is then bit for bit
  what build 8 played.
- `UNMATCHED=hd` the app's own default for "If the HD signal is another station's": play it
  anyway. (Not set = stay on the analog, as every test before build 9 ran.)
- arguments: frequency in Hz, source (`0` Digital only, `1` Analog only, `2` Auto), seconds,
  then optional events `t:prog:N` (switch to program N at t seconds; 0 = HD1) and `t:src:N`.

It prints one status line every 0.25 s (sync, alignment, blend state) plus the app's own log lines.
The columns `hd on 7% demod 5%` are the two loads the app shows in its signal details: the
time nrsc5 and the FM demodulator take, in % of real time (`hd rest` / `hd look` = the HD
search is resting / taking a look). The last line, `COST over ...`, is their average over
the run and the processor time of the whole harness - the figure to compare before and
after a change that is meant to save battery.

How it works: `rtl_stub.c` is a fake librtlsdr that "receives" the file, `fftw3.h` is a tiny
stand-in for FFTW, `apptest.cpp` includes `native-lib.cpp` and calls its JNI functions with a
fake `JNIEnv`. nrsc5, FAAD2 and the librtlsdr headers are downloaded into `build/`.

`blendtest` (`build/blendtest`) tests `blend.hpp` alone with made-up audio.

## The rest of the HD search: searchtest and mkhdgap

In Auto, a station without HD made nrsc5 search for ever, which costs more than decoding.
Since build 9 the search rests after 30 s and looks again for 3 s in every 30
(`hdsearch.hpp` has the rule and the reasons). Two tools for that:

    build/searchtest              # the rule alone, in made-up time: PASS / FAIL (instantly)
    build/searchtest v            # ... with every change of state printed

    build/mkhdgap kost.cu8 nohd.cu8             # the whole recording without its HD
    build/mkhdgap kost.cu8 late.cu8 0:45        # the HD only appears after 45 s
    build/mkhdgap kost.cu8 lost.cu8 4:50        # HD for 4 s, gone, back at 50 s
    IQFILE=late.cu8 IQSTEP=1 PRINT=1 build/apptest 103500000 2 74

`mkhdgap` takes the HD Radio signal out of a recording for the seconds you name and leaves
the analog station as it was: the HD carriers sit beside the analog signal (from 129 kHz
out), so a low-pass filter on the IQ samples removes them. That gives the two things no
real station does on request: a station that has no HD while its twin with HD is on file
for comparison, and HD that comes and goes at a known second.

What to look for in `apptest`'s output: `hd rest` from 30 s on, a few lines of `hd look`
every 30 s, and in the `late` file `SYNC` during the look that follows the 45 s mark, then
the usual alignment and `blend hd`. Run the same file with `HDREST=off` to see what it
cost before.

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

## Two stations on one frequency: identtest, mkcochannel and calls.sh

A frequency is not a station. With a distant station coming in over a local one, the HD can
be one station's and the analog heard another's - and then nothing the app says or plays may
treat the two as one (`stationid.hpp` has the rules and the three sightings behind them;
`blend.hpp`, "the HD plays alone"). Three tools:

    build/identtest               # stationid.hpp alone against the app's real station list: PASS / FAIL
    sh calls.sh 99.1              # the calls the list has on 99.1, as the app hands them to the engine
    build/mkcochannel kost.cu8 xetra.cu8 two.cu8 74 36:43
    CALLS="$(sh calls.sh 103.5)" UNMATCHED=hd IQFILE=two.cu8 IQSTEP=1 PRINT=1 build/apptest 103500000 2 74

`identtest` needs no recording. It feeds the judge what the engine would - the calls licensed
on the frequency, the HD's name, the RDS decoder's status, which station's RDS blocks came in
lately, the aligner's attempts - for the cases that were met on the air: XHITZ-FM's code
that spells "WHTZ", KDSC's HD under XETRA-FM's analog (first meeting, the evening's sequence,
and with the owner remembered), a station that changed its code, KGGI's wrong code with a
weak HD (the false "another station" of 2026-10-02 must never come back), KTWV's HD that
names itself "WAVE", KHHT / KSOF, and ordinary stations where nothing may happen.

`mkcochannel` makes the recording nobody can make on demand: the HD carriers of one recording
with the analog of another on top, the two analogs trading places (`36:43` = from 36 s to
43 s the HD station's own analog is the stronger one; everywhere else the other station wins,
as an FM receiver only hears the stronger of two). A recording of a station whose level moves
(an antenna that was touched) makes the two flip by themselves - that is realistic. Combine
it with `mkhdgap` to take the HD away for a while (`mkhdgap two.cu8 gap.cu8 20:34`: more than
ten seconds of silence, so the safety net gives the other station's analog back).

`OWNER="KOST 36DF"` hands the engine the owner a saved logo's file name would carry (the HD
station's name and its own RDS code; `OWNER=KOST` = the code is not known).

`apptest`'s status line ends with `rds F000 '' same 2 '' contest 2 own 36DF`: the RDS code and
the call that may be shown (`maybe WHTZ` = a call only the formula gives), the same-station
verdict (0 can't tell, 1 yes, 2 no) with the analog station's name, the contest (0 none,
1 suspected, 2 confirmed) and the HD station's own code; `blend alone` = the HD plays alone,
`SILENT 3 s` = it has dropped out and the silence is waiting for it.

`blendtest` has the blend's side of it with made-up audio: the HD alone, silence in a
dropout and never a frame of the other station, the safety net after 10 s, "stay on analog",
and the way back to the ordinary aligned blend.
