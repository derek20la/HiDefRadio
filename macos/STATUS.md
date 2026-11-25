# macOS port: status

## Where we stopped (2026-11-24, end of the day)

- Done: 0a (harness on the Mac, baseline) and 0 (the move into `core/`, phone test passed).
- Next: step 0b (the C API), build 42.
- To decide first:
  1. Install Android's command-line build tools on the MacBook (Homebrew: a JDK, the SDK
     and the NDK), so phone builds no longer need the Lenovo. The commands are shown before
     they run. Releases stay on the Lenovo.
  2. The version-number proposal in `PLAN.md` ("Version numbers").

## Step 0: the move into core/ (done, 2026-11-24; phone test passed)

**What changed:**
- `git mv`: `native-lib.cpp` and the 15 headers -> `core/src/`, `nrsc5-config/` -> `core/`,
  `external/` -> `core/`. 3932 renames, 0 lines changed inside them, so git keeps each file's history.
  `app/src/main/cpp/` now holds only `CMakeLists.txt`, and `build.gradle.kts` is unchanged.
- Only paths changed in: the Android `CMakeLists.txt` (`HIDEF_CORE` = `core/`); harness `build.sh`,
  `objcheck.sh`, `regress.sh`, a comment in `drifttest.cpp`; `lrcheck/run.sh` and a comment in
  `demod_app.cpp`; the READMEs, the links in `docs/how-the-blend-works.md`, and `NOTICES.txt`
  (the About screen's licence text now says "under core/external").

**Proof on the Mac:**
1. `sh regress.sh check` after a clean harness build: IDENTICAL. The engine sources hash
   `2e3b6428d9b76210` is the same as the baseline's.
2. The Android CMake, configured on this Mac (host compiler, no NDK) before and after the move:
   all 844 compile commands are the same files with the same flags. Only the object file names
   in the build folder differ: CMake names objects after the full path for sources outside its
   own folder. (This check needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` with Homebrew's CMake 4.5,
   because FFTW asks for CMake 3.0. Android's CMake 3.22 doesn't need it.)

**Found during the phone test:** our rtl-sdr-blog copy is missing two upstream files,
`cmake/Modules/Version.cmake` and `cmake/rtlsdrConfig.cmake`. Its own `.gitignore` (`*.cmake`) kept them out when it was first added, so they
were only ever on the Lenovo. The Android build doesn't use them: all six librtlsdr `.c` files compile
from the committed files with the Android flags. In 0b, `vendor.sh` brings them back, and the pin check
looks for the same problem in the other libraries.

**Not run:** `objcheck.sh` (needs a JDK, g++ and `sha256sum`) and `lrcheck/run.sh` (clones
ngsoftfm from GitHub). Their paths were updated but not tested.

**Phone test (Lenovo, 2026-11-24): passed.**
- Clean build (`app/.cxx` and `app/build` deleted): `gradlew clean assembleDebug` OK in 3 min,
  no compiler warnings. Its `compile_commands.json`: 844 entries (843 in `core\external`, plus
  `core\src\native-lib.cpp`), and none under `app\src\main\cpp`.
- Debug app (1.4.1-dev, build 41) installed on the moto g stylus 5G (2024) next to the normal install.
- FM HD syncs and blends analog -> HD; AM plays; the licences screen says "under core/external".
- The two leftovers on the Lenovo (`cmake/Modules/Version.cmake`, `cmake/rtlsdrConfig.cmake`) were
  deleted. rtl-sdr's own CMakeLists needs `Version.cmake`, so 0b must restore both.

**Build number:** from now on, every step tested on the phone gets the next build number
(`versionCode` in `app/build.gradle.kts`), so test installs can be told apart. Step 0b = build 42.
A release from `main` in the meantime must skip past the branch's numbers.

## Step 0a: the harness on the Mac (done, 2026-11-24)

**What changed (only `tools/harness/`):**
- `jni.h`: the harness's own stand-in for the JDK's jni.h (the JNI types, the 12 functions).
  No JDK is needed any more, on macOS or Linux. The Android app still uses the NDK's jni.h.
- `maccompat.h`: on Apple only, `MSG_NOSIGNAL` = 0 and `sched_getcpu()` = -1 ("core unknown").
  Passed with `-include` to the apptest compile.
- `build.sh`: uses those two files instead of the JDK, and writes its flags to `build/flags.txt`.
  nrsc5 and FAAD2 compiled on the Mac with no changes.
- `regress.sh` and `regress-baseline.txt`: 16 recording runs, 7 stand-alone tests, one SHA-256
  per output, about 90 s. `sh regress.sh check` compares with the baseline.
- README: the build line (no JDK) and a `regress.sh` section.

**Proof:**
- The engine is `main` 6dbcf17's: `git diff 6dbcf17 -- app` is empty.
- Two runs back to back: all WAVs, the IQ recording and the 7 stand-alone tests were identical
  without any filter. The apptest logs differed only in real-clock fields, which are now blanked:
  "N ms after tuning", "auto-gain took N ms", the status line's load %, and the COST line's load %.
- With the baseline recorded, `sh regress.sh check` printed IDENTICAL.
- The XEKA alignment comes out at 960.9 ms (the plan says 961 ms).

**Not done / notes:**
- `objcheck.sh` still needs a JDK and `sha256sum`, so it doesn't run on the Mac yet. Its paths
  change in step 0 anyway.
- The baseline is for Apple clang 21.0.0 on arm64. Another compiler can give different numbers.

**What I needed from you:** nothing for 0a.
