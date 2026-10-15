# How the blend works

*A walk through HiDef Radio's analog ↔ HD blend, for radio hobbyists who want to know what happens between the antenna and the speaker. No maths beyond "multiply and add" is needed; the code is linked at every step.*

A car radio with HD Radio does something clever. It starts playing the ordinary analog FM the moment you tune, switches to the digital (HD) sound a few seconds later without a skip, and when the HD drops out under a bridge it falls back to the analog so smoothly that most people never notice. HiDef Radio does the same with an RTL-SDR dongle and a phone. This page explains how.

## Contents

1. [The station's side: one programme, sent twice](#1-the-stations-side-one-programme-sent-twice)
2. [One stream of samples, two decoders](#2-one-stream-of-samples-two-decoders)
3. [Measuring the delay](#3-measuring-the-delay)
4. [Two clocks](#4-two-clocks)
5. [Matching the loudness](#5-matching-the-loudness)
6. [The crossfade](#6-the-crossfade)
7. [The radio that can see the future](#7-the-radio-that-can-see-the-future)
8. [When the two sides don't match](#8-when-the-two-sides-dont-match)
9. [The stereo blend, a different thing](#9-the-stereo-blend-a-different-thing)
10. [Try it yourself](#10-try-it-yourself)
11. [Words used on this page](#11-words-used-on-this-page)

---

## 1. The station's side: one programme, sent twice

A hybrid HD Radio FM station sends its main programme twice at the same time:

- as **analog FM** in the middle of the channel (±100 kHz), exactly as stations have since the 1940s, and
- as **digital audio (HD1)** in two sidebands just outside it (from about 129 kHz to 198 kHz either side of the carrier in the common "MP1" mode).

The digital signal is built to survive fading: each bit of audio is spread out over time (the *interleaver*) and protected by error-correcting codes. The price is delay. A receiver has to collect a few seconds of signal before it can give out any audio.

So the station **delays its analog audio** by the same amount, the *diversity delay*. In a car radio both decoders then come out at the same moment of the song, and switching between them is seamless. On most stations around Los Angeles that delay is close to 2.5 seconds.

## 2. One stream of samples, two decoders

The dongle delivers raw radio, 1,488,375 complex samples ("IQ") per second, centred on the station. HiDef Radio hands **the same samples** to two decoders:

```
                    RTL-SDR, 1,488,375 IQ samples/s
                                 |
                +----------------+----------------+
                |                                 |
           libnrsc5                         fmdemod.hpp
     (the HD decoder, unmodified)        (our analog FM receiver)
                |                                 |
           HD1 audio                        analog audio
                |                          drift.hpp (clock)
                |                                 |
                +------> aligner.hpp <------------+
                |      (delay and loudness)       |
                +---------> blend.hpp <-----------+
                                 |
                              speaker
```

- **[`fmdemod.hpp`](../app/src/main/cpp/fmdemod.hpp)** filters out the analog part of the channel (and with it the HD carriers, which would otherwise sound like a loud hiss), demodulates the FM, decodes stereo and measures how clean the signal is. Its first audio comes within a second of tuning.
- **libnrsc5** ([nrsc5 on GitHub](https://github.com/theori-io/nrsc5), used unmodified) decodes the HD Radio signal. It needs about 0.5 s to find the digital signal and another 2 to 3 seconds before the first audio.

Here is the first surprise: nrsc5 hands out its audio *sooner* than a car radio's chip does. In this app the HD audio for any moment of the song arrives **about 2.5 seconds before** the analog audio of the same moment. The station's diversity delay was meant to make up for a slow decoder; with a fast one it turns into a head start for the HD.

That head start is what the rest of this page is built on.

## 3. Measuring the delay

To switch without a skip the app must know the delay to within a fraction of a millisecond. It can't simply assume 2.5 s:

| Station | Measured delay (analog behind HD) |
|---|---|
| KRTH 101.1, KBIG 104.3, KKGO 105.1, KYSR 98.7, KLLI 93.9 | 2484–2486 ms |
| KLOS 95.5 | 2416–2485 ms, different from one day to the next |
| KKLQ 100.3 | 2457–2468 ms |
| KDAY 93.5 | 2886–2912 ms |

It is each station's own setting, and a few drift. So **[`aligner.hpp`](../app/src/main/cpp/aligner.hpp)** measures it on every tune, from the audio itself.

**The idea.** Take the last 3 seconds of analog audio as a *template*. Slide it along the HD audio of the last several seconds. At every position compute how alike the two are, as a *correlation coefficient*: 1.0 means the same shape, 0 means unrelated. Where they are most alike, that's the delay.

**Done efficiently.** Sliding sample by sample over seven seconds of possible delay would be slow, so it is done in two passes:

1. **Coarse.** Both streams are boiled down to mono at 2,756 samples per second (the shape of the music survives, the detail doesn't need to), and the whole search range (the analog between 1 s ahead and 6 s behind the HD) is covered at once with an FFT. Multiplying two spectra and transforming back gives the correlation at every delay in three transforms.
2. **Fine.** At 11,025 samples per second, each delay near the coarse peak is tried directly, and a parabola through the best three points finds the peak to a fraction of a sample. The result is better than one 44.1 kHz audio frame (23 µs).

Once a delay is known, later measurements search only a narrow window around it (−0.25 / +0.35 s).

**Believing a measurement.** A correlation below 0.5 is "no match": two unrelated programmes give 0 to 0.3, the same programme 0.9 or more, and a station whose HD is processed very differently from its analog around 0.5 to 0.6. The aligner keeps the last nine accepted results and reports their **median**, so one odd result can't move it. It says "ok" once two results in a row agree to within 3 frames. It measures again one second after the first result (to confirm it quickly), then every 5 seconds.

**Anchoring the HD audio in time.** The analog audio's timing is exact: audio frame *n* came from IQ sample *n* × 33.75 (1,488,375 / 44,100). The HD audio comes from nrsc5 in pieces of 2,048 frames, and the app only knows which block of IQ samples it was feeding nrsc5 when a piece came out. That pins each piece down to within one block (about 88 ms). The aligner narrows it to a few milliseconds by watching how successive pieces fall into successive blocks (`anchorCorrection()`), and the correlation takes care of the rest.

## 4. Two clocks

The second surprise, found by the app's first outside tester: one stream of samples does **not** mean one clock.

- The **HD audio** follows the *station's* clock. nrsc5 tracks the timing of the digital signal, and the result is exactly 44,100 frames per second of the station's time.
- The **analog audio** follows the *dongle's* clock: one frame per 33.75 IQ samples, and the IQ samples are paced by the dongle's crystal.

A crystal that is 57 parts per million fast (normal for an older dongle without a temperature-compensated crystal) makes the analog gain 2.5 frames on the HD every second. Two delay measurements 5 seconds apart then differ by 13 frames, they never agree, and the blend never starts. On an RTL-SDR Blog V3 or V4 (1 ppm) the effect is 25 times smaller, which is why it went unnoticed at first.

**[`drift.hpp`](../app/src/main/cpp/drift.hpp)** fixes it in two parts:

- **A resampler** (`drift::Stretch`) stretches or squeezes the analog audio by those few millionths, so it keeps the station's time. Its error is about −99 dB, and at zero correction it copies the audio bit for bit.
- **A tracker** (`drift::Tracker`) works out how much to stretch. It has two witnesses to the crystal's error:
  - the **tuning error**: one crystal sets both the tuner's frequency and the sample clock, so a station that appears 5.4 kHz low at 94.5 MHz means a crystal 57 ppm fast. Known two seconds after tuning, accurate to 1 to 3 ppm.
  - the **audio**: a straight line through the aligner's measurements over time; its slope is the error. Ten times more precise, but only after half a minute of measurements that agree.

  The tuning error is the referee. The audio's figure counts only once at least four measurements lie on a tight line, and it may not move more than 8 ppm away from the tuning error unless the audio makes a strong case three times running. (Three measurements always lie on *some* line. Before this rule, one bad measurement on a station with poorly matching audio once sent the correction to "+103 ppm".)

The HD audio is never touched.

## 5. Matching the loudness

The aligner also compares the energy of the two streams over the matched stretch, in 10 ms pieces. Pieces where nrsc5 filled in silence (a dropout) are left out, or the analog would look louder than it is. KLOS's analog measured 4 dB louder than its HD.

Before that, the app applies the station's own **digital audio gain**: every HD programme carries a small field (−8 to +7 dB) telling receivers how much to raise or lower the digital audio so that it matches the analog in loudness. nrsc5 reports it but doesn't apply it; `fixHdAudio()` in `native-lib.cpp` does. With it, KLOS's difference went from +4.1 dB to +0.1 dB.

Whatever difference is left, the blend corrects on the **analog** side (at most ±12 dB, changing by no more than 2 dB per second), so the HD sounds exactly as it does in "Digital only".

## 6. The crossfade

**[`blend.hpp`](../app/src/main/cpp/blend.hpp)** keeps both streams in ring buffers on one time line, counted in audio frames since the tune:

- the analog audio, about 4 seconds of it, and
- the HD audio, 6 seconds, with one flag per frame: **good**, or **bad** (a 2,048-frame piece that is all zeros is a dropout nrsc5 filled in), or **not received**.

The play position walks along the analog time line. The HD frame that carries the same sound is the play position minus the measured offset.

The blend is a small state machine:

```
   ANALOG ──(aligned, next 2 s of HD clean)──► TO_HD ──(0.5 s)──► HD
     ▲                                                             │
     └──────────(0.5 s)◄── TO_ANALOG ◄──(dropout ahead / re-align)─┘
```

The output is `(1 − g) × analog + g × HD`, separately for left and right, where `g` goes from 0 to 1 over half a second along a **raised cosine** (half a cosine wave). It starts and ends gently, so there's no audible corner. Because the two streams are lined up to a fraction of a millisecond, the crossfade is just a change of sound quality: no echo, no skip, no jump in volume.

After a tune the sequence is typically: analog at once (first sound in under a second) → HD synced after ~0.5 s → first HD audio after ~3 s → first delay measurement → confirmed one second later → crossfade to HD, about **10 seconds** after tuning.

## 7. The radio that can see the future

Now the head start pays off.

```
     time line ->                     now (what you hear)
     analog   .............................|............
     HD       ................|.........................XXXX......
                              |<-- 2.5 s ->|
                         same sound as "now"            ^ a dropout,
                                                          seen 2.5 s before it is heard
```

At every moment the app holds 2.5 seconds of HD audio *from the future*. A car radio learns about a dropout when it happens and must cover it in a hurry. Here every dropout is visible long before it plays.

So on every audio buffer (about every 46 ms) the blend asks: **is anything wrong in the next 0.65 s of HD?** That is the 0.5 s fade plus a 0.15 s guard. If so, it starts fading to the analog now, and the fade is finished before the gap arrives. You hear analog for a while instead of a gap.

Coming back is just as careful: the HD must be clean for the **next 2 seconds** before the blend fades back in. If the problem ahead clears up during the fade out (a piece arrived late), the fade simply reverses.

**Avoiding the flapping.** A signal on the edge (a weak station, driving through hills) could bounce between HD and analog every few seconds, which is more annoying than staying on analog. So after each fade back to analog the blend waits before trying again: 0 s the first time, then 3, 8 and 15 s if it keeps happening. Ten seconds of steady HD resets the count. These figures were tuned on 20 recordings with the antenna moved every 10 seconds or so.

**Re-aligning.** After a sync loss nrsc5 sometimes comes back exactly one 2,048-frame piece later than before. The aligner notices the jump. The blend then fades out to analog, takes the new figure, and fades back in, instead of jumping in the middle of the song.

## 8. When the two sides don't match

Sometimes the aligner never finds a match. There are three reasons, and the app tells them apart:

1. **The same station, processed very differently.** Some stations run their HD and analog through different audio processing, and the correlation never reaches 0.5. If RDS (the analog's data) and the HD both name the same call letters, the app knows it is one station and plays the HD anyway, without time alignment. After 20 seconds without a match it does so with a one-time jump of about 2.5 s, 0.3 s behind live.
2. **The HD belongs to another station.** A DXer meets this: a distant station's HD and a local station's analog on one frequency. The setting *"If the HD signal is another station's"* decides: play that HD anyway, or stay on the analog.
3. **Two stations taking turns.** With a distant station fading in and out over a local one, the analog you hear can switch between two stations from second to second (on 91.1 west of Los Angeles, KDSC's HD with the analog of XETRA-FM, "91X"). Falling back on "the analog" in every HD dropout would then swap between two programmes several times a minute. **[`stationid.hpp`](../app/src/main/cpp/stationid.hpp)** collects the evidence: which RDS code belongs to the HD station (its call letters, or the audio matching closely while that code was on the air), and whether a *different* code is coming in now. It says "another station" only when the audio agrees, meaning three attempts in a row that found no likeness although the HD audio was really there. From then on the blend plays **the HD alone**, like an HD2 sub-channel. In a dropout you hear silence, never the other station's programme. After 10 seconds of silence (you drove out of range) the other station's analog is given back. A line on the screen offers a tap to listen to the other station.

Because a "no match" can also mean the HD audio simply wasn't there yet, the aligner reports how much of the searched HD stretch held real audio (`hdCover`), and only attempts with real audio count as evidence.

## 9. The stereo blend, a different thing

There is a second, older kind of blend inside `fmdemod.hpp`: **stereo → mono** on a noisy analog signal, the way car radios do it.

FM stereo is noisier than mono. The left-minus-right signal rides on a 38 kHz subcarrier, up where the receiver's noise is strongest, so a weak station that sounds fine in mono hisses in stereo. The app measures the noise in the part of the FM signal where a station sends nothing (60–100 kHz, the *quieting*, which is also the 5-bar signal meter) and narrows the stereo gradually: full stereo above 36 dB of quieting, mono below 24 dB, a straight line in between. It moves toward mono quickly (0.05 s) and back to stereo slowly (4 s), the same shape as the Silicon Labs Si468x chips found in many car radios.

A finding from this project: on the RTL-SDR the noise after the FM discriminator is almost **flat** from 16 to 100 kHz (the 8-bit converter sets it), where textbooks show it rising with frequency. Stereo therefore costs only about 5 dB of extra hiss instead of the classic 20 dB, so the stereo blend matters less than in a car. A listening test on a weak station still preferred it.

## 10. Try it yourself

Everything on this page can be watched on a PC, with no phone and no dongle. [`tools/harness`](../tools/harness/README.md) runs the app's whole native engine against an IQ recording:

```sh
cd tools/harness
sh build.sh                 # Linux / macOS
# record a station on any computer with nrsc5:  nrsc5 -w klos.cu8 95.5 0
IQFILE=klos.cu8 IQPACE=1 WAVOUT=out.wav PRINT=1 build/apptest 95500000 2 40
```

`95500000` is the frequency in Hz, `2` means Auto (the blend; `0` = Digital only, `1` = Analog only), `40` is the number of seconds. With `PRINT=1` it prints the engine's state once a second: the delay measurements and their correlation, the clock correction in ppm, the blend's state and the reason for every fade. `out.wav` is what the phone would have played. Some tools that come with it:

- `mkhdgap` removes the HD from a recording for chosen seconds, to watch the look-ahead fade around the gap.
- `mkdrift` turns a recording into what a dongle with a cheap crystal would have made.
- `blendtest` and `drifttest` run the blend and the clock correction through made-up audio, including hours of it in seconds.

All the headers named here (`fmdemod.hpp`, `rds.hpp`, `drift.hpp`, `aligner.hpp`, `blend.hpp`, `stationid.hpp`, `hdsearch.hpp`, `vec4.hpp`) are plain C++17 with no Android code in them. Feel free to lift them into your own nrsc5 project (GPL-3.0-or-later).

## 11. Words used on this page

- **IQ samples**: the dongle's raw output, pairs of numbers (in-phase and quadrature) describing the radio signal around the tuned frequency. "cu8" = each number one unsigned byte.
- **MPX** (multiplex, composite): what comes out of an FM discriminator before any audio filtering. Mono audio up to 15 kHz, the 19 kHz stereo pilot, the stereo difference signal around 38 kHz, RDS at 57 kHz.
- **Quieting**: how far the noise sits below full modulation. A strong station "quiets" the receiver; the app's signal meter is built on it.
- **Diversity delay**: the delay a station puts on its analog audio so that it lines up with the digital audio in a receiver.
- **Correlation coefficient**: how alike two signals are in shape, from −1 to 1, independent of how loud they are.
- **ppm** (parts per million): 1 ppm of a 28.8 MHz crystal is 28.8 Hz; 57 ppm makes the analog audio gain 2.5 frames a second on the HD.
- **Frame**: one stereo audio sample, 1/44,100 of a second (23 µs).
- **MER / BER**: modulation error ratio and bit error rate of the HD signal, shown in the app's signal details. Higher MER and lower BER are better.
- **RDS / RBDS**: the small data signal at 57 kHz that carries a station's code (PI), its name, text and programme type. RBDS is the North American version.
