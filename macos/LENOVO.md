# Brief for Claude on the LENOVO (Windows 11, Android Studio)

You are helping with the macOS port of HiDef Radio (an Android HD Radio app). The plan is in
`macos/PLAN.md`, the progress in `macos/STATUS.md`. The work is done in another Claude chat on
the user's **MacBook**. That chat is the only one that edits code, commits and pushes.

**Your job on this machine:** the Android phone test for **step 0** on branch `macos-port`.
Step 0 moved the engine (`native-lib.cpp`, the `.hpp` headers, `nrsc5-config/`, `external/`) from
`app/src/main/cpp/` into `core/` at the top of the repo. Only paths changed. The Android app must
build and behave **exactly** as before.

## Rules
- Stay inside this project folder, plus Android Studio and the Android SDK.
- Don't edit any tracked file, don't commit, don't push, don't switch branches except as below.
- Ask the user before deleting anything. Show them what would be deleted first.
- The user is learning git: one step at a time, and say what each command does.
- Android Studio's Terminal tab sometimes pops up "IDE internal errors" bubbles while typing. That
  is an IDE bug, harmless.

## Steps
1. `git status` should say "On branch macos-port". Then `git pull` to fetch the latest
   (fast-forward only; stop if it says anything else).
2. **Leftover files in `app/src/main/cpp/external`.** They are untracked build leftovers that were
   made on this Lenovo. The libraries' own `.gitignore` files used to hide them, and those moved to
   `core/external`. The build no longer reads this folder.
   - `git clean -n -d app/src/main/cpp/external` lists them (deletes nothing). Show the user.
   - If they are only build leftovers and the user agrees: `git clean -f -d app/src/main/cpp/external`.
   - If any of them look like real source files, stop and tell the user to ask the MacBook chat.
3. **Clean build.** `app/.cxx` and `app/build` are generated folders that still hold the old CMake
   setup. After the user agrees, delete both. Then `.\gradlew.bat clean assembleDebug`.
   If Gradle can't find Java, use Android Studio's own: set `JAVA_HOME` to
   `C:\Program Files\Android\Android Studio\jbr` for that command.
4. **Check the native build used `core/`.** Find `compile_commands.json` under `app\.cxx\`.
   - Its paths should point at `core\src\native-lib.cpp` and `core\external\...`.
   - None should point at `app\src\main\cpp\external`.
   - Then report any compiler warnings or errors from the build.
5. **Install on the phone.** The user plugs in the phone (USB debugging on), then
   `.\gradlew.bat installDebug`. The debug build is a separate app ("-dev", id ending `.debug`),
   so the user's normal install and its settings are not touched.
6. **The user tests on the phone:**
   - an FM HD station syncs, and the blend goes from analog to HD;
   - an AM station plays;
   - Settings > About > Open-source licenses opens, and its text says "under core/external".
7. **Report.** Write a short summary the user can paste into the MacBook chat: the build result,
   the warnings, the `core/` path check, the install, and each phone check passed or failed.
8. Leave this machine on `macos-port` unless the user asks for `main` (`git switch main`).
