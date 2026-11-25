/*
 * HiDef Radio: hand-written config.h for nrsc5 (library only).
 *
 * nrsc5's own build (CMake) generates this file from src/config.h.in by
 * testing the compiler. We write it by hand for Android (NDK clang + bionic),
 * the same way libusb's android/config.h replaces its ./configure step.
 */
#pragma once

/* Decode HD Radio audio with FAAD2 (our faad_hdc library). */
#define USE_FAAD2

/* Android's C library (bionic) already has strndup(). */
#define HAVE_STRNDUP

/* CMPLXF(re, im) builds a "float complex" number. nrsc5's config.h.in only
 * recognises GCC's way of doing this, so provide it for clang if the system
 * <complex.h> doesn't already. */
#include <complex.h>
#ifndef CMPLXF
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#endif

/* nrsc5's internal messages go to stderr, which Android throws away.
 * 5 = print nothing (same as nrsc5's default). */
#define LIBRARY_DEBUG_LEVEL 5

/* nrsc5_get_version() reports this. nrsc5's CMake reads it from git;
 * we pin the release we downloaded. */
#define GIT_COMMIT_HASH "v3.2.0"
