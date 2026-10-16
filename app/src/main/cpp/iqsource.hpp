// iqsource.hpp - where the radio's samples come FROM (build 17, the 1.1 "source split").
//
// THE IDEA
// Everything the app does - the HD decoder (nrsc5), the analog FM demodulator, RDS, the
// aligner, the blend, the clock correction - works on ONE stream of numbers: the raw
// samples of a 1.488 MHz wide slice of the radio band, 1,488,375 pairs a second, each pair
// two bytes (I and Q, 0..255, 127.5 = zero; the "cu8" format). None of that code cares
// where the bytes came from.
//
// Up to build 16 the place they came from - an RTL-SDR dongle on the phone's USB port,
// driven by librtlsdr - was woven right through native-lib.cpp: about thirty calls to the
// driver in five different places (opening the dongle, tuning, the auto-gain search, the
// streaming thread, the gain controls). This file draws a line between the two halves:
//
//     a SOURCE  ---- cu8 samples ---->  the RADIO (native-lib.cpp and the DSP headers)
//               <--- tune / gain -----
//
// iq::Source is the list of things the radio asks of a source. A source is anything that
// can do those things. Today there is one: iq::UsbSource (iqsource_usb.hpp), the dongle on
// the USB port, which does each of them with exactly the librtlsdr call the app always made.
// Later sources plug in beside it without touching the radio:
//   - rtl_tcp (planned for 1.1): the dongle sits on another computer and the samples come
//     over the home network. Its protocol has a command for every item below except two it
//     cannot answer (the dongle's USB names, and whether the driver went to direct sampling);
//     see "WHAT A SOURCE MAY NOT KNOW".
//   - a recording (.cu8 file) played back - the desktop test harness does this today one
//     level lower, by standing in for librtlsdr itself (tools/harness/rtl_stub.c).
//
// WHY AN INTERFACE AND NOT A SWITCH
// The radio holds one pointer, `Source *`, and calls through it. Picking USB or the network
// happens once, when the source is opened; after that no line of the radio says "if USB".
// A virtual call costs a few nanoseconds; the busiest one here (the sample callback) is not
// even virtual - the source calls the radio's function directly, once per block of 131,072
// samples (~88 ms), exactly as librtlsdr did.
//
// THE RULES EVERY SOURCE KEEPS
//   - Samples are cu8 at the rate the radio set with setSampleRate() (always 1,488,375 -
//     the only rate nrsc5 accepts), centred on the frequency it set with setFrequency().
//   - stream() runs on the radio's streaming thread and calls `fn` once per block until
//     cancel() is called (from another thread) or the source dies (USB unplugged, network
//     gone). Then it returns. The radio's onSamples() must be the only thing `fn` does.
//   - readBurst() is only used before stream() starts (the auto-gain's short measurements).
//   - The control calls (frequency, gain ...) come from one thread at a time: Kotlin's
//     "engine" thread (RadioService.engine). A source does not need locks of its own for
//     them. setGain() / gain() may arrive while stream() is running (the gain watchdog
//     moves the gain live); a source must allow that, as librtlsdr does.
//   - Deleting the source closes it. The radio stops the stream first.
//
// WHAT A SOURCE MAY NOT KNOW
// Two answers only the USB driver can give:
//   - directSampling(): an R820T dongle (RTL-SDR Blog V3) switches its tuner off below
//     24 MHz and feeds the antenna straight to its converter (build 4, decision 54). The
//     radio then skips every gain call. A source that cannot tell answers false (the gain
//     calls then reach a tuner that is switched off: harmless, they only do nothing).
//   - the dongle's own name (manufacturer / product / serial): describe() says what it can.
// And one it may only know roughly: the list of gain steps. librtlsdr has the exact list per
// tuner; rtl_tcp only sends the tuner type and the NUMBER of steps, so its source will use
// the known table for that tuner type.
//
// Plain C++17, no Android in it. Header only: native-lib.cpp includes it, nothing else to
// build (no CMake change).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iq {

// What the radio hands a source to receive the samples: (bytes, how many, ctx).
// Same shape as librtlsdr's rtlsdr_read_async_cb_t, so the USB source can pass it on as is.
using SampleFn = void (*)(unsigned char *buf, uint32_t len, void *ctx);

class Source {
public:
    virtual ~Source() = default;     // closes the source (the stream must be stopped first)

    // A short word for logs and the debug text: "USB", later "rtl_tcp".
    virtual const char *kind() const = 0;

    // What the source found when it was opened, several lines, shown under "Show debug
    // info" (for USB: the driver's "opened OK", the dongle's USB names, tuner, gain steps).
    virtual std::string describe() const = 0;

    // ---- tuning ----
    // Each returns what the driver returned: 0 (or more) = done, negative = failed.
    virtual int setSampleRate(uint32_t samplesPerSecond) = 0;
    virtual uint32_t sampleRate() = 0;                  // what it really runs at now
    virtual int setFrequency(uint32_t hz) = 0;          // the centre of the slice
    virtual uint32_t frequency() = 0;                   // what it is really tuned to now
    virtual void flush() = 0;                           // throw away samples already on their way
    virtual bool directSampling() = 0;                  // see "WHAT A SOURCE MAY NOT KNOW"

    // ---- gain (tenths of a dB: 496 = 49.6 dB) ----
    virtual std::vector<int> gainSteps() = 0;           // the tuner's steps, low to high; empty = unknown
    virtual int setGainMode(bool manual) = 0;           // false = the tuner's own AGC
    virtual int setGain(int tenths) = 0;                // the source picks the nearest step it has
    virtual int gain() = 0;                             // the gain set now

    // ---- samples ----
    // A short burst of samples, read and returned at once (the auto-gain's measurements;
    // only while not streaming). *got = bytes really read. Returns false if nothing came.
    virtual bool readBurst(uint8_t *buf, int len, int *got) = 0;
    // Feeds fn(buf, len, ctx) block after block until cancel(); then returns. The return
    // value is only for the log (librtlsdr's own code).
    virtual int stream(SampleFn fn, void *ctx) = 0;
    virtual void cancel() = 0;                          // from another thread: make stream() return
};

}  // namespace iq
