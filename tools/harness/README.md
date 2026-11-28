# The desktop harness

Runs the app's **whole native engine** (`core/src/native-lib.cpp` with the FM
demodulator, the aligner, the blend, RDS and libnrsc5) on a PC against an IQ recording -
no phone, no dongle. It is how every DSP step of HiDef Radio was tested, and it is a small
example of driving the engine outside Android.

    cd tools/harness
    sh build.sh                      # Linux / macOS: cmake, gcc, clang++, no JDK (jni.h is our own)
    IQFILE=klos.cu8 IQPACE=1 WAVOUT=out.wav build/apptest 95500000 2 34

**`regress.sh`** (macOS port, step 0a) is the proof that a refactor changed nothing: it builds,
plays a fixed set of runs (`IQSTEP=1`) of five recordings - KMZT 1260-3, XEKA 950, KKLQ, KOST,
Z90 - in HD only / Auto / Analog only, with a program switch, mono, clock correction off,
auto-gain, an IQ recording and the rtl_tcp path, plus blendtest, drifttest, amtest, identtest,
pstest, gaintest and searchtest, and prints one SHA-256 per output (about 90 s on an M-series Mac).
WAV and `.cu8` outputs must match byte for byte; the text logs are compared with the real-clock
figures blanked (the list is in the script). The recordings stay outside the repo:

    sh regress.sh check                                      # runs, then compares with regress-baseline.txt
    RECORDINGS="/path/to/IQ" sh regress.sh > regress-baseline.txt   # record a new baseline

`regress-baseline.txt` was recorded on the engine of `main` 6dbcf17 with Apple clang 21; its
`# build:` lines (compiler, flags) must match too, so a difference can only come from the code.

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
- events `t:rec:N` (1.3: start an IQ recording into `IQREC`, default `iqrec.cu8` - a second one
  goes to `iqrec.cu8.2` - with an N s limit, 0 = none) and `t:recstop:0` (stop it). The run prints
  what was saved and why it ended; the file must be an exact slice of `IQFILE`.
- `RDS: PS text "..."` in the log (build 37): a station that scrolls its song through the PS
  (91X: "THE" "MIDDLE" "by Jimmy" ...) put back together by `psstitch.hpp`, once it has gone
  round. `build/pstest` checks the stitching on made-up sequences (garbled frames, a new song,
  a sliding window) that the recordings don't have.
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
fake `JNIEnv`. nrsc5, FAAD2 and the librtlsdr headers are copied into `build/` from the app's
vendored copies (`core/external`), so the harness runs exactly the decoder the app
ships, HiDef Radio's nrsc5 patches included (until build 26 it downloaded them from GitHub).
`NRSC5_SRC=<dir>` builds another nrsc5 tree instead - see "The faster HD search" below.

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

## rtl_tcp (1.2): rtltcp_replay.py, SOURCE=tcp:..., tcptest

`core/src/iqsource_tcp.hpp` (iq::TcpSource) takes the samples from an rtl_tcp server
instead of the USB dongle. To test it without a dongle, `rtltcp_replay.py` is a stand-in
server that plays a .cu8 recording and speaks the real protocol (12-byte "RTL0" greeting, raw
cu8, 5-byte commands, which it logs with `--trace`):

    python3 rtltcp_replay.py krth.cu8 1234 --trace calls.txt &          # "lockstep": starts at the first tune
    SOURCE=tcp:127.0.0.1:1234 IQSTEP=1 UNMATCHED=hd WAVOUT=tcp.wav build/apptest 101100000 2 30
    tail -c +1190701 krth.cu8 > krth_cut.cu8                             # TcpSource drops 0.4 s after a tune
    IQFILE=krth_cut.cu8 IQSTEP=1 UNMATCHED=hd WAVOUT=usb.wav build/apptest 101100000 2 30
    cmp tcp.wav usb.wav                                                  # identical

**Build 32: a V3 behind the server on AM.** The replay ignores the gain commands, which is
exactly what a V3 in direct sampling does on AM (the server can't tell us). On AM over
rtl_tcp the auto-gain now notices it - the tried gains span 20 dB or more and the peaks moved
less than 6 dB - and treats it like direct sampling (`Gain: no effect`, no watchdog, nothing
remembered). On FM the replay ignores the gain too, but there the check is never made, so the
FM runs above stay bit for bit. `GAINHINT=297` gives the auto-gain a remembered gain (29.7 dB)
to start from:

    python3 rtltcp_replay.py 740-1.cu8 1234 &
    SOURCE=tcp:127.0.0.1:1234 GAIN=auto GAINHINT=297 IQSTEP=1 UNMATCHED=hd SIGDUMP=10 build/apptest 740000 2 20
    # auto-gain: the gain has no effect (0.0 ... 29.7 dB tried, peak -8.6 ... -8.3 dBFS) - ...

`--live` sends like a real server (from the moment of connecting, real time, whatever the
client does); `--speed X`, `--cut-after S` (the server dies after S seconds), `--tuner N
--gains N` (what the greeting says). `apptest` gets `SOURCE=tcp:host:port`; `build.sh` builds
it with `-DHIDEF_HARNESS`, which gives TcpSource the virtual-time hook. `tcptest.cpp` is the
source alone against a live replay, for the thread sanitizer:
`g++ -std=c++17 -fsanitize=thread -I build/stub -I ../../core/src tcptest.cpp -o tcptest -lpthread`.

Results on 2026-10-24: through the socket = bit for bit the USB path on KRTH (FM, Auto, 30 s)
and KBRT 740 (AM, Auto, 40 s); the server got rate / freq / gain mode / gain 30.0 dB. Live
server: auto-gain (5 tries sent as commands, ~0.9 s - each measurement waits 0.12 s for the new
gain), KKGO HD1 -> HD2 -> HD1, blend to HD. `--cut-after 8`: "the server closed the
connection", stream returns -1, `sourceLost=1`. No server / unknown host / not an rtl_tcp
server: each gives its own reason. A link at 85 % of the needed rate: no errors, the engine
waits. tcptest under the thread sanitizer: 10 checks, no reports. And the USB path is bit for
bit build 23 (KRTH Auto, KKGO with auto-gain and sub-channels, KMZT, KBRT on a V3).

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

    sh build.sh                      # once (copies the nrsc5 / librtlsdr headers into build/)
    sh objcheck.sh > before.txt      # on the old sources
    ... edit ...
    sh objcheck.sh > after.txt && diff before.txt after.txt && echo IDENTICAL

Comments that move code to other lines don't show up (no line numbers in the objects); a
change of one digit in a constant does (tried: `PILOT_OFF` 0.012 -> 0.013 changes all three
`native-lib.o`).

## The faster HD search (build 27): nrsc5's sync.c, before / after with NRSC5_SRC

Build 27 is the first build with a patched nrsc5: our fork `derek20la/nrsc5`, branch `hidef`
(v3.2.0 + one commit per fix). Fix 1 is the HD search: while nrsc5 has no sync,
`detect_cfo()` in `sync.c` ran 76 trial frequency offsets x 22 reference carriers x 32
symbols per block, each step with two `cexpf()` and a `cargf()` - about 1.7 million libm
calls a second. The search now uses its own copy of the loop (`adjust_ref_search()`): a sine
table and a polynomial arc tangent, the same arithmetic in the same order. Every spot is
marked `HiDef Radio patch (2026-10-24)` in the source.

To compare with stock nrsc5, build a second harness from an unpatched v3.2.0 tree:

    git clone --branch v3.2.0 https://github.com/theori-io/nrsc5.git /tmp/nrsc5_orig
    cp -r ../harness ../harness_old && cd ../harness_old
    NRSC5_SRC=/tmp/nrsc5_orig sh build.sh

and run the same command with both `build/apptest`s (virtual time, `WAVOUT=`, then `cmp` the
WAVs and `diff` the logs with the "ms after tuning" and CPU-% figures taken out).

Results on 2026-10-24 (x86, virtual time; 33 runs, Digital only and Auto on every file):
**the same SYNC / LOST SYNC sequence, the same frequency offsets and the audio bit for bit
the same** on KRTH, KOST (5 syncs / 4 losses), KLOS, KKGO (HD2 and back), KGB, KPBS, KGGI
(weak), KYSR, KLLI (3 / 2), the marginal 94.9 / KYMI (16 / 15) / KWIZ (9 / 8), KKLA (no HD),
KMZT 1260 AM, plus `HDREST=off` and "play the HD anyway" runs. The only log differences are
the order of two start-up lines (two threads) and the "synced" grace period, which runs on
the real clock: the same old binary gives different values from run to run there.
Cost without HD (KKLA, search never resting): HD decode 16.6 % -> 10.0 % of real time in
apptest; libnrsc5 alone in pipe mode 171 -> 102 ms of processor time per second of radio.
With HD (KRTH) decoding is unchanged (77 -> 73 ms/s, within noise).


## HD audio left / right (build 28): nrsc5's output.c, before / after

Fix 2 in the fork (`derek20la/nrsc5`, branch `hidef`, commit `894585e`): stock nrsc5 hands out
its HD audio with left and right swapped and both inverted (`tools/lrcheck/README.md`). Until
build 27 the app corrected that in `native-lib.cpp` (`fixHdAudio()`); now nrsc5's
`output_advance()` corrects FAAD2's 16-bit output, exactly as the app did, and the app's
function is reduced to the station's TX digital audio gain (`applyHdGain()`).

The proof is the same as for the HD search: the harness built from build 27 (old) and from
build 28 (new), the same recording through both, virtual time, `WAVOUT=`, `cmp` the WAVs and
`diff` the logs (with the "ms after tuning" times, the CPU-% figures and the column padding
taken out). Results on 2026-10-24 (x86), **every WAV bit for bit the same and every log the
same**:

| recording | runs |
|---|---|
| KRTH 101.1 (52 s) | Digital only, Auto |
| KLOS klos1 95.5 (39 s, TX gain +4 dB = the gain path) | Digital only, Auto |
| KKGO 105.1 (16 s) | Digital only HD1 / HD2 / HD3 / HD4, Auto (HD2 at 5 s, back at 12 s) |
| KMZT 1260-3 AM (24 s, `UNMATCHED=hd`) | Digital only, Auto |

Exactness: with gain 0 the app now passes nrsc5's samples on untouched; with a gain the app
used to compute `nrsc5 sample x -10^(dB/20)` and now `(-nrsc5 sample) x 10^(dB/20)`, which is
the same float product. The only value that could differ is a raw -32768 on a station that
sends a gain (nrsc5 now turns it into +32767 first) - a full-scale sample; none occurs here.


## MER sideband labels (build 29): nrsc5's sync.c, before / after

Fix 3 in the fork (`derek20la/nrsc5`, branch `hidef`, commit `16e6142`): in FM, nrsc5 mirrors
the spectrum before decoding (`acquire.c`, `cq15_to_cf_conj`), and stock nrsc5 v3.2.0 named
the two HD sidebands after the mirrored spectrum - its MER event's "lower" was the sideband
ABOVE the carrier. From 9b (2026-10-02) to build 28 the app swapped the two values back in
`native-lib.cpp`; now nrsc5's `sync_process_fm()` reports them the right way round
(lower = below the carrier, upper = above) and the app takes them as they come. AM sends no
MER event, so it is not affected.

The proof: the harness built from build 28 (old) and from build 29 (new), each with the same
sandbox-only probe line after the MER event (it printed nrsc5's two values and the app's two,
as exact hex floats - not part of the app), virtual time, `WAVOUT=`, `cmp` / `diff`.
Results on 2026-10-24 (x86), 10 runs, Digital only and Auto on each of KRTH 101.1, KLOS klos1
95.5, KKGO 105.1 (HD2 at 5 s, back at 12 s), KYSR 98.7 and KGGI kkgi2 99.1 (weak, very
uneven sidebands: 8.1 / 2.0 dB): **every MER figure the app shows bit for bit the same (108
readings per mode, 216 in all), nrsc5's raw values exactly swapped, every WAV and every log the same** (logs
with the times, the CPU-% figures, the column padding and two start-up lines from two threads
taken out).

Which side is which, checked directly: KYSR with +129..+199 kHz deleted from the recording
(a block FFT, `numpy`): stock nrsc5 said "lower -13.9, upper 13.1" - the dead sideband named
"lower"; the fork says "lower 13.1, upper -13.9". With -199..-129 kHz deleted the fork's
"lower" collapses (-16.1 / 9.8). The app's screen showed the dead sideband as "upper" in both
builds.

## The FM auto-gain (build 39): GAINFILES, gaintest, gainkeepertest

Until build 38 the harness could not test the auto-gain search at all: a recording is made
at ONE gain, so whatever gain the app set, it got the same samples back. Build 39's search
judges a gain by the signal itself (how far the HD sidebands stand out of the noise floor,
`gainscore.hpp`), so the fake dongle learned to answer a gain change:

    GAINFILES="37:kost_3.7.cu8,87:kost_8.7.cu8,144:kost_14.4.cu8,207:kost_20.7.cu8,280:kost_28.0.cu8" \
        GAIN=auto IQSTEP=1 build/apptest 103500000 0 10

- `GAINFILES` one recording per tuner gain (tenths of a dB `:` file), all of the same station,
  made one after the other with the manual gain setting (the app's own IQ recorder does it:
  ~15 s each). Whenever the app sets a gain, the fake dongle goes on with the recording made at
  the NEAREST of those gains, at the same time offset, and says so (`STUB: gain 16.6 dB -> the
  14.4 dB recording`; `GAINFILES_QUIET=1` = don't). `GAIN=auto` then runs the app's real search
  on real signals - a station whose HD dies at 20.7 dB dies here too - and a `t:again:N` event
  changes the signal as the watchdog would.
- `GAINSTEPS=files` the fake tuner has ONLY those gains as its steps. (Else it has the V4's 29
  steps and several of them share one recording, which the search sees as "all the same".)
- `GAINSTART=2.5` starts 2.5 s into the recordings: the same search on another stretch of the
  same signals (run it for 0, 0.5, 1 ... to see how often the search picks what).
- `GAINHINT=207` a remembered gain, as before: the way to replay "the app remembered a gain that
  no longer works".
- `GAINHINTQ=126` (build 39a) what GainMemory knows about that gain: tested, and its HD sideband
  stood 12.6 dB over the floor then (`0` = tested, no figure). Such a gain gets one look and no
  search if it still fits. Not given = remembered by build 38 or older: the search starts from it.

The log shows every measurement (`auto-gain: try 14.4 dB -> peak -11.5 dBFS, HD sidebands 10.7 /
11.3 dB over the floor`) and the choice. `SIGDUMP=8 PRINT=4` adds the decoder's verdict on it
(`merLower`, `merUpper`, `berAvg`, `crcErrors`).

`build/gaintest file.cu8 ...` runs `gainscore.hpp` alone over whole recordings, burst by burst:
the mean and the scatter of "sideband over floor" per side, and in how many bursts a side
"looks like HD". It is how the HD-or-not thresholds were checked (stations with HD: 99.9-100 %
of the bursts at any usable gain; a neighbour's analog signal across the sideband: 0-3 %).

`gainkeepertest.kt` tests the watchdog (`GainKeeper.kt` is plain Kotlin) on a made-up station
whose HD is best at one gain and gone two steps up; `sh gainkeepertest_38.sh` (needs `kotlinc`,
`java` and the git history) compiles build 38's GainKeeper in beside the current one and also
checks that without a quality sweep (AM, stations without HD) every answer is the same as
build 38's, over 300,000 random seconds.

### Build 41: "quiet but overloaded", and what a fix costs a station without HD

Two kinds of test that real gain-step recordings can't do on their own, both made with
`scaleiq.py` (numpy):

- **An overload the peak can't see** (Long Beach, 11-20: strong stations outside our 1.5 MHz
  overloaded the tuner, the peak read a calm -10 dBFS, and build 40 said "no HD sidebands"
  while the HD was fine 20 dB lower). Take a real gain-step set and scale the high-gain files
  DOWN until their peaks read -9 ... -12 dBFS: the overload damage stays in the samples, the
  peak no longer shows it. Build 41 used the 11-23 KDAY set (40.2 -9, 33.8 -9.5, 28.0 / 22.9 /
  20.7 -10, 19.7 -8, 14.4 -2 dB; 8.7 as it was):
  `python3 scaleiq.py kday_402.cu8 kdayc_402.cu8 -9 12` and so on, then GAINFILES with the
  `kdayc_` files. Build 40: 40.2 dB "no HD" in 16 of 16 tunes; build 41: 8.7 or 14.4 dB.
- **A station without HD at every gain.** `python3 scaleiq.py --steps 903.cu8 a903 297 4`
  makes one file per V4 step out of one recording, so the peak follows the gain; GAINFILES
  with all 29 (`GAINSTEPS` not set). Build 41's longer look for HD lower down costs such a
  station one burst more than build 40 in most tunes (three at most, on 879), with the same
  gain chosen in every tune.

`gainkeepertest.kt` tests 8-12 are build 41's watchdog rules (FM only: a real clip goes 6 dB
down at once; a synced station whose MER is below zero gets an early try 3 dB down). With
`gainkeepertest_38.sh`, tests 11 and 12 also prove what did NOT change: FM without HD = build 38,
FM with HD but no clip and no MER below zero = build 40, answer for answer.

**The watchdog on real signals** (build 41). `FMTRACE=1` makes `apptest` print, once a second,
what RadioService hands the GainKeeper: the second's loudest peak, sync, the MER reading count
and values, and the BER. Run each file of a gain-step set at its own gain with it (one trace
file per gain, `<station>_<gain>.txt`), and `gainkeeperreplay.sh` replays build 41's GainKeeper
next to build 40's on them: each second comes from the recording at the gain the watchdog has
set right now. It is also the quickest way to read a file's MER and BER off ("ground truth").
The 11-23 LA set, stuck at 40.2 dB with no sweep: clipping 4.8 % of the time with build 40,
1.2 % with build 41; KOST in sync 95.8 % -> 98.8 %.

AM does not use any of this: `cmp` the `WAVOUT` of an AM run with `GAIN=auto` before and after -
identical (KBRT 740 and KMZT 1260, with and without `GAINHINT`).

### Build 41, final: the LA set, KGGI at night, and the analog's quieting

After the LA gain-step set and live Auto in Long Beach (11-23), build 41 got four more changes
(GainKeeper.kt's top and doAutoGainFm() in native-lib.cpp say why, with the numbers):
a remembered FM gain between -4 and -1.5 dBFS competes with the search instead of being thrown
away; rule 1's 6 dB only for a converter full two seconds running; on FM, a peak that is loud but
not clipping (-1.5 ... -0.5 dBFS) steps down only if it keeps happening; and rule 4, the "no
sync -> lower" walk at the start of a tune. Plus one fix to fix 1: "HD" found at a peak under
-16 dBFS no longer counts (it fooled the search on analog-only 90.3).

- **`FMTRACE=1` now also prints the analog quieting** (`quiet`, Signal.fmQuietDb), the FM
  carrier level and the damaged audio packets so far (`crc`). Make the traces in **Auto with the HD search never resting** (source `2`,
  `HDREST=off`, see gainkeeperreplay.sh), so each second has sync, MER AND quieting. Rule 4
  checks the quieting; it is also a good overload meter in its own right: on the 11-23 sets the
  quieting of an overloaded station got 10-30 dB better as the gain came down, while on a station
  that is not overloaded it hardly moved (0-3 dB per 6 dB of gain).
- **`gainkeeperreplay.sh`** replays the current GainKeeper next to build 40's, and with
  `OLD41A=<a GainKeeper.kt>` a third one (e.g. build 41 as first written). `TRAJ=1` prints every
  gain change of the first replay (when, from, to, the peak and MER that second). The analog's average
  quieting is a column now. Scenarios used: the search's pick; stuck at 40.2 without a sweep;
  a sweep at 28.0; KGGI 7 PM at gains with no sync; the emulated blind overloads (`scaleiq.py`,
  peaks -9 ... -12); KPCC indoors (sync marginal at every gain - its numbers swing with timing,
  don't read much into them).
- **`gainkeepertest.kt` 13-21**: 13 just under full scale (KGGI at 28.0); 14-19 rule 4 (finds
  sync; gives up by 26 s and goes back; the quieting check stops it; Digital only; never after a
  fade, never in Analog only, never without a sweep; a clip during the walk ends it); 20 = 300,000
  random seconds where tunes often start out of sync: rule 4 only ever acts 8-26 s into a tune
  that has not synced, and every other tune answers exactly as build 40; 21 the loud-but-not-
  clipping patience (AM: one step at once, as before). Tests 11, 12 and 20 keep their random
  peaks out of -1.5 ... -0.5 dBFS (test 21's band), so their answer-for-answer checks still hold.

## The AM auto-gain (build 42): amgaintest, GAINFILES on AM, gainkeepertest 22-27

Until build 41 AM used the peak rule alone (the highest gain whose peak stays under -4 dBFS). On a V4
that went wrong both ways (XEKAM 950 and KBRT 740 recordings of 11-24 ... 11-27): past ~30 dB the AM
input compresses and the peak hardly moves, so on 950 the search climbed to 49.6 dB (HD 5 dB worse,
no audio); on 740 the V4's own 28.8 MHz oscillator leaks in at -740 kHz, is the loudest thing in the
window, and stopped the search at 25.4 (3 dB below KBRT's best). Build 42 measures the AM HD itself:
`gainscore::measureAm()` (primary sidebands 10.5-14.5 kHz over the noise at 16.5-19 kHz, median of 3
pieces of a 55 ms burst) and `doAutoGainAm()` in native-lib.cpp (a sweep 25.4 / 29.7 / 33.8 / 38.6 /
43.4 / 49.6, the lowest gain within 0.5 dB of the best; no HD = the gain nearest 33.8; the peak only
as a clip guard at -0.5 dBFS). GainKeeper's `am` flag: rule 1 only for a real clip, rule 2 never above
the tune's gain.

    build/amgaintest file.cu8 [...]      # the AM HD figure over a recording, one burst every 0.25 s (AMEVERY=1.0)

It agrees with the field test's numpy figures within ~0.3 dB on every file (950 at 49.6: 13.8; 32.8 ...
38.6: 18.5-18.8; 740 at 25.4 / 28.0 / 33.8 / 38.6: 14.2 / 16.2 / 17.4 / 16.8; 640 KFI: 0.0). Every
other carrier in those recordings (30-odd stations, analog only): 0 +- 2 dB median, so HD = 6 dB.

Searches - `GAINFILES` with one recording per gain, 16 tunes 0.5 s apart (`GAINSTART`), Digital, 1 s:

| set (gains recorded) | build 41 | build 42 |
|---|---|---|
| 950 11-25 10:44 (32.8 33.8 36.4 38.6 49.6), `GAINSTEPS=files` | 49.6 x16 | 32.8 x13, 38.6 x3 |
| 950 11-25 08:12 (29.7 49.6), files | 49.6 x16 | 29.7 x16 |
| 950 11-27 (32.8 33.8 36.4 37.2), files | 37.2 x16 (the top) | 32.8 x3, 37.2 x13 |
| 740 11-27 (25.4 28.0 33.8 38.6), files | 25.4 x16 | 33.8 x9, 38.6 x6, 25.4 x1 |
| 740 11-27, V4 steps | 25.4 x16 | 29.7 (= the 28.0 file) x6, 33.8 x8, 38.6 / 43.4 x2 |
| 740 11-21 Irvine (25.4 28.0 29.7), files | 25.4 x12, 28.0 x3, 29.7 x1 | 29.7 x13, 25.4 x3 |
| 640 KFI (22.9 28.0), V4 steps | 25.4 x16 | no HD: 33.8 x14, 29.7 x2 (33.8 = the 28.0 file, which clips now and then) |
| 950 10:44 set, `GAINHINT=496` | remembered 49.6 x16 | never 49.6 (AM doesn't use a remembered gain) |

With the V4's 29 steps the gains nobody recorded replay the nearest recording (25.4 on 950 = the 32.8
file, 43.4 / 49.6 on 740 = the 38.6 file), so those picks say little. The recordings of one set were
made minutes apart and the 740 ones swing 3-4 dB within a second: a replayed sweep compares gains at
DIFFERENT moments, a real one within half a second - most of the scatter above is that. A Monte Carlo
of the rule with one burst's real scatter (+-0.45 dB) and simultaneous bursts: 0.08-0.25 dB below the
best on average; a second look at the close call would gain ~0.05 dB - not done.

End to end (950 10:44 set, Digital, 25 s): build 41 49.6 dB, sync lost, BER 11.2 %, no audio; build 42
32.8 dB, BER 6.5 %, audio and "XEKA". `DONGLE=V3`: no gain call at all, as before.

**Build 42b - rtl_tcp: late samples.** On the phone (V4 over rtl_tcp, Wi-Fi, 740) build 42 said "gain
has no effect" on most tunes: it compared 0.0 and 25.4 dB with two bursts back to back, and with a
backlog in the server's / network's buffers both bursts still carried the gain from before (each
burst waits only 0.12 s, `BURST_SETTLE_S`). `rtltcp_replay.py` has two new options to reproduce it:
`--gain-effect T` (the commanded gain scales the samples against T tenths, through the 8 bits) and
`--lag S` (a gain command shows S seconds of samples later). Build 42 with `--live --tuner 6
--gain-effect 254 --lag 0.5` on the 740 25.4 file: "no effect", as on the phone. Build 42b: one
reference burst, then ONE gain change at a time (0.0 dB, then the sweep's first gain - only a move UP
counts there, a move down is the first change arriving late), each watched for up to 6 bursts; the
burst the peak moved in = how late the samples are, and every sweep burst reads that many more first:

| lag | build 42 | build 42b | 25.4 / 29.7 / 33.8 measured as | time |
|---|---|---|---|---|
| 0 | gain works | gain works | -6.6 / -2.1 / clips | 0.8 s |
| 0.3 s | "no effect" | works, 3 bursts late | -6.4 / -2.1 / clips | 2.7 s |
| 0.5 s | "no effect" | works, 4 bursts late | -6.1 / -2.4 / clips | 3.4 s |
| 1.0 s | "no effect" | works (0.0 dB "only now"), 7 late | -6.5 / -2.0 / clips | 6.6 s |
| 1.5 s | "no effect" | works, 10 late | -6.3 / -1.9 / clips | 8.7 s |
| gain ignored (direct sampling) | no effect, 0.3 s | no effect, 13 bursts | | 2.2 s |

(Without the late reads a lagging sweep judges each gain by the one before it - 0.5 s lag: "25.4 dB"
measured -30 dBFS, the 0.0 dB samples.) USB searches: unchanged, tune for tune. The FM sweep over
rtl_tcp has the same blind spot (each burst waits 0.12 s) - not changed here.
`regress.sh` (all at manual gain or FM): build 41 and build 42 identical, but for the port number.

`gainkeepertest_38.sh` now also compiles build 41's GainKeeper (f3c7914, "GainKeeper41"): 60 checks,
22-25 the AM rules on made-up 950 / 740 signals, 26 = without `am` every answer is build 41's (300,000
random seconds, FM and AM, sweeps, tries, rule 4), 27 = with `am` never above the tune's gain and never
down without a clip.

## A favorite's program (build 39b): programwishtest.kt

`ProgramWish.kt` is the rule `RadioService` follows when a favorite asks for a program (HD1 at
once; a sub-channel at once while no HD has been decoded, else when it is on the air). Plain
Kotlin, tested alone (needs `kotlinc`):

    kotlinc ../../app/src/main/java/io/github/derek20la/hidefradio/ProgramWish.kt programwishtest.kt -include-runtime -d build/pw.jar && java -jar build/pw.jar

What the ENGINE does with those switches is an `apptest` run with `t:prog:N` events on a
recording without HD - silence on the sub-channel, the analog back on HD1 (`blend sub` /
`blend analog` in the `PRINT=1` lines):

    IQFILE=903.cu8 IQSTEP=1 PRINT=1 WAVOUT=out.wav build/apptest 90300000 2 9 0.01:prog:3 4:prog:0 7:prog:3
