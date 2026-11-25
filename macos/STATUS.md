# macOS port: status

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

**What I need from you:** nothing for 0a. Next is step 0 (the `git mv` into `core/`), which
needs a phone test at the end: push the branch, pull it on the Android machine, clean debug build.
