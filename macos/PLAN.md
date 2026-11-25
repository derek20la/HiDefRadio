# HiDef Radio for macOS: the plan

Branch `macos-port`. Apple Silicon first, shipped ad-hoc signed (no paid Apple developer
account). Test dongle: RTL-SDR Blog V4.

## Goal

A macOS version of HiDef Radio, built so that future engine updates reach **both** apps
without copying code:

- **One copy of the engine** (DSP, FM/AM demodulators, blend, C-QUAM, RDS, clock correction,
  source layer) in a shared `core/` folder. Both the Android CMake and the Mac CMake build from it.
- **A small plain-C API** (`engine.h`) between the core and each app. Android's JNI becomes a
  thin wrapper over it, and Swift calls the same functions.
- **Thin app shells:**
  - Android = JNI + USB file descriptor + AudioTrack + Kotlin UI.
  - Mac = dongle by index + Core Audio + SwiftUI.
- **One pin per dependency.** The nrsc5 fork and the RTL-SDR Blog librtlsdr are each pinned in
  ONE place, so updating either means changing one pin.
- **The test harness builds and runs on the Mac.**

The Android app's behaviour does not change at any step. Every refactor step is proven in two
ways: the harness output is bit-identical, and a debug build is tested on the phone.

## What the code looks like today (findings)

- `native-lib.cpp` (the engine plus its JNI entry points) and the DSP headers compile on
  Apple clang 21 **unchanged**. Only three things are missing on a Mac:
  1. `<jni.h>`: there is no JDK on the Mac. The engine uses only 12 JNI functions
     (NewStringUTF, NewString, GetStringUTFChars, ReleaseStringUTFChars, GetArrayLength,
     NewByteArray, NewIntArray, NewLongArray, Set{Byte,Int,Long,Short}ArrayRegion).
  2. `<android/log.h>`.
  3. Two calls that exist only on Linux: `sched_getcpu()` (the speed-test core read-out) and
     `MSG_NOSIGNAL` (`iqsource_tcp.hpp`, line 317).
- The source layer (`iqsource.hpp`) is already the right boundary. `iqsource_usb.hpp` opens the
  dongle with `rtlsdr_open_fd(fd)`. The vendored librtlsdr also has the normal
  `rtlsdr_open(index)`.
- The vendored libusb has the macOS backend (`os/darwin_usb.c`, `Xcode/config.h`).
- Some of the radio's behaviour lives in Kotlin and must be done by every shell:
  - `measureAlignmentNative()` every 2 s. The blend needs it.
  - The audio pull loop (`AudioPlayer.kt`): 0.25 s of pre-buffer, then reads with a 200 ms
    timeout, paced by a blocking write.
  - The gain watchdog (`GainKeeper.kt`, every 1 s).
- The harness (`tools/harness`) runs `native-lib.cpp` with a fake JNIEnv. Today it needs a
  JDK and Linux include paths, so it does not build on this Mac yet.
- The Android app is built on another machine (this Mac has no Android Studio or SDK).

## Target layout

```
core/                          ONE copy of the engine, built by both apps
  CMakeLists.txt               (0b) libusb (Linux or macOS backend), librtlsdr, FFTW+NEON, FAAD2, nrsc5, hidefcore
  include/hidef/engine.h       (0b) the plain-C API
  src/engine.cpp               native-lib.cpp without the JNI (0: moved, 0b: JNI removed, renamed)
  src/*.hpp                    (0) fmdemod, amdemod (C-QUAM), blend, aligner, drift, rds, stationid,
                                   hdsearch, gainscore, psstitch, iqrecord, vec4, iqsource*.hpp
  nrsc5-config/config.h        (0)
  external/                    (0) nrsc5, rtl-sdr-blog, faad2, fftw, libusb: the only copies
  external/PINS.md             (0b) one line per library: upstream, branch, commit, our patches
app/src/main/cpp/
  CMakeLists.txt               Android shell: builds core/ + jni.cpp (Gradle path unchanged)
  jni.cpp                      (0b) thin JNI wrappers: Kotlin names and arguments unchanged
macos/                         (1) Mac shell: dongle by index, Core Audio, later SwiftUI
tools/harness/                 builds from core/; from 0b it calls engine.h directly (no JNI, no JDK)
```

## Step 0a: the harness on the Mac (only `tools/harness/` changes)

- A harness-only `jni.h` covering the 12 functions above, so no JDK is needed.
- A Mac compatibility header passed to the compiler (`sched_getcpu`, `MSG_NOSIGNAL`).
  Defining `MSG_NOSIGNAL` as 0 is fine here, for the harness only. The real fix for the Mac
  build comes in 0b (see there).
- `build.sh` made to work on macOS.
- **`tools/harness/regress.sh`**: runs a fixed set of tests and prints a checksum for every
  output.
  - **Recordings stay outside the repo.** `regress.sh` reads them from the folder given as
    `RECORDINGS=...`, by default `"$HOME/Downloads/1260 AM/IQ"` (the path has a space, so
    it is always quoted). Reading `~/Downloads` is allowed for these files only. `*.cu8` is
    added to `.gitignore` as a safety net.
  - **Baseline recordings:**

    | File | What it covers |
    |---|---|
    | `1260-3.cu8` | AM HD (KMZT), run with `UNMATCHED=hd` |
    | `950AM_XEKA_11-21_1118.cu8` | AM HD; analog lags HD by 961 ms |
    | `kklq.cu8` | FM HD and the blend aligner |
    | `HiDefRadio_2026-11-16_17-43-35_103.5MHz_1488375sps.cu8` | FM HD (KOST) |
    | `HiDefRadio_2026-10-26_10-39-53_90.3MHz_1488375sps.cu8` | FM (Z90) |

    There is no real C-QUAM recording, so `amtest` covers C-QUAM.
  - **Recording runs**, in deterministic mode (`IQSTEP=1`, where the same input gives the same
    numbers every run):
    - HD only and Auto/blend on the files above;
    - "Analog only" on one FM file (KOST, so the forced analog path is tested on a station
      that also has HD; Z90 already covers analog in Auto);
    - a program switch, mono, clock correction off and auto-gain;
    - an IQ recording;
    - the rtl_tcp path.
  - **Stand-alone tests:** blendtest, drifttest, amtest, identtest, pstest, gaintest and
    searchtest.
  - **Comparison:** WAV files must match byte for byte. Text logs are compared with the
    real-clock timing fields removed.
- Run it on today's `main` code (6dbcf17) to get the **baseline**. The same script then proves
  every later step, and future engine updates too.
- **The baseline records how it was built:** the compiler version (`clang --version`) and the
  exact harness compile flags are saved with the checksums. Every later step uses the same
  compiler and flags, so a "not identical" result can only come from our changes.

## Step 0: the move (files only, no behaviour change)

- `git mv` the headers, `nrsc5-config/` and `external/` into `core/`, and `native-lib.cpp`
  into `core/src/`. `git mv` keeps each file's history.
- Only paths change:
  - the Android `CMakeLists.txt`;
  - harness `build.sh` and `objcheck.sh`, and `lrcheck/run.sh`;
  - README, the docs links and the path in `NOTICES.txt`.
- No code changes, and `build.gradle.kts` stays as it is.
- **Proof:**
  1. `regress.sh` output is identical to the baseline.
  2. The Android CMake finds all its files.
  3. A debug build is tested on the phone: push the branch, pull it on the Android machine,
     clean build.

## Step 0b: the C API (Android behaviour unchanged)

- **The split:** `native-lib.cpp` becomes `core/src/engine.cpp`, with plain C functions
  declared in `engine.h` (for example `hidef_open_usb_fd`, `hidef_open_usb_index`,
  `hidef_open_tcp`, `hidef_start`, `hidef_stop`, `hidef_read_audio`, `hidef_get_signal`,
  `hidef_set_program`, `hidef_measure_alignment`). `app/.../jni.cpp` becomes one-line
  wrappers with the same Kotlin function names, so no Kotlin changes.
- **Small core changes, all behind `#ifdef`, so the code Android compiles stays the same:**
  - a logging header (Logcat on Android, stderr on the Mac);
  - `UsbSource` gets an open-by-index next to open-by-fd, sharing the same describe text;
  - `sched_getcpu` and `MSG_NOSIGNAL` are only used on Linux and Android.
  - On the Mac, the rtl_tcp socket gets `SO_NOSIGPIPE` (macOS has no `MSG_NOSIGNAL`).
    Without it, the program is killed by SIGPIPE when an rtl_tcp server drops the
    connection.
- **CMake:** one `core/CMakeLists.txt` that both apps include.
- **Pins:** `core/external/PINS.md` plus `tools/vendor.sh <lib> <commit>`, which downloads
  that commit into `core/external/<lib>` so `git diff` shows exactly what changed. Updating a
  library then means changing one pin and running one command.
  - **Before switching any library to `vendor.sh`,** check that today's vendored copy
    matches what is being pinned. If it doesn't, the difference is saved as a patch file
    (`core/external/patches/<lib>.patch`) and listed in `PINS.md`.
  - nrsc5: check against `derek20la/nrsc5@9dff25a` (branch `hidef`).
  - librtlsdr: fork `rtlsdrblog/rtl-sdr-blog` to `derek20la/rtl-sdr-blog`, branch `hidef`,
    carrying the `rtlsdr_open_fd` patch, so both libraries are pinned the same way
    (approved). The `gh` command is shown to you before it runs; it will be
    `gh repo fork rtlsdrblog/rtl-sdr-blog --clone=false`.
  - FFTW is pinned to the official **3.3.10 release tarball and its SHA-256**, not a git
    commit: FFTW's git repo doesn't include the generated codelets.
  - FAAD2 2.11.2 and libusb 1.0.30 are pinned to their releases.
- **Harness:** it switches to calling `engine.h` directly, exactly as the Mac app will.
- **Proof:** `regress.sh` output identical, then another phone test.

## Step 1: the Mac command-line radio

```
hidefradio --list                 # dongles found (index, name, serial)
hidefradio 95.5                   # FM, HD Radio Auto (blend), HD1
hidefradio 1260k                  # AM (the V4's upconverter is handled by the driver)
  -d 0   -p 2   -s auto|hd|analog   -g auto|29.7   --tcp host:1234   -v
```

- **While it plays:** type a number and Enter to switch program, `f 101.1` to retune, `q` or
  Ctrl-C to quit. Quitting stops the stream and closes the dongle properly.
- **Status line once a second:** station, program, HD sync, blend state, MER/BER and gain.
- **Threads:** the same rules as on Android.
  - The main thread plays Kotlin's "engine thread": every control call, plus the 2 s
    alignment timer.
  - **The AudioQueue callback never blocks.** A separate pull thread does what
    `AudioPlayer.kt` does (0.25 s of pre-buffer, then the blocking read with its 200 ms
    timeout) and fills a ring buffer. The callback only copies from the ring, and plays
    silence when the ring is empty. Core Audio converts the engine's 44.1 kHz to the Mac's
    output rate.
  - SIGPIPE is ignored in the CLI as a second guard (the socket already has `SO_NOSIGPIPE`
    from 0b).
- **Build:** `macos/CMakeLists.txt` includes `core/` and links everything statically
  (Frameworks: IOKit, CoreFoundation, Security, AudioToolbox). No Homebrew libraries are
  needed at run time; the program depends only on Homebrew's cmake to build.
- **Left out of step 1:** the live gain watchdog (the auto-gain search at each tune still
  runs), remembered gain per station, the FCC call-letter list, logos and album art,
  recording.
- **Testing:**
  1. It builds with no warnings from our code.
  2. `--list` shows the V4.
  3. A local HD station syncs, the blend goes analog → HD, and the audio sounds clean.
  4. An analog-only station and an AM station play.
  5. Ctrl-C exits cleanly and the dongle opens again straight away.

## Step 2: SwiftUI (later)

- A SwiftUI window based on the Android tablet two-pane layout, calling `engine.h` directly.
- The gain watchdog (`GainKeeper.kt`) moves into the core in its own step, proven with the
  existing `gainkeeperreplay` tests, so it isn't written twice (Kotlin and Swift).
- Later still: remembered gain, the FCC station list (`app/src/main/assets/stations`),
  logos and art.

## Decisions (were open questions)

1. **Recordings:** not copied into the repo. `regress.sh` reads them from `RECORDINGS=...`
   (see step 0a for the folder and the five files).
2. **`derek20la/rtl-sdr-blog`:** yes, fork it (branch `hidef`) in step 0b. The `gh` command
   is shown before it runs.
