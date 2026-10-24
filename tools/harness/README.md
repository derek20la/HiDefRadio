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
  `DONGLE=FAIL`: the dongle can't be opened (the app's error path).
- `RTLTRACE=calls.txt` the fake dongle writes every driver call the app makes, with its
  arguments, to that file (see "The source split" below).
- events `t:gain:N` (the manual gain setting, tenths of a dB) and `t:again:N` (the gain
  watchdog moving the auto gain) besides `t:prog:N` and `t:src:N`.
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
- `SPEEDTEST=5` presses the app's "Run speed test" button 5 s into the run and prints the
  result at the end (see "What does the radio cost" below).
- `STEREO=mono` / `STEREO=stereo` the app's FM stereo setting (Always mono / Always stereo;
  not set = Auto, as the app ships).
- arguments: frequency in Hz, source (`0` Digital only, `1` Analog only, `2` Auto), seconds,
  then optional events `t:prog:N` (switch to program N at t seconds; 0 = HD1) and `t:src:N`.

It prints one status line every 0.25 s (sync, alignment, blend state) plus the app's own log lines.
The columns `hd on 7% demod 5%` are the two loads the app shows in its signal details: the
processor time nrsc5 and the FM demodulator take, in % of real time (`hd rest` / `hd look` =
the HD search is resting / taking a look). The last line, `COST over ...`, is their average
over the run and the processor time of the whole harness - the figure to compare before and
after a change that is meant to save battery.

### What does the radio cost: the "% CPU" lines and the speed test

On a PC the two loads are a fair measure of work. **On a phone they are not**: a phone has
fast and slow cores and changes their clock speed all the time, so the same code reads
"10 %" on a fast core flat out and "40 %" on a slow core clocked down (seen with build 9).
Since build 12 the loads are the thread's own processor time (before: time on the wall
clock), and the app's signal details say which core the engine ran on and at what speed -
that explains the figure, it does not make it comparable.

To compare two builds on a phone, use the **speed test** (debug info -> "Run speed test";
here: `SPEEDTEST=5`). It records 10 s of the station being played and then runs a fresh FM
demodulator and a fresh HD decoder over the recording flat out, three times each, on a
thread of its own. A thread that never pauses gets a fast core at full speed, and the best
of three runs counts, so the answer is the same every time:

    Speed test: 101.1 MHz, 10.0 s of radio, best of 3 runs
    FM demodulator: 70.2 ms per second of radio, stereo decoder on 96 % of the time (runs: 78.2 / 70.2 / 73.6)
    HD decoder, decoding: 72.9 ms per second of radio (in sync for 9.8 s; runs: 75.5 / 83.6 / 72.9)

A station without HD gives `HD decoder, searching: ...` instead (the search costs more than
the decoding: 190 against 75 ms on the PC these lines come from). Compare like with like:
the same station, a strong one, the same FM stereo setting. With `SPEEDTEST` the `COST`
line's "whole harness" figure includes the test's own work.

How it works: `rtl_stub.c` is a fake librtlsdr that "receives" the file, `fftw3.h` is a tiny
stand-in for FFTW, `apptest.cpp` includes `native-lib.cpp` and calls its JNI functions with a
fake `JNIEnv`. nrsc5, FAAD2 and the librtlsdr headers are downloaded into `build/`.

`blendtest` (`build/blendtest`) tests `blend.hpp` alone with made-up audio.

## The NEON step (build 14): what changed, what is still bit for bit, what is not

Build 14 made the FM demodulator work a block at a time with four floats per instruction
(`vec4.hpp`), switched on nrsc5's NEON paths and FFTW's NEON codelets on arm64. Three facts
for anyone who compares recordings across builds:

- **On a PC the harness still makes bit for bit the audio of build 13.** The filters sum
  their taps in the same four-lane order as before, and the discriminator keeps the C
  library's `atan2f` unless the engine is compiled with `-DFM_FAST_ATAN2=1`. Checked on five
  runs (KRTH Auto / Analog only / Digital only, KKGO HD2 and back, KLLI): `cmp` says identical.
- **On an arm64 phone the audio is not bit for bit the same, and cannot be:** NEON's
  multiply-add rounds once instead of twice, and the discriminator uses a polynomial
  arc tangent (vec4.hpp; 4.8e-7 rad worst case against the C library). What it comes to was
  measured with the same engine built for arm64 and run in an emulator (zig cc + qemu-user):
  in mono the audio differs from the PC's by 1 LSB (of 16 bits) on 0.03 % of samples; in
  stereo by up to 4-5 LSB (-84 dB) on about 1 % of samples, because the pilot PLL's sine table
  has 4,096 steps and a 1e-7 difference in the input flips a step now and then (rms of the
  difference: 4e-6, -108 dB). The blend's decisions ("HD is good" at the same second, the
  same fades) and the alignment figures were the same in every run. The HD audio itself
  differs between a PC and an arm64 build by 1 LSB on 0.1 % of samples even with no NEON
  anywhere - that is nrsc5's and FAAD2's own float arithmetic on two architectures.
- **nrsc5's NEON paths and FFTW's NEON codelets change nothing in the HD audio:** bit for
  bit the same on arm64 with and without them (four runs each), as expected for a decoder
  whose output is bits.

How the arm64 engine was run without a phone: `python3 -m pip install ziglang` gives a C/C++
compiler that cross-compiles for `aarch64-linux-musl` (`python3 -m ziglang cc -target
aarch64-linux-musl`), FAAD2 and FFTW build with CMake and a two-line toolchain file that names
it, the rest of the harness builds like `build.sh` does with `gcc` / `clang++` replaced; the
static `qemu-aarch64` from the multiarch/qemu-user-static releases runs the result on a PC
at about real time. Not a speed measurement (an emulator), but every instruction the phone
will execute runs there, and `WAVOUT` + the Python below compare the audio:

    import numpy as np, wave
    a, b = [np.frombuffer(wave.open(f).readframes(10**9), np.int16).astype(float) for f in ("ref.wav", "new.wav")]
    n = min(len(a), len(b)); d = a[:n] - b[:n]
    print("max |diff| %d LSB, rms %.2g, differing %.2f %%" % (abs(d).max(), np.sqrt((d*d).mean()), 100 * (d != 0).mean()))

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

## The source split (build 17): RTLTRACE

Build 17 moved every librtlsdr call out of `native-lib.cpp` into `iqsource_usb.hpp`, behind
the interface in `iqsource.hpp` - so that a second source (rtl_tcp) can be added without
touching the radio. A move like that must change nothing the dongle sees and nothing you hear.
The proof: run the old and the new engine on the same recording in virtual time and compare
both the audio and the list of driver calls:

    IQFILE=krth.cu8 IQSTEP=1 GAIN=auto WAVOUT=new.wav RTLTRACE=new.txt build/apptest 101100000 2 40
    (the same with the build 16 engine -> old.wav, old.txt)
    cmp old.wav new.wav && diff old.txt new.txt

The last line of the trace ("read_async: N blocks delivered") counts how many blocks the fake
dongle sent before the harness stopped it; that number moves by a block or two from run to
run even with the same engine (the harness's own threads), so leave it out of the diff.

Result on 2026-10-16, build 16 against build 17, 14 runs: audio bit for bit
the same and the same driver calls in every one - KRTH Auto with auto-gain, KRTH with manual
and watchdog gain changes, KRTH Digital only, KKGO HD1 -> HD2 -> HD1 -> HD4, KLLI, KYSR
Analog only, KOST with its dropouts and four source switches, KKLA (no HD, the search rests),
KKLQ, AM 1260 on a V4, AM 1260 on a V3 (direct sampling: no gain calls at all, also when the
gain is asked for), KRTH on a V3, and a dongle that fails to open.

## Analog AM (build 18): amtest, and AM in apptest

Build 18 gives the AM band its analog side (`amdemod.hpp`: synchronous detector, C-QUAM
stereo) and lets the aligner and the blend work on AM. Three tools:

    build/amtest                                   # amdemod.hpp alone on made-up signals: 33 checks, PASS / FAIL
    build/amtest 1260-1.cu8 out.wav [secs] [bw] [on|off]   # a recording: the meter once a second, the audio as a WAV
                                                   # (on|off = NRSC-1 de-emphasis, build 21; off when not given)
    build/amtest gen plain|cquam out.cu8 [secs]    # a made-up AM station as a .cu8 for apptest (no HD)
    IQFILE=1260-1.cu8 IQSTEP=1 UNMATCHED=hd WAVOUT=am.wav build/apptest 1260000 2 46   # the whole engine, AM in Auto

The made-up signals come from the standards' formulas, not from the decoder's idea of them:
plain AM = (1 + m) cos(wt), C-QUAM = (1 + L + R) cos(wt + atan((L - R) / (1 + L + R))) with
the 25 Hz pilot at 4 %. The checks: a 50 % tone comes out at 50 %, the carrier offset reads
right, the meter reads the true carrier-to-noise within 2 dB (the 8-bit rounding of the
samples sets a floor near 50 dB), a drifting carrier is followed, a signal in quadrature
(where hybrid HD puts its tertiary carriers) is rejected by 32 dB, a tone on the left of a
C-QUAM signal comes out on the LEFT at 39 dB separation with the pilot notched out of the
audio, hdSynced switches the stereo off, noise at 28 dB C/N narrows it, and noise alone
leaves the output bounded. The `apptest` status line for AM shows the analog side after a
`|`: locked, carrier dBFS, C/N, offset, pilot, stereo %, bandwidth.

Two things the recordings taught, both built in (the headers say more):

- **On AM the HD is not 2.5 s early.** nrsc5's AM audio came out in step with the analog on
  both stations (KMZT 1260: the analog 0.16 s ahead, KBRT 740: 0.12 s ahead), so the blend
  plays the analog ~3 s behind live in Auto (`blend::Blender::setRestartLatency`), which gives
  the dropout look-ahead its room. Analog only stays live.
- **The waveforms do not match, the envelopes do.** Analog vs HD waveform correlation on AM:
  r 0.41-0.58 (an AM transmitter's audio processor phase-rotates the analog; the HD feed is
  untouched). So `aligner.hpp` has an envelope mode for AM (|audio| low-passed at 30 Hz):
  r 0.72-0.95, the lag good to a millisecond or two, "agree" / "jump" widened to 200 / 800
  frames, and the blend's re-align tolerance to 200. Two more rules keep a mostly-empty HD
  stretch from matching by accident: at least 30 % of the HD must be real audio, and the two
  must be within 12 dB of each other in loudness ("4568 ms, r 0.89, analog -21 dB" was a
  step edge in zero-filled pieces after a sync loss).

Results on 2026-10-16 (virtual time, Auto, "play the HD anyway"): KMZT 1260-1 (46 s): analog
at once, aligned at 14 s, HD from 16 s to the end, 7 of 7 measurements accepted (r 0.84-0.92).
1260-2 (61 s, two sync losses): HD at 16 s, each dropout seen ahead, back on HD at 38 and
60 s. 1260-3: HD at 16 s. KBRT 740-1 (51 s, speech): HD at 18 s, 7 of 7 accepted (r
0.72-0.81), two dropouts caught. 740-2: HD at 30 s. 740 (weak, BER 7 %, sync losses): stays on
the analog - right. A made-up plain AM station without HD: the HD search rests at 30 s. A
made-up C-QUAM station: left and right come out on the right sides in Analog only and Auto,
mono in the mono setting.

**What is still bit for bit build 17** (`cmp` on the WAVs, same harness driver): FM in every
mode tried - KRTH Auto (50 s), KLLI Auto, KKGO HD1 -> HD2 -> HD1 -> HD4, KKLA with the search
resting over 75 s (IQLOOP), KKLQ "stay on analog", KRTH Digital only, KRTH Analog only in
forced stereo, KRTH with auto-gain and gain changes - and AM in Digital only on a V4 and on a
V3 (direct sampling, no gain calls). **The one FM change:** after Digital only -> Auto the
first 0.3 s used to come out of the analog ring as it stood (audio from a lap of the ring
earlier); it is silence now (`blend::Blender::skipAnalogTo` clears the skipped frames). Found
because the ring grew from 4 s to 8 s for AM's hold-back and that run's stale 0.3 s changed.

Cost on the PC: the AM demodulator 8.7 ms of processor time per second of radio against the
FM demodulator's 35 on the same machine (speed test, `SPEEDTEST=5`).

### NRSC-1 de-emphasis (build 21): amtest check 9, `AMDEEMPH=off`

`amdemod.hpp` takes out the treble boost US AM stations add (NRSC-1: +6.9 dB at 5 kHz, +10 dB
at 10 kHz), as the standard asks of receivers - a one-pole / one-zero filter fitted to the
standard's receiver curve (the bilinear transform was 0.5 dB off at 5 kHz). It is ON in the
app by default (Settings -> AM de-emphasis) and in `apptest` unless `AMDEEMPH=off`; `amtest`'s
own demodulator is flat unless told. amtest check 9: tones from 100 Hz to 9.5 kHz with it on
against off follow NRSC-1 Table 2 within 0.01 dB (-0.81 dB at 1 kHz, -6.93 at 5 kHz, -9.82 at
9.5 kHz); off is bit for bit the build 20 output; C-QUAM's separation is unchanged (39.5 dB).

Results on 2026-10-24 (virtual time, Auto, "play the HD anyway"): with `AMDEEMPH=off` KMZT
1260-3 (Auto and Analog only) and KBRT 740-2 are bit for bit build 20, and so is FM (KRTH Auto
and Analog only, KKGO with sub-channels, KKGO Digital only). With it on, the analog matches the
HD better (the HD audio is not pre-emphasized): KMZT r 0.77 / 0.83 / 0.74 -> 0.83 / 0.84 /
0.71, analog +2.2 / +1.1 dB -> +0.7 / +0.3 dB against the HD; KBRT r 0.82 / 0.80 / 0.67 / 0.82
-> 0.89 / 0.84 / 0.72 / 0.92, and on the HD at 25 s instead of 29 s. Same delays, one fade to
the HD, none back.

## "Comments only": objcheck.sh

A change that is meant to touch only comments (the documentation pass of 2026-10-15, for one)
must leave the program exactly as it was. `objcheck.sh` proves it: it compiles `native-lib.cpp`
with every header, and the stand-alone tests, three ways - clang++ and g++ on the PC, and clang
for 64-bit ARM (the phone's code path with NEON; needs `pip install ziglang`) - without debug
information, and prints a SHA-256 for each object file.

    sh build.sh                      # once (fetches the nrsc5 / librtlsdr headers)
    sh objcheck.sh > before.txt      # on the old sources
    ... edit ...
    sh objcheck.sh > after.txt && diff before.txt after.txt && echo IDENTICAL

Comments that move code to other lines don't show up (no line numbers in the objects); a
change of one digit in a constant does (tried: `PILOT_OFF` 0.012 -> 0.013 changes all three
`native-lib.o`).
