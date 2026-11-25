// Harness only (macOS port, step 0a): the two Linux-only things native-lib.cpp uses, so the
// harness builds on macOS. build.sh passes it to the apptest compile with -include; on Linux it
// does nothing. The real fix for the Mac app (SO_NOSIGPIPE on the rtl_tcp socket) comes in 0b.
#pragma once
#ifdef __APPLE__
#include <sys/socket.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0        // harness only: a dropped rtl_tcp connection would raise SIGPIPE
#endif
// The speed test's "which core am I on": -1 = unknown, which the engine already handles.
static inline int sched_getcpu(void) { return -1; }
#endif
