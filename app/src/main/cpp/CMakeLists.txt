# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("hidefradio")

# Always optimize native code, even in Debug builds.
# Real-time radio decoding is too slow without optimization.
# (The later -O2 overrides the Debug default of -O0.)
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O2")

# ---------------------------------------------------------------------------
# libusb (static library)
# Talks to USB devices. We compile its C files ourselves instead of running
# its Unix ./configure script; android/config.h replaces what configure makes.
# ---------------------------------------------------------------------------
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/libusb)

add_library(usb-1.0 STATIC
        # Core (works on every operating system)
        ${LIBUSB_DIR}/libusb/core.c
        ${LIBUSB_DIR}/libusb/descriptor.c
        ${LIBUSB_DIR}/libusb/hotplug.c
        ${LIBUSB_DIR}/libusb/io.c
        ${LIBUSB_DIR}/libusb/strerror.c
        ${LIBUSB_DIR}/libusb/sync.c
        # Linux/Android backend
        ${LIBUSB_DIR}/libusb/os/linux_usbfs.c
        ${LIBUSB_DIR}/libusb/os/events_posix.c
        ${LIBUSB_DIR}/libusb/os/threads_posix.c
        ${LIBUSB_DIR}/libusb/os/linux_netlink.c)

# PRIVATE = only needed while compiling libusb itself.
#   android/  -> where config.h lives
#   libusb/os -> backend headers (same as libusb's own Android build)
target_include_directories(usb-1.0 PRIVATE
        ${LIBUSB_DIR}/android
        ${LIBUSB_DIR}/libusb/os)

# PUBLIC = also given to anything that links libusb, so our code
# (and librtlsdr next step) can #include <libusb.h>.
target_include_directories(usb-1.0 PUBLIC
        ${LIBUSB_DIR}/libusb)

# libusb writes its log messages to Logcat, which needs Android's log library.
target_link_libraries(usb-1.0 PUBLIC log)

# ---------------------------------------------------------------------------
# librtlsdr (static library) - RTL-SDR Blog fork
# Knows how to talk to the RTL2832U chip and its tuner chips. Uses libusb.
# We only build the library part - not the command-line tools (rtl_fm etc.).
# ---------------------------------------------------------------------------
set(RTLSDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/rtl-sdr-blog)

add_library(rtlsdr STATIC
        ${RTLSDR_DIR}/src/librtlsdr.c       # main library (open, tune, read samples)
        ${RTLSDR_DIR}/src/tuner_e4k.c       # tuner chip drivers:
        ${RTLSDR_DIR}/src/tuner_fc0012.c
        ${RTLSDR_DIR}/src/tuner_fc0013.c
        ${RTLSDR_DIR}/src/tuner_fc2580.c
        ${RTLSDR_DIR}/src/tuner_r82xx.c)    # R820T / R828D (V3, V4, Nooelec)

# PUBLIC: rtl-sdr.h lives here, and our own code will #include it too.
target_include_directories(rtlsdr PUBLIC
        ${RTLSDR_DIR}/include)

# Tells rtl-sdr.h "this is a static library" (no DLL import/export markers).
target_compile_definitions(rtlsdr PUBLIC rtlsdr_STATIC)

# librtlsdr calls libusb, so it links our usb-1.0 library.
target_link_libraries(rtlsdr PUBLIC usb-1.0)

# ---------------------------------------------------------------------------
# FFTW 3.3.10 (static library, single precision = "fftw3f")
# Fast Fourier Transforms - nrsc5 uses these to pull the digital HD signal
# out of the raw radio samples. FFTW ships its own CMakeLists.txt, so we just
# set its options and include it with add_subdirectory().
# ---------------------------------------------------------------------------
# FFTW's options must be set as CACHE variables (with FORCE). FFTW's
# CMakeLists uses option(), and with its old cmake_minimum_required(3.0)
# option() would silently ignore plain set() variables.
set(ENABLE_FLOAT      ON  CACHE BOOL "FFTW: single precision (fftw3f)" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "FFTW: don't build test programs" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries"          FORCE)

# EXCLUDE_FROM_ALL = only build the parts something actually links to.
add_subdirectory(external/fftw EXCLUDE_FROM_ALL)

# fftw3.h lives in fftw/api - make it visible to whoever links fftw3f (nrsc5).
# $<BUILD_INTERFACE:...> means "use this path while building". It's needed
# because FFTW's CMakeLists also sets up "install/export" rules, and CMake
# refuses to export a path that points inside the source folder.
target_include_directories(fftw3f PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/fftw/api>)

# ---------------------------------------------------------------------------
# FAAD2 2.11.2 + nrsc5's HDC patch (static library "faad_hdc")
# AAC audio decoder. HD Radio uses a special AAC variant ("HDC"); the patch
# (external/nrsc5/support/faad2-hdc-support.patch, already applied to these
# sources) adds a "faad_hdc" library that can decode it.
# ---------------------------------------------------------------------------
set(FAAD_BUILD_CLI OFF CACHE BOOL "FAAD2: don't build the faad command-line tool" FORCE)

# FAAD2 notices it is inside another project ("bundled mode") and then builds
# static libraries and skips install rules. EXCLUDE_FROM_ALL = only faad_hdc
# gets built (not faad, faad_drm, faad_fixed...), because only it is linked.
add_subdirectory(external/faad2 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# nrsc5 v3.2.0 (static library "nrsc5") - the HD Radio decoder itself
# We build only the library files (not main.c/log.c = the command-line tool).
# nrsc5's own build generates a config.h; we use our hand-written one in
# nrsc5-config/config.h instead (like libusb's android/config.h).
# ---------------------------------------------------------------------------
set(NRSC5_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/nrsc5)

add_library(nrsc5 STATIC
        ${NRSC5_DIR}/src/acquire.c       # find the HD signal
        ${NRSC5_DIR}/src/sync.c          # lock onto it
        ${NRSC5_DIR}/src/decode.c        # demodulate / de-interleave
        ${NRSC5_DIR}/src/frame.c         # split into audio & data frames
        ${NRSC5_DIR}/src/output.c        # audio (via FAAD2) + events
        ${NRSC5_DIR}/src/pids.c          # station name, etc.
        ${NRSC5_DIR}/src/here_images.c   # traffic/weather images
        ${NRSC5_DIR}/src/input.c         # takes our samples in
        ${NRSC5_DIR}/src/nrsc5.c         # the public nrsc5_... functions
        ${NRSC5_DIR}/src/rtltcp.c        # (network mode - not used, but nrsc5.c refers to it)
        ${NRSC5_DIR}/src/firdecim_q15.c  # filter
        ${NRSC5_DIR}/src/conv_dec.c      # error correction (Viterbi)
        ${NRSC5_DIR}/src/rs_init.c       # error correction (Reed-Solomon)
        ${NRSC5_DIR}/src/rs_decode.c
        ${NRSC5_DIR}/src/unicode.c       # text conversion
        ${NRSC5_DIR}/src/strndup.c)

# nrsc5 is written in C11 with GNU extensions (= -std=gnu11).
set_target_properties(nrsc5 PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_definitions(nrsc5 PRIVATE _GNU_SOURCE HAVE_FAAD2)

target_include_directories(nrsc5
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nrsc5-config   # our config.h
                ${NRSC5_DIR}/src
        PUBLIC  ${NRSC5_DIR}/include)                      # nrsc5.h (for our code)

# nrsc5 uses FAAD2 (audio), FFTW (math), librtlsdr (headers/API) and libm.
target_link_libraries(nrsc5 PUBLIC faad_hdc fftw3f rtlsdr m)

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp)

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        nrsc5       # nrsc5 HD Radio decoder (static) - Milestone 5
        rtlsdr      # librtlsdr (static) - which brings in usb-1.0
        usb-1.0     # libusb (static)
        fftw3f      # FFTW single precision (static) - Milestone 5
        faad_hdc    # FAAD2 with HD Radio (HDC) support (static) - Milestone 5
        android
        log)