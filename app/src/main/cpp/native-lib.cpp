#include <jni.h>
#include <string>
#include <thread>          // std::thread - the background streaming thread
#include <atomic>          // std::atomic - numbers shared safely between threads
#include <cmath>           // sqrtf
#include <mutex>           // std::mutex - protects the decoder status (Milestone 6)
#include <vector>          // std::vector - gain list / sample buffer (auto-gain)
#include <condition_variable>  // lets the audio reader sleep until audio arrives (Milestone 7)
#include <chrono>          // timeouts
#include <algorithm>       // std::min
#include <cctype>          // std::isalnum, std::toupper (14b step 4)
#include <deque>           // std::deque - recent album art (9d step 3)
#include <android/log.h>   // __android_log_print -> Logcat

#include "rtl-sdr.h"       // librtlsdr (with our rtlsdr_open_fd patch)
#include "fmdemod.hpp"     // M11: our analog FM demodulator
#include "aligner.hpp"     // M11 (11c): measures the analog-vs-HD time offset and loudness
#include "blend.hpp"       // M11 (11d): the analog <-> HD blend ("HD Radio: Auto")
#include "drift.hpp"            // build 6: the dongle's clock vs the station's (clock correction)
#include "hdsearch.hpp"         // build 9: the HD search rests on a station without HD

// nrsc5.h is a C header without C++ guards, so tell the C++ compiler
// "these are C functions" (otherwise the linker can't find them).
extern "C" {
#include "nrsc5.h"         // the HD Radio decoder library
}

// Log to Logcat with the tag "HiDefRadio-native"
#define TAG "HiDefRadio-native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// The open dongle, or nullptr if none is open.
// Only one dongle at a time for now.
static rtlsdr_dev_t *g_dev = nullptr;

// ---- Streaming state (Milestone 4) ----
// HD Radio (nrsc5) needs exactly this sample rate.
static const uint32_t SAMPLE_RATE = 1488375;

static std::thread g_streamThread;               // runs rtlsdr_read_async()
static std::atomic<bool> g_streaming{false};     // true while samples are flowing
static std::atomic<long long> g_bytes{0};        // total bytes received
static std::atomic<float> g_level{0.0f};         // signal level of the latest block
static std::atomic<float> g_peakDbfs{-99.0f};    // peak-to-peak of latest block, dB below full scale
// M10c: the LOUDEST block since Kotlin's gain watchdog last asked
// (takePeakMaxNative). One block is only ~88 ms; the watchdog looks once a
// second and must not miss a short loud moment in between.
static std::atomic<float> g_peakMax{-99.0f};

static void stopStreaming();   // defined further down

// ---- Decoder state (Milestone 6) ----
static nrsc5_t *g_nrsc5 = nullptr;   // the nrsc5 decoder, fed by onSamples()

// Milestone 9b: FM or AM. Only FM for now (AM arrives in 9c), but the MER
// fix below needs to know, because nrsc5 swaps the sideband labels in FM only.
static int g_mode = NRSC5_MODE_FM;          // 9c: set per tune by startStreamNative (AM below AM_BELOW_HZ)
static const int AM_BELOW_HZ = 30000000;

// Milestone 9b: a station can carry up to 8 audio programs:
// program 0 = HD1 (the "MPS"), 1 = HD2 (SPS1) ... 7 = HD8 (SPS7).
static const int MAX_PROGRAMS = 8;

// Everything we know about ONE program.
struct ProgramInfo {
    bool onAir = false;         // true once nrsc5 has seen this program
    int type = -1;              // program type number (-1 = not known yet), e.g. Top 40
    std::string sigName;        // name from the station's SIG table, e.g. "MPS", "SPS1", or a real name
    std::string title, artist;  // now playing (from the ID3 tags)
    long long audioValues = 0;  // 16-bit audio values decoded for this program

    // 9d: audio bit rate, measured like the nrsc5 command-line program does:
    // count the compressed audio (HDC) packets and their bytes; every 32
    // packets, work out the kbps and start counting again.
    long long hdcBytes = 0;     // bytes in the packets counted so far
    int hdcPackets = 0;         // packets counted so far (0..31)
    float kbps = 0;             // latest result (0 = not measured yet)
    long long crcErrors = 0;    // damaged packets since the tune (each = a tiny dropout)
};

// 9d: a moment in time, for "how long since..." (steady = never jumps when
// the phone's clock is changed).
using Clock = std::chrono::steady_clock;

// 9d: milliseconds from `start` until now.
static long long msSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// 9d: sync-loss grace period (idea from the SDR# plugin). A weak station can
// lose sync for a split second and get it straight back; we keep SHOWING
// "synced" for this long after a loss, so the display doesn't flicker.
static const long long SYNC_GRACE_MS = 1500;

// What nrsc5 has told us so far. Written by the decoder callback (streaming
// thread), read by getStatusNative() (UI thread) -> protected by g_statusMutex.
struct DecoderStatus {
    bool synced = false;        // locked onto an HD signal right now?
    int syncCount = 0;          // how many times we gained sync
    int merCount = 0;           // M10c step 2: MER readings since tuning (one every ~1.5 s)
    float freqOffsetHz = 0;     // tuning error nrsc5 measured
    float merLower = 0, merUpper = 0;   // modulation error ratio (dB), higher = better
    float ber = -1;             // bit error rate, lower = better (-1 = none yet)
    // 9d: BER statistics since the tune (like nrsc5-dui): average, best, worst.
    double berSum = 0;
    int berCount = 0;
    float berMin = 0, berMax = 0;
    int psmi = 0;               // 9d: service mode number (e.g. 3 = MP3), 0 = not known yet
    long long syncMs = -1;      // 9d: ms from tune to the FIRST sync (-1 = not yet)
    long long audioMs = -1;     // 9d: ms from tune to the first decoded audio (-1 = not yet)
    Clock::time_point lostSyncAt;   // 9d: when sync was last lost (for the grace period)
    std::string stationName;    // e.g. "KIIS"
    std::string slogan;
    ProgramInfo programs[MAX_PROGRAMS];   // 9b: one entry per program (HD1..HD8)
};
static DecoderStatus g_status;
static std::mutex g_statusMutex;

// 9d: when the current tune started (set in startStreamNative), and the gain
// it chose.
// M10b: startStreamNative / setGainNative now run on Kotlin's "engine" thread
// while the screen reads these on the UI thread -> atomics (safe to share).
// The tune time is kept as a plain number (clock ticks), because a
// Clock::time_point can't be atomic.
static std::atomic<long long> g_tuneTicks{(long long)Clock::now().time_since_epoch().count()};
static std::atomic<int> g_gainTenths{-1};      // tuner gain in tenths of a dB (-1 = tuner AGC)
static std::atomic<bool> g_gainAuto{true};     // chosen by our auto-gain (true) or set by hand (false)
// 14c: DIRECT SAMPLING. Dongles with an R820T tuner (the RTL-SDR Blog V3 and its relatives)
// can't tune below about 24 MHz with the tuner. For AM the driver then switches the tuner
// off by itself and feeds the antenna straight into the dongle's converter chip
// (rtlsdr_set_center_freq in librtlsdr.c: "direct sampling", input Q - the one the V3 has
// wired to its antenna socket). The tuner is out of the signal path, so there is NO tuner
// gain to set: no auto-gain search, no watchdog, no manual gain. (The V4 never does this:
// its R828D tuner has a built-in upconverter for AM, and its gain works as usual.)
static std::atomic<bool> g_direct{false};
// M10c: how the auto-gain found its starting gain on this tune.
static std::atomic<bool> g_gainRemembered{false};   // true = the remembered gain was fine (no search)
static std::atomic<int> g_gainSearchMs{-1};         // how long the auto-gain took (-1 = manual gain)

// M10b: milliseconds since the current tune started.
static long long msSinceTune() {
    return msSince(Clock::time_point(Clock::duration(g_tuneTicks.load())));
}

// Milestone 9b: the program we PLAY (0 = HD1). All programs are decoded;
// only this one's audio goes into the ring buffer. Changed by setProgramNative().
static std::atomic<int> g_program{0};

// ---- Milestone 9d step 3: station logos and album art ----
// Stations send pictures as "LOT" files (complete files, sent piece by piece
// and repeated). nrsc5 tells us, for each file, which SIG service carries it
// and what the station says it is (decision 16):
//   NRSC5_MIME_STATION_LOGO  = the station / program logo
//   NRSC5_MIME_PRIMARY_IMAGE = album art (or an ad picture during breaks)
// The song info (ID3) can point at the picture that goes with it ("XHDR"):
//   param 0 + a LOT number = show that picture, param 1 = no picture (show the logo).
// Kept apart from DecoderStatus because getStatusNative() copies DecoderStatus
// every second, and pictures are big. Protected by g_statusMutex too.
struct Picture {
    int lot = -1;                   // LOT number the station gave the file
    std::string name;               // file name, e.g. "SLKRTH$$...png"
    std::vector<uint8_t> bytes;     // the file itself (PNG or JPEG); empty = none
};

static const int ART_WANT_ANY = -2;     // no XHDR seen yet -> show the newest album art
static const int ART_WANT_NONE = -1;    // station said "no picture" -> show the logo
static const size_t MAX_RECENT_ART = 6; // album art pictures kept per program

struct ProgramPictures {
    Picture logo;                   // this program's logo
    std::deque<Picture> recentArt;  // newest last
    int wantedArt = ART_WANT_ANY;   // ART_WANT_ANY, ART_WANT_NONE, or the LOT number to show
};
static ProgramPictures g_pictures[MAX_PROGRAMS];
static Picture g_stationLogo;           // a logo carried by a DATA service (not tied to one program)
static int g_otherLots = 0;             // LOT files we don't show (weather, traffic, unknown...)
static std::string g_lastLot;           // the latest LOT file, for the debug text

// Goes up by one whenever any picture changes (or we retune). Kotlin checks it
// every second and only fetches the pictures again when it has changed.
static std::atomic<int> g_pictureGen{0};

// Is this file name an iHeart-style logo? (iHeart logo files start with "SL",
// e.g. "SLKRTH$$...png" - a fallback for stations that label logos wrongly.)
static bool looksLikeLogo(const char *name) {
    return name != nullptr && name[0] == 'S' && name[1] == 'L';
}

// 9b: text from the station (song titles, names) -> a Java String, safely.
// JNI's NewStringUTF() crashes the app (in debug builds) on characters it
// doesn't expect, e.g. an emoji in a song title or a broken byte. So we decode
// the UTF-8 ourselves into UTF-16 (what Java strings use) and replace anything
// broken with "?".
static jstring toJString(JNIEnv *env, const std::string &s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80)              { cp = c;        extra = 0; }
        else if ((c >> 5) == 0x06) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0x0E) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { out += u'?'; i++; continue; }            // not a valid first byte
        bool ok = true;                                  // check the continuation bytes
        for (int k = 1; k <= extra; k++) {
            if (i + k >= s.size() || ((unsigned char)s[i + k] >> 6) != 0x02) { ok = false; break; }
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out += u'?'; i++; continue; }
        if (cp >= 0x10000) {                             // e.g. emoji: needs two UTF-16 units
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
        i += extra + 1;
    }
    return env->NewString(reinterpret_cast<const jchar *>(out.data()), (jsize)out.size());
}

// "HD1" .. "HD8"
static std::string hdName(int program) {
    return "HD" + std::to_string(program + 1);
}

// Program type number -> text, e.g. 10 -> "Top 40" (nrsc5's own list).
static std::string typeName(int type) {
    if (type < 0) return "";
    const char *name = nullptr;
    nrsc5_program_type_name((unsigned)type, &name);
    return name ? name : "";
}

// 9d: service mode number -> its usual name. FM: 1 -> "MP1", 3 -> "MP3",
// 11 -> "MP11". AM: 1 -> "MA1", 2 -> "MA3" (AM uses its own numbering).
static std::string modeName(int psmi) {
    if (psmi <= 0) return "";
    if (g_mode == NRSC5_MODE_AM) return psmi == 1 ? "MA1" : psmi == 2 ? "MA3" : "MA?";
    return "MP" + std::to_string(psmi);
}

// 9d: should the screen say "synced"? Yes while really synced, and also for
// SYNC_GRACE_MS after losing it (the grace period).
static bool syncShown(const DecoderStatus &st) {
    if (st.synced) return true;
    return st.syncCount > 0 && msSince(st.lostSyncAt) < SYNC_GRACE_MS;
}

// ---- Audio ring buffer (Milestone 7) ----
// nrsc5 hands us decoded audio (16-bit, stereo interleaved L,R,L,R..., 44,100 Hz)
// on the streaming thread. Kotlin's audio thread takes it out and plays it.
// A "ring" buffer: when the write position reaches the end it wraps to the start.
static const size_t AUDIO_RING_VALUES = 44100 * 2 * 4; // 4 seconds of stereo audio
static std::vector<int16_t> g_ring(AUDIO_RING_VALUES);
static size_t g_ringRead = 0, g_ringWrite = 0, g_ringCount = 0;
static long long g_audioDropped = 0;                   // values thrown away (buffer full)

// 9b step 3: smooth program switching. Instead of emptying the ring (which
// made a 0.5 s gap), we fade the old program OUT over its last 20 ms still in
// the ring, and fade the new program IN over its first 20 ms. Both programs
// are decoded in step, so the new one simply continues where the old one ends.
static const size_t FADE_VALUES = 44100 * 2 * 20 / 1000;   // 20 ms of stereo = 1,764 values
static size_t g_fadeInLeft = 0;                            // new-program values still to fade in
static std::mutex g_audioMutex;
static std::condition_variable g_audioCv;              // "new audio arrived" signal

// ======================= M11 (11a): analog FM =======================
// The same IQ samples that feed nrsc5 also go through our own FM demodulator
// (fmdemod.hpp), on the streaming thread. Its audio (44,100 Hz, float,
// 1.0 = 75 kHz deviation; 11b: STEREO, interleaved L R) lands in a second ring.
// readAudioNative() serves ONE of the two rings, chosen by the "HD Radio" setting (setAudioSourceNative):
//   SOURCE_HD     = digital only (the HD ring, as before; 11d: demodulator + aligner OFF)
//   SOURCE_ANALOG = analog only
//   SOURCE_AUTO   = 11d: the blend (blend.hpp) - analog at once, HD1 when it's
//                  good and aligned; HD2+ play live from the HD ring as always
static const int SOURCE_HD = 0, SOURCE_ANALOG = 1, SOURCE_AUTO = 2;
static std::atomic<int> g_audioSource{SOURCE_HD};
static fm::Demod g_fm;                                    // only the streaming thread touches it (reset before the thread starts)
static const size_t FM_RING_FRAMES = 44100 * 4;           // 4 seconds of audio (11b: stereo frames, 2 values each)
static std::vector<int16_t> g_fmRing(FM_RING_FRAMES * 2);
static size_t g_fmRingRead = 0, g_fmRingWrite = 0, g_fmRingCount = 0;   // in frames
static long long g_fmDropped = 0;
static size_t g_sourceFadeLeft = 0;                       // stereo values still to fade in after a source switch
// Analog level: 1.0 = 75 kHz deviation = "100 % modulation". Stations run their
// peaks right up to (and a bit over) that, so 0.7 keeps them out of the clipper.
// The sign is PLUS: the carrier swinging UP in frequency = a positive audio sample, as the
// FM standard says. (Checked 2026-10-02: in a KLOS 95.5 recording the station at +400 kHz
// in the samples is 95.9, so the dongle's spectrum is the right way up and our
// discriminator's sign is the true one.) Until then this was -0.7, to match nrsc5's HD
// audio - which turned out to be the inverted one: see fixHdAudio().
static const float FM_OUTPUT_GAIN = 0.7f;
// Stats for the signal details (written by the streaming thread, read by the UI).
static std::atomic<float> g_fmCarrierDb{-99.0f};          // level of the filtered FM channel (dB, 0 = full scale)
static std::atomic<float> g_fmOffsetHz{0.0f};             // carrier offset the demodulator sees
static std::atomic<float> g_fmQuietDb{-99.0f};            // 9k: FM quieting (noise 60-100 kHz, dB re 75 kHz; lower = better)
// 11b: stereo. The setting (setStereoModeNative) is handed to the demodulator at the
// start of every block on the streaming thread; the demodulator reports back whether a
// 19 kHz pilot is there, how strong (1.0 = 75 kHz deviation, stations send 0.08-0.10)
// and the stereo amount in use (0 = mono ... 1 = full stereo; the blend on noisy signals).
static std::atomic<int> g_stereoMode{fm::STEREO_AUTO};
static std::atomic<int> g_deemphUs{75};                    // 11b step 2: 75 (Americas) or 50 us, by region
static std::atomic<int> g_fmPilot{0};
static std::atomic<float> g_fmPilotLevel{0.0f};
static std::atomic<float> g_fmStereo{0.0f};
// M12: RDS (rds.hpp, inside the demodulator). The streaming thread copies the decoder's
// status here whenever it changed; the UI reads it in getSignalNative. g_rdsMutex is a
// LEAF lock (never held while taking another).
static std::mutex g_rdsMutex;
static rds::Status g_rds;
static uint32_t g_rdsSerial = 0;                          // streaming thread only: the decoder's change counter last copied
// M12 step 2: "is the HD the same station as the analog?" - the RDS PI against the HD's
// call sign (SIS). g_hdCall and g_sameCall are under g_rdsMutex; g_sameStation is
// blend::SAME_UNKNOWN / SAME_YES / SAME_NO, kept until the next tune (so it survives the
// demodulator being switched off, e.g. "tap to listen" to the other station's HD).
static std::string g_hdCall;                              // the HD station's name as sent ("KSOF-FM")
static std::string g_sameCall;                            // the ANALOG station's call letters, when known
static std::atomic<int> g_sameStation{blend::SAME_UNKNOWN};
// 13b: the FCC station list. Kotlin (Stations.kt) reads the list built into the app and,
// before every tune, hands over the call letters licensed on that frequency
// (setChannelCallsNative): "KHYL", "KWYE", "KRTH"... - letters only, no "-FM". Two uses:
//  - a "1xxx" RDS code (rds.hpp: iHeart & co. replace the first digit of the PI by 1, which
//    leaves nine possible calls): normally only ONE of the nine is licensed on this
//    frequency -> that is the station (tableCall);
//  - an HD station name counts as "another station's call sign" only if somebody with
//    that call IS licensed here (KTWV's HD calls itself "WAVE" - a name, not a call).
// Both under g_rdsMutex. g_channelKnown = false: no list for this frequency (AM, another
// region) -> the rules from before the list apply.
static std::vector<std::string> g_channelCalls;
static bool g_channelKnown = false;

static std::atomic<int> g_fmLoadPct{0};                   // demodulator CPU time, % of real time
static std::atomic<int> g_hdLoadPct{0};                   // 11d: nrsc5's own decode time, % of real time (for comparison)

// 9i: the two loads above are averaged over ~1 s of samples (about 11 blocks).
// One 88 ms block on its own jumped around too much (5 % one second, 26 % the
// next) to compare anything. Used by the streaming thread only; reset in
// startStreamNative before that thread starts.
struct LoadMeter {
    double usedMs = 0, blockMs = 0;
    void reset() { usedMs = blockMs = 0; }
    // Adds one block; publishes the average to `out` once ~1 s has been collected.
    void add(double used, double block, std::atomic<int> &out) {
        usedMs += used;
        blockMs += block;
        if (blockMs >= 1000.0) {
            out = (int)(usedMs / blockMs * 100.0 + 0.5);
            usedMs = blockMs = 0;
        }
    }
};
static LoadMeter g_fmLoad, g_hdLoad;

// ---- M11 (11c): analog <-> HD alignment ----
// The aligner gets a copy of BOTH audio streams (analog from demodAnalog, HD1
// from onNrsc5Event) on the streaming thread and, when Kotlin asks
// (measureAlignmentNative, engine thread, every 2 s), works out how far the
// analog lags the HD and how much louder it is (decision 33). 11d uses that
// to blend. (11c ran the demodulator in every mode for this; since 11d it runs
// only in Auto and Analog only - Digital only is digital only.)
static align::Aligner g_align;
// IQ position (samples since the tune) of the block being processed right now
// (streaming thread only) - the aligner anchors nrsc5's audio pieces to it.
// (11d: g_iqPos is also read by setAudioSourceNative on the engine thread -> atomic.)
static std::atomic<long long> g_iqPos{0};
static long long g_iqBlockStart = 0, g_iqBlockLen = 0;

// ---- Build 6: the dongle's clock against the station's (drift.hpp) ----
// The analog audio is paced by the dongle's crystal, the HD audio by the station's clock.
// With a cheap crystal (50 ppm and more) the two drift apart by frames per second, the
// aligner never sees two measurements agree, and the blend stays on analog for ever. So
// the analog audio that goes to the aligner and the blender passes through a resampler
// first (g_stretch, streaming thread only) which puts it on the station's clock; g_drift
// decides by how much - from the tuning error at first, then from the aligner's own
// measurements. The analog ring ("Analog only") gets the audio as it comes: there is
// nothing to line up there.
// g_slip = the frames the resampler has taken out so far. The time line the aligner and
// the blender count on is "IQ time / 33.75 - g_slip": every IQ position is converted with
// timelineFrame() before it goes to them.
static drift::Stretch g_stretch;
static drift::Tracker g_drift;
static double g_driftRate = 0;                            // streaming thread: the correction for the next block
static double g_idleSlip = 0;                             // streaming thread: frames "taken out" while the demodulator was off
static std::atomic<double> g_slip{0.0};
static std::atomic<int> g_tunedHz{0};                     // the tuned frequency (the tuning error is judged against it)
static std::vector<float> g_fixedAudio;                   // streaming thread: the analog audio on the station's clock
static long long timelineFrame(long long iqSamples) {
    return (long long)std::llround((double)iqSamples / align::IQ_PER_FRAME - g_slip.load());
}

// ---- M11 (11d): the blend ----
// In Auto, both audio streams go into the blender (analog from demodAnalog, HD1
// from onNrsc5Event) and readAudioNative() takes the mixed output from it. It
// has its own lock (a leaf, like the aligner's) and its own wake-up.
static blend::Blender g_blend;

// ---- M12 step 2: "is the HD the same station as the analog?" (globals above, with RDS) ----
// The call letters in an HD station name. Stations send "KKLQ", "KGB-FM", but also
// "101.5 KGB" (seen 09-30), so look at each word: the first one that is K/W + 2-3 letters
// (a "-FM" / "-HD1" ending dropped) counts. exact = the whole name is just the call (+ ending).
// "" when there is none - Mexican XH..., "MPS", an empty name: can't be judged.
static std::string usCall(const std::string &name, bool *exact = nullptr) {
    std::vector<std::string> words;
    std::string w;
    for (char ch : name + " ") {
        if (ch == ' ' || ch == '/' || ch == ',') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    for (const std::string &word : words) {
        std::string c = word.substr(0, word.find('-'));
        if (c.size() < 3 || c.size() > 4 || (c[0] != 'K' && c[0] != 'W')) continue;
        bool letters = true;
        for (char ch : c) if (ch < 'A' || ch > 'Z') letters = false;
        if (!letters) continue;
        if (exact) *exact = words.size() == 1;
        return c;
    }
    return "";
}

// 13b: is somebody with these call letters licensed on the tuned frequency? (g_rdsMutex held)
static bool licensedHere(const std::string &call) {
    return std::find(g_channelCalls.begin(), g_channelCalls.end(), call) != g_channelCalls.end();
}

// 13b: the call letters for an RDS code of the 1xxx kind, from the FCC list: the ONE of its
// nine possible calls that is licensed on this frequency. "" when the list has none or
// several of them here (about 3 in 100 stations share their frequency with another of
// "their" nine somewhere in the country - then it stays open), or when there is no list.
// (g_rdsMutex held)
static std::string tableCall(uint16_t pi) {
    if (!g_channelKnown || (pi >> 12) != 1) return "";
    std::string found;
    for (const std::string &c : rds::callCandidates(pi)) {
        if (!licensedHere(c)) continue;
        if (!found.empty()) return "";                    // two of the nine on this frequency: can't tell
        found = c;
    }
    return found;
}

// ---- 14b step 4: an RDS code that doesn't fit the frequency ------------------------------------
// KGGI 99.1 Riverside sends the PI 69D8, which by the call-letter formula is "WIAQ" - a call
// that isn't licensed on 99.1 (or anywhere). The app showed "WIAQ", decided the HD ("KGGI")
// was another station's, hid the logo and refused to blend (Derek, 2026-10-02). KXLU 88.9
// sends 3712 = "KOUS". Stations keep a stale or mistyped PI for years because US radios
// don't use it for anything.
// With the FCC list at hand (g_channelKnown) a call from the PI is now only taken at once if
// it is licensed on the tuned frequency. If it is not:
//  1. the station's own text decides: a call that IS licensed here and appears as a word in
//     the PS or the RadioText ("99.1 KGGI - Bad Bunny - Dtmf") is the station's call;
//  2. otherwise it may be a translator relaying its parent station (the parent's call is
//     licensed on another frequency) - or a wrong code. The call from the PI is then held
//     back for RDS_CALL_HOLD_MS, so the text gets its chance first, and shown after that.
// g_rdsCallSure: the call is licensed on this frequency (or there is no list to check).
// All under g_rdsMutex.
static const long long RDS_CALL_HOLD_MS = 10000;
static bool g_rdsCallSure = false;
static std::string g_rdsTextCall;                         // the call found in the station's text ...
static uint16_t g_rdsTextPi = 0;                          // ... and the PI it was found with
static long long g_rdsPiSinceMs = -1;                     // msSinceTune() when this PI was first seen
static uint16_t g_rdsPiSeen = 0;
static bool g_rdsCallHeld = false;                        // streaming thread only: settleRdsCall() said "look again"

// A call licensed on this frequency that appears as a whole word in `text` ("" = none).
static std::string callInText(const std::string &text) {
    std::string word;
    for (size_t i = 0; i <= text.size(); i++) {
        unsigned char ch = i < text.size() ? (unsigned char)text[i] : ' ';
        if (std::isalnum(ch)) { word.push_back((char)std::toupper(ch)); continue; }
        if (word.size() >= 3 && word.size() <= 4 && (word[0] == 'K' || word[0] == 'W') && licensedHere(word))
            return word;
        word.clear();
    }
    return "";
}

// Settles now.call (see above). Returns true while a call is being held back - the caller
// then looks again a little later even if the RDS data itself didn't change.
static bool settleRdsCall(rds::Status &now) {
    g_rdsCallSure = false;
    if (!now.piOk) return false;
    // 13b: a 1xxx code -> the call letters from the FCC list, if it has exactly one of the
    // nine candidates on this frequency. (The decoder's own rule comes first: a candidate
    // the station's text mentions - "101.5 KGB" - is the station speaking for itself.)
    if (now.call.empty()) now.call = tableCall(now.pi);
    if (!g_channelKnown) { g_rdsCallSure = true; return false; }      // no list: as before 13b
    if (!now.call.empty() && licensedHere(now.call)) { g_rdsCallSure = true; return false; }
    if (g_rdsPiSeen != now.pi || g_rdsPiSinceMs < 0) { g_rdsPiSeen = now.pi; g_rdsPiSinceMs = msSinceTune(); }
    if (g_rdsTextPi != now.pi) { g_rdsTextCall.clear(); g_rdsTextPi = now.pi; }
    if (g_rdsTextCall.empty()) {
        g_rdsTextCall = callInText(now.ps + " " + now.rt);
        if (!g_rdsTextCall.empty())
            LOGI("RDS: PI %04X = \"%s\" is not licensed on this frequency; the station's text says %s",
                 now.pi, now.call.c_str(), g_rdsTextCall.c_str());
    }
    if (!g_rdsTextCall.empty()) { now.call = g_rdsTextCall; g_rdsCallSure = true; return false; }
    if (now.call.empty()) return false;
    if (msSinceTune() - g_rdsPiSinceMs < RDS_CALL_HOLD_MS) { now.call.clear(); return true; }
    return false;                                                      // shown, but not "sure"
}

// Judge "same station?" again (the PI or the HD name changed). Called on the streaming
// thread (RDS copy, station-name event). Needs g_rdsMutex FREE (takes it; the blend's lock
// is taken after it is released - both are leaf locks).
static void judgeSameStation() {
    int verdict = blend::SAME_UNKNOWN;
    uint16_t pi = 0;
    std::string hd;
    {
        std::lock_guard<std::mutex> lock(g_rdsMutex);
        bool exact = false;
        hd = usCall(g_hdCall, &exact);
        if (!g_rds.piOk || hd.empty()) return;
        pi = g_rds.pi;
        std::vector<std::string> cands = rds::callCandidates(pi);
        if (cands.empty()) return;                        // a PI that isn't a US call (Mexico, 0000...)
        // 13b: a 1xxx code whose call letters are settled (g_rds.call: from the station's own
        // text, or from the FCC list - see demodAnalog) -> compare with that call alone, not
        // with all nine candidates.
        // 14b step 4: with the FCC list, a call that is settled AND licensed on this frequency
        // (g_rdsCallSure: by the formula, the list or the station's own text) is compared
        // directly - so KGGI's wrong PI no longer makes its own HD "another station".
        bool sure = g_channelKnown && g_rdsCallSure && !g_rds.call.empty();
        bool same = sure || ((pi >> 12) == 1 && !g_rds.call.empty()) ? g_rds.call == hd
                                                                     : rds::piMatchesCall(pi, hd);
        if (same) verdict = blend::SAME_YES;
        // "another station" only from a name that IS a call - 13b: and only if that call is
        // licensed on this frequency (with the list at hand). "WAVE" on 94.7 is just a name.
        // 14b step 4: ... and only if the ANALOG's call is licensed here too: two different
        // stations on one frequency. A PI call from elsewhere (a relay, or a wrong code)
        // proves nothing - the audio comparison decides then, as it does without RDS.
        else if (exact && (!g_channelKnown || (licensedHere(hd) && sure))) verdict = blend::SAME_NO;
        else return;                                      // "101.5 KGB"-style name without a match: can't tell
        g_sameCall = !g_rds.call.empty() ? g_rds.call
                   : !g_channelKnown && cands.size() == 1 ? cands[0] : "";
    }
    if (g_sameStation.exchange(verdict) != verdict) {
        g_blend.setSameStation(verdict);
        LOGI("Same station? RDS PI %04X vs HD \"%s\": %s", pi, hd.c_str(),
             verdict == blend::SAME_YES ? "yes" : "NO - the HD is another station");
    }
}

// 11d: does the analog demodulator (and with it the aligner + blender) run?
// Only when the analog can be heard: Analog only, or Auto. In Digital only it's
// off (Derek's choice, 11c) - saves 10-20 % of a core.
static bool demodWanted() { return g_audioSource.load() != SOURCE_HD; }

// 12a step 3: and the HD decoder (nrsc5)? Only when the HD can be heard: Digital only, Auto,
// and always on AM. In Analog only it gets no samples at all (Derek: choosing Analog only is
// deliberate) - that saves the 23-34 % of a core nrsc5 burns SEARCHING on a station without
// HD (4-5 % on one it decodes). The mirror image of 11d's "Digital only = demodulator off".
// The streaming thread opens / closes nrsc5 when this changes (onSamples).
static bool hdWanted() { return g_mode == NRSC5_MODE_AM || g_audioSource.load() != SOURCE_ANALOG; }
static std::atomic<bool> g_hdOn{false};      // nrsc5 is being fed (for the screen: "HD off")
static bool g_hdOpenFailed = false;          // streaming thread only: don't retry a failed open every block

// Build 9: and in AUTO, on a station that turns out to have no HD? nrsc5 would search for
// ever, at 23-34 % of a core - as much as, or more than, the analog demodulator that makes
// all the sound there. So after 30 s without sync the search RESTS: nrsc5 gets 3 s of
// samples out of every 30, until one of those looks finds HD (hdsearch.hpp has the rule
// and the reasons). While it rests nrsc5 stays open and simply is not fed. Whenever the
// samples start again after a rest - a look, or the listener switching to Digital only -
// it is with a FRESH decoder (close + open, a few allocations), for two reasons found
// with the harness: a decoder that kept its state across the gap synced twice (the first
// time 20 Hz beside the station's real tuning error; it lost that sync two seconds later
// and synced again, and the audio came 1.5 s later than with a fresh one), and a decoder
// that had played audio before the rest went on handing out silent audio pieces from the
// first sample of the look - seconds of zeros at the head of the new stretch of HD audio,
// which the aligner then tried to match with the analog. A fresh decoder says nothing
// until it has real audio, exactly as after a tune - the path every station survey and
// every blend test went down.
// Only in Auto on HD1: in Digital only, on AM and on an HD2+ program there is nothing else
// to hear, so the search runs all the time there, as before.
static hdsearch::Rest g_hdSearch;                          // streaming thread only (reset before it starts)
static int g_hdSyncsSeen = 0;                              // streaming thread only: syncs counted at the last block
static int g_hdRestWas = hdsearch::SEARCHING;              // streaming thread only: for the log
static bool g_hdFed = true;                                // streaming thread only: nrsc5 got the block before this one
static std::atomic<int> g_hdRest{hdsearch::SEARCHING};     // for the screen: searching / resting / looking
static std::atomic<int> g_hdLookInS{0};                    // for the screen: seconds until the next look
static std::atomic<bool> g_hdRestAllowed{true};            // tests only (setHdRestNative)

// Called from onNrsc5Event (streaming thread): add audio to the ring,
// but only if it belongs to the selected program (9b). The check happens
// INSIDE the lock, so after setProgramNative() returns, not a single value
// of the old program can sneak into the ring.
// ---- HD audio: left/right and polarity (2026-10-02) -----------------------------------------
// nrsc5 (v3.2.0 with the FAAD2 "HDC" patch) hands out the HD audio with LEFT AND RIGHT
// SWAPPED and the polarity inverted, compared with the same station's analog audio decoded
// the standard way. Measured on 20 Los Angeles stations (every one the same) and re-checked
// against a second FM stereo decoder (ngsoftfm): nrsc5's left = -(the analog's right),
// nrsc5's right = -(the analog's left). A station feeds one stereo program to both its
// analog and its HD side, and a real HD radio blends between the two - so they must agree,
// and the analog side is the one with a 60-year-old standard behind it.
// (Until this fix the app had bent its ANALOG audio to match nrsc5 instead, so everything
// played with left and right exchanged.)
// Corrected once, here, for every program (HD1, HD2 ..., AM too):
//     left = -(nrsc5's right)      right = -(nrsc5's left)
//
// 14a step 2 - the station's "TX digital audio gain": every HD program carries a small
// number in its audio header, -8 ... +7 dB, "the offset to be applied to the digital audio
// level at the receiver to equalize the subjective loudness of the digital and analog
// audio" (NRSC-5, SY_IDD_1017s section 5.2.1.3). nrsc5 reports it (NRSC5_EVENT_AUDIO_SERVICE)
// but does not apply it - that is the receiver's job, so it is done here as well.
// Most stations send 0. KLOS sends +4: its HD1 came out 4.2 dB quieter than its analog
// until this step (the blend then turned the analog DOWN to match, so KLOS played 4 dB
// quieter than its neighbours). Survey 2026-10-02: KLOS +4, KSBR +2, KCRW HD2 +6.
//
// Called only from the nrsc5 callback (= the streaming thread), so one buffer is enough.
static std::atomic<int> g_hdGainDb[MAX_PROGRAMS];        // dB per program; 0 = none (static = starts at 0)
static std::vector<int16_t> g_hdFixed;
static const int16_t *fixHdAudio(const int16_t *data, size_t count, unsigned program) {
    g_hdFixed.resize(count);
    int db = program < (unsigned)MAX_PROGRAMS ? g_hdGainDb[program].load() : 0;
    if (db == 0) {
        // the usual case: exact, no rounding
        for (size_t i = 0; i + 1 < count; i += 2) {
            int left = -(int)data[i + 1], right = -(int)data[i];
            g_hdFixed[i]     = (int16_t)(left  > 32767 ? 32767 : left);    // -(-32768) doesn't fit in 16 bits
            g_hdFixed[i + 1] = (int16_t)(right > 32767 ? 32767 : right);
        }
    } else {
        // dB -> factor: 10^(dB/20). +6 dB doubles the samples, so clamp (a station that asks
        // for gain leaves room for it: KLOS's HD1 peaks at -5 dBFS before its +4 dB).
        float k = -std::pow(10.0f, (float)db / 20.0f);
        for (size_t i = 0; i + 1 < count; i += 2) {
            float left = (float)data[i + 1] * k, right = (float)data[i] * k;
            g_hdFixed[i]     = (int16_t)std::lrintf(left  > 32767.f ? 32767.f : left  < -32768.f ? -32768.f : left);
            g_hdFixed[i + 1] = (int16_t)std::lrintf(right > 32767.f ? 32767.f : right < -32768.f ? -32768.f : right);
        }
    }
    if (count & 1) g_hdFixed[count - 1] = data[count - 1];             // never happens (stereo), but stay tidy
    return g_hdFixed.data();
}

static void pushAudio(int program, const int16_t *data, size_t count) {
    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (program != g_program) return;
        // M11 (11a): while the analog audio plays, the HD audio isn't kept in the
        // ring (it would just pile up 4 s of stale sound). 11d: in Auto, HD1 goes
        // to the blender instead (onNrsc5Event feeds it directly); HD2+ still
        // play live from this ring.
        int src = g_audioSource.load();
        if (src == SOURCE_ANALOG || (src == SOURCE_AUTO && program == 0)) return;
        for (size_t i = 0; i < count; i++) {
            int16_t v = data[i];
            if (g_fadeInLeft > 0) {
                // Fade in: the volume rises from almost 0 to full over FADE_VALUES.
                // (Counted in stereo frames, so left and right get the same volume.)
                size_t frame = (FADE_VALUES - g_fadeInLeft) / 2;
                v = (int16_t)(v * (float)(frame + 1) / (FADE_VALUES / 2));
                g_fadeInLeft--;
            }
            g_ring[g_ringWrite] = v;
            g_ringWrite = (g_ringWrite + 1) % AUDIO_RING_VALUES;
            if (g_ringCount == AUDIO_RING_VALUES) {
                // Full: the reader fell behind -> drop the oldest value.
                g_ringRead = (g_ringRead + 1) % AUDIO_RING_VALUES;
                g_audioDropped++;
            } else {
                g_ringCount++;
            }
        }
    }
    g_audioCv.notify_one();   // wake the reader if it's waiting
}

static void clearAudio() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_ringRead = g_ringWrite = g_ringCount = 0;
    g_audioDropped = 0;
    g_fadeInLeft = 0;
    g_fmRingRead = g_fmRingWrite = g_fmRingCount = 0;    // M11
    g_fmDropped = 0;
    g_sourceFadeLeft = 0;
}

// Streaming thread: demodulate one block of samples and queue the audio.
static void demodAnalog(const unsigned char *buf, uint32_t len) {
    Clock::time_point t0 = Clock::now();
    g_fm.setStereoMode(g_stereoMode.load());         // 11b: the setting (changed from the engine thread)
    g_fm.setDeemphasis(g_deemphUs.load());           // 11b step 2: the region's de-emphasis
    g_fm.processCu8(buf, len);
    std::vector<float> &out = g_fm.audioOut;         // 11b: stereo, interleaved L R
    if (!out.empty()) {
        // Scale and clamp in place (11c: the aligner gets exactly what goes into the ring).
        for (float &v : out) {
            v *= FM_OUTPUT_GAIN;
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        }
        size_t frames = out.size() / 2;
        {
            std::lock_guard<std::mutex> lock(g_audioMutex);
            for (size_t i = 0; i < frames; i++) {
                g_fmRing[g_fmRingWrite * 2] = (int16_t)(out[2 * i] * 32767.0f);
                g_fmRing[g_fmRingWrite * 2 + 1] = (int16_t)(out[2 * i + 1] * 32767.0f);
                g_fmRingWrite = (g_fmRingWrite + 1) % FM_RING_FRAMES;
                if (g_fmRingCount == FM_RING_FRAMES) {
                    g_fmRingRead = (g_fmRingRead + 1) % FM_RING_FRAMES;   // full: drop the oldest
                    g_fmDropped++;
                } else {
                    g_fmRingCount++;
                }
            }
        }
        // Build 6: from the dongle's clock to the station's (drift.hpp). The aligner and the
        // blender get this corrected audio; a frame more or less than came in now and then.
        g_fixedAudio.clear();
        g_stretch.process(out.data(), frames, g_driftRate, g_fixedAudio);
        size_t fixedFrames = g_fixedAudio.size() / 2;
        const double slip = g_stretch.slip() + g_idleSlip;
        g_slip = slip;
        // 11b: the aligner keeps working on mono (L+R)/2 - the same signal as before stereo,
        // whatever the blend does to L-R (its cross-correlation with HD1 is unchanged).
        static std::vector<float> mono;              // streaming thread only
        mono.resize(fixedFrames);
        for (size_t i = 0; i < fixedFrames; i++) mono[i] = 0.5f * (g_fixedAudio[2 * i] + g_fixedAudio[2 * i + 1]);
        long long timeline = g_align.pushAnalog(mono.data(), fixedFrames);   // 11c (its own lock, taken after ours - never inside)
        g_blend.pushAnalog(g_fixedAudio.data(), fixedFrames);   // 11d (likewise; wakes the reader in Auto). 11b: stereo
        g_fm.audioOut.clear();
        g_fmCarrierDb = g_fm.carrierDbfs();
        g_fmOffsetHz = g_fm.freqOffsetHz();
        // ... and how much to correct in the next block.
        g_driftRate = g_drift.update(timeline, slip, g_fm.freqOffsetHz(), (double)g_tunedHz.load(),
                                     (double)len / 2.0 / SAMPLE_RATE);
        g_fmQuietDb = g_fm.quietingDb();             // 9k
        g_fmPilot = g_fm.pilot() ? 1 : 0;            // 11b
        g_fmPilotLevel = g_fm.pilotLevel();
        g_fmStereo = g_fm.stereoAmount();
    }
    // M12: RDS - copy the decoder's status when something changed (a few times a second at most).
    // 14b step 4: ... or while a call is being held back (it becomes due without any new RDS data).
    bool rdsChanged = g_fm.rds().serial() != g_rdsSerial || g_rdsCallHeld;
    if (rdsChanged) {
        g_rdsSerial = g_fm.rds().serial();
        rds::Status now = g_fm.rds().status();
        std::lock_guard<std::mutex> lock(g_rdsMutex);
        g_rdsCallHeld = settleRdsCall(now);           // 13b + 14b step 4: the call letters
        if (now.piOk && !g_rds.piOk) LOGI("RDS: PI %04X %s", now.pi, now.call.c_str());
        g_rds = now;
    }
    if (rdsChanged) judgeSameStation();               // M12 step 2 (outside g_rdsMutex)
    if (g_audioSource.load() == SOURCE_ANALOG) g_audioCv.notify_one();
    // Load: time spent here vs the time this block of samples represents.
    double blockMs = len / 2.0 / SAMPLE_RATE * 1000.0;
    double usedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (blockMs > 0) g_fmLoad.add(usedMs, blockMs, g_fmLoadPct);   // 9i: 1 s average
}

// Turns librtlsdr's tuner number into a readable name.
static const char *tunerName(enum rtlsdr_tuner t) {
    switch (t) {
        case RTLSDR_TUNER_E4000:  return "Elonics E4000";
        case RTLSDR_TUNER_FC0012: return "Fitipower FC0012";
        case RTLSDR_TUNER_FC0013: return "Fitipower FC0013";
        case RTLSDR_TUNER_FC2580: return "FCI FC2580";
        case RTLSDR_TUNER_R820T:  return "Rafael Micro R820T";
        case RTLSDR_TUNER_R828D:  return "Rafael Micro R828D";
        default:                  return "unknown";
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    // Asking nrsc5 for its version proves the whole library linked in.
    const char *version = nullptr;
    nrsc5_get_version(&version);
    std::string hello = "Hello from C++ - nrsc5 ";
    hello += (version != nullptr) ? version : "?";
    return env->NewStringUTF(hello.c_str());
}

// Kotlin (RadioEngine): external fun openDongleNative(fd: Int): String
// Opens the dongle with the fd Android gave us and reports what it found.
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_openDongleNative(
        JNIEnv* env,
        jobject /* this */,
        jint fd) {
    if (g_dev != nullptr) {
        return env->NewStringUTF("librtlsdr: already open");
    }

    LOGI("rtlsdr_open_fd(fd=%d)...", fd);
    int r = rtlsdr_open_fd(&g_dev, fd);
    if (r < 0) {
        g_dev = nullptr;
        LOGE("rtlsdr_open_fd failed: %d", r);
        std::string msg = "librtlsdr: open FAILED (error " + std::to_string(r) + ")";
        return env->NewStringUTF(msg.c_str());
    }

    // Ask the dongle about itself - proves we can really talk to it.
    char manufact[256] = {0};
    char product[256] = {0};
    char serial[256] = {0};
    rtlsdr_get_usb_strings(g_dev, manufact, product, serial);

    enum rtlsdr_tuner tuner = rtlsdr_get_tuner_type(g_dev);
    int gainCount = rtlsdr_get_tuner_gains(g_dev, nullptr);  // nullptr = just count them

    LOGI("Opened! manufacturer='%s' product='%s' serial='%s'", manufact, product, serial);
    LOGI("Tuner: %s, %d gain steps", tunerName(tuner), gainCount);

    std::string msg = "librtlsdr: opened OK\n";
    msg += std::string(manufact) + " " + product + " (serial " + serial + ")\n";
    msg += "Tuner: " + std::string(tunerName(tuner)) + "\n";
    msg += "Gain steps: " + std::to_string(gainCount);
    return env->NewStringUTF(msg.c_str());
}

// Kotlin (RadioEngine): external fun closeDongleNative()
// Shuts down librtlsdr + libusb. Must run BEFORE Kotlin closes the
// UsbDeviceConnection (which closes the fd).
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_closeDongleNative(
        JNIEnv* /* env */,
        jobject /* this */) {
    stopStreaming();   // safety: never close while the streaming thread is running
    if (g_dev != nullptr) {
        LOGI("rtlsdr_close()");
        rtlsdr_close(g_dev);
        g_dev = nullptr;
    }
    g_direct = false;   // 14c: the next dongle starts with its tuner in use
}


// ======================= Milestone 6: decoding =======================

// 9d step 3: a complete LOT file arrived. Called from onNrsc5Event, with
// g_statusMutex held. Decide what it is and which program it belongs to.
static void onLotFile(const nrsc5_event_t *evt) {
    const char *name = evt->lot.name ? evt->lot.name : "";
    uint32_t mime = evt->lot.component ? evt->lot.component->data.mime : 0;

    // Which program? The file travels inside a SIG service. If that's an
    // AUDIO service (MPS, SPS1...), its audio port is the program number.
    int program = -1;
    const nrsc5_sig_service_t *svc = evt->lot.service;
    if (svc != nullptr && svc->type == NRSC5_SIG_SERVICE_AUDIO && svc->audio_component != nullptr)
        program = svc->audio_component->audio.port;
    if (program >= MAX_PROGRAMS) program = -1;

    bool isArt = (mime == NRSC5_MIME_PRIMARY_IMAGE);
    bool isLogo = (mime == NRSC5_MIME_STATION_LOGO) || (!isArt && looksLikeLogo(name));
    // 11d follow-up (KLOS never showed album art): the song's XHDR names the LOT
    // number of its picture. If a file with that number arrives outside the
    // program's own audio service (a data service, program -1) or with a MIME we
    // don't know, it is still that song's picture - trust the XHDR link.
    if (!isLogo) {
        for (int p = 0; p < MAX_PROGRAMS; p++) {
            if (g_pictures[p].wantedArt >= 0 && g_pictures[p].wantedArt == (int)evt->lot.lot) {
                if (program < 0 || !isArt) LOGI("LOT %u matches %s's XHDR - treating it as album art", evt->lot.lot, hdName(p).c_str());
                program = p;
                isArt = true;
                break;
            }
        }
    }

    Picture pic;
    pic.lot = (int)evt->lot.lot;
    pic.name = name;
    if (evt->lot.data != nullptr && evt->lot.size > 0)
        pic.bytes.assign(evt->lot.data, evt->lot.data + evt->lot.size);

    std::string where = program >= 0 ? hdName(program) : "station";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s lot %d \"%s\" %u bytes",
             where.c_str(), isLogo ? "logo" : isArt ? "album art" : "other file",
             pic.lot, name, evt->lot.size);
    g_lastLot = buf;
    LOGI("LOT file: %s (mime %08X)", buf, mime);

    if (pic.bytes.empty()) return;
    if (isLogo) {
        // Only count it as a change if the picture is really different
        // (stations repeat their logo every few minutes).
        Picture &old = program >= 0 ? g_pictures[program].logo : g_stationLogo;
        if (old.bytes != pic.bytes) {
            old = pic;
            g_pictureGen++;
        }
    } else if (isArt && program >= 0) {
        ProgramPictures &pp = g_pictures[program];
        // 11d follow-up (KLOS): its song tags say "no picture" (XHDR param 1) even
        // though album art keeps arriving for the song -> the art was never shown.
        // A station doesn't send a picture for nothing: if the tag said "none" and
        // art arrives (new, or a repeat of one we have), show that one.
        if (pp.wantedArt == ART_WANT_NONE) {
            pp.wantedArt = pic.lot;
            g_pictureGen++;
            LOGI("%s: tag said no picture, but art lot %d arrived - showing it", hdName(program).c_str(), pic.lot);
        }
        for (const Picture &p : pp.recentArt)
            if (p.lot == pic.lot && p.bytes == pic.bytes) return;   // already have it
        pp.recentArt.push_back(pic);
        if (pp.recentArt.size() > MAX_RECENT_ART) pp.recentArt.pop_front();   // forget the oldest
        g_pictureGen++;
    } else {
        g_otherLots++;
    }
}

// 9d step 3: the album art to show for a program right now (nullptr = none).
// Needs g_statusMutex.
static const Picture *chosenArt(int program) {
    const ProgramPictures &pp = g_pictures[program];
    if (pp.wantedArt == ART_WANT_NONE || pp.recentArt.empty()) return nullptr;
    if (pp.wantedArt == ART_WANT_ANY) return &pp.recentArt.back();      // newest
    for (const Picture &p : pp.recentArt)
        if (p.lot == pp.wantedArt) return &p;
    return nullptr;   // the linked picture hasn't arrived yet -> show the logo meanwhile
}

// 9d step 3: the logo for a program: its own, else the station-wide one. Needs g_statusMutex.
static const Picture *chosenLogo(int program) {
    if (!g_pictures[program].logo.bytes.empty()) return &g_pictures[program].logo;
    if (!g_stationLogo.bytes.empty()) return &g_stationLogo;
    return nullptr;
}

// nrsc5 calls this whenever it has news: sync, signal quality, station name,
// song info, audio... It runs on the streaming thread (inside
// nrsc5_pipe_samples_cu8), so we just copy what we need and return quickly.
// Text pointers are only valid during the call, so we copy them to std::string.
static void onNrsc5Event(const nrsc5_event_t *evt, void * /* opaque */) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    DecoderStatus &st = g_status;

    switch (evt->event) {
        case NRSC5_EVENT_SYNC:
            st.synced = true;
            st.syncCount++;
            st.freqOffsetHz = evt->sync.freq_offset;
            st.psmi = evt->sync.psmi;                         // 9d
            if (st.syncMs < 0) st.syncMs = msSinceTune();   // 9d: first sync only
            LOGI("SYNC (freq offset %.1f Hz, psmi %d) %lld ms after tuning",
                 evt->sync.freq_offset, evt->sync.psmi, msSinceTune());
            break;
        case NRSC5_EVENT_LOST_SYNC:
            st.synced = false;
            st.lostSyncAt = Clock::now();                     // 9d: grace period starts
            LOGI("LOST SYNC");
            break;
        case NRSC5_EVENT_MER:
            // 9b: in FM, nrsc5 mirrors the spectrum before decoding (acquire.c,
            // cq15_to_cf_conj), so what it calls "lower" is really the sideband
            // ABOVE the carrier, and "upper" is really below. Swap them back.
            // (AM is not mirrored -> keep as is.)
            if (g_mode == NRSC5_MODE_FM) {
                st.merLower = evt->mer.upper;
                st.merUpper = evt->mer.lower;
            } else {
                st.merLower = evt->mer.lower;
                st.merUpper = evt->mer.upper;
            }
            st.merCount++;          // M10c step 2: tells the gain watchdog "a NEW reading"
            break;
        case NRSC5_EVENT_BER:
            st.ber = evt->ber.cber;
            // 9d: statistics since the tune
            if (st.berCount == 0 || st.ber < st.berMin) st.berMin = st.ber;
            if (st.berCount == 0 || st.ber > st.berMax) st.berMax = st.ber;
            st.berSum += st.ber;
            st.berCount++;
            break;
        case NRSC5_EVENT_HDC:
            // 9d: one packet of compressed audio (before decoding). Used for the
            // bit rate, and to count damaged packets (CRC errors).
            if (evt->hdc.program < MAX_PROGRAMS) {
                ProgramInfo &pi = st.programs[evt->hdc.program];
                pi.hdcBytes += (long long)evt->hdc.count;
                pi.hdcPackets++;
                if (evt->hdc.flags & NRSC5_PKT_FLAGS_CRC_ERROR) pi.crcErrors++;
                if (pi.hdcPackets >= 32) {
                    // Each packet holds 2,048 samples of audio = 2048/44100 s.
                    // bits / seconds / 1000 = kbps
                    double seconds = pi.hdcPackets * (double)NRSC5_AUDIO_FRAME_SAMPLES
                                     / NRSC5_SAMPLE_RATE_AUDIO;
                    pi.kbps = (float)(pi.hdcBytes * 8 / seconds / 1000.0);
                    pi.hdcBytes = 0;
                    pi.hdcPackets = 0;
                }
            }
            break;
        case NRSC5_EVENT_STATION_NAME:
            if (evt->station_name.name && st.stationName != evt->station_name.name) {
                st.stationName = evt->station_name.name;
                LOGI("Station name: %s", st.stationName.c_str());
                { std::lock_guard<std::mutex> lock(g_rdsMutex); g_hdCall = st.stationName; }   // M12 step 2
                if (g_mode == NRSC5_MODE_FM) judgeSameStation();
            }
            break;
        case NRSC5_EVENT_STATION_SLOGAN:
            if (evt->station_slogan.slogan) st.slogan = evt->station_slogan.slogan;
            break;
        case NRSC5_EVENT_AUDIO_SERVICE: {
            // 9b: "program N is on the air, and it's of type X" (from the audio
            // data itself, so it works even when a station sends no SIG table).
            unsigned p = evt->audio_service.program;
            if (p < MAX_PROGRAMS) {
                if (!st.programs[p].onAir) LOGI("Program %s on air", hdName(p).c_str());
                st.programs[p].onAir = true;
                st.programs[p].type = (int)evt->audio_service.type;
                // 14a step 2: the station's TX digital audio gain for this program (see
                // fixHdAudio). Outside the standard's -8 ... +7 dB = not a real value: ignore.
                int gain = evt->audio_service.digital_audio_gain;
                if (gain < -8 || gain > 7) gain = 0;
                if (g_hdGainDb[p].exchange(gain) != gain)
                    LOGI("%s: TX digital audio gain %+d dB (set by the station)", hdName(p).c_str(), gain);
            }
            break;
        }
        case NRSC5_EVENT_SIG:
            // 9b: the Station Information Guide - a list of the station's services.
            // Audio services have a name ("MPS", "SPS1", or sometimes a real name)
            // and an audio component whose "port" is the program number.
            for (nrsc5_sig_service_t *s = evt->sig.services; s != nullptr; s = s->next) {
                if (s->type != NRSC5_SIG_SERVICE_AUDIO || s->audio_component == nullptr) continue;
                unsigned p = s->audio_component->audio.port;
                if (p < MAX_PROGRAMS && s->name != nullptr) {
                    st.programs[p].sigName = s->name;
                    LOGI("SIG: %s = \"%s\"", hdName(p).c_str(), s->name);
                }
            }
            break;
        case NRSC5_EVENT_ID3:
            // 9b: remember the song for EVERY program, so switching shows it at once.
            if (evt->id3.program < MAX_PROGRAMS) {
                ProgramInfo &pi = st.programs[evt->id3.program];
                std::string t = evt->id3.title ? evt->id3.title : "";
                std::string a = evt->id3.artist ? evt->id3.artist : "";
                if (t != pi.title || a != pi.artist) {
                    pi.title = t;
                    pi.artist = a;
                    LOGI("%s now playing: %s - %s",
                         hdName(evt->id3.program).c_str(), a.c_str(), t.c_str());
                }
                // 9d step 3: XHDR = which picture goes with this song.
                // param 0 = show LOT number xhdr.lot; param 1 = no picture;
                // param -1 = this ID3 has no XHDR -> keep what we had.
                if (evt->id3.xhdr.mime == NRSC5_MIME_PRIMARY_IMAGE && evt->id3.xhdr.param >= 0) {
                    int want = (evt->id3.xhdr.param == 0 && evt->id3.xhdr.lot >= 0)
                               ? evt->id3.xhdr.lot : ART_WANT_NONE;
                    ProgramPictures &pp = g_pictures[evt->id3.program];
                    if (pp.wantedArt != want) {
                        pp.wantedArt = want;
                        g_pictureGen++;
                        LOGI("%s XHDR: %s", hdName(evt->id3.program).c_str(),
                             want >= 0 ? ("picture lot " + std::to_string(want)).c_str()
                                       : "no picture");
                    }
                }
            }
            break;
        case NRSC5_EVENT_LOT:
            onLotFile(evt);   // 9d step 3: a logo or album art (or something else)
            break;
        case NRSC5_EVENT_AUDIO:
            // Real decoded audio (16-bit stereo, 44,100 Hz). nrsc5 decodes ALL
            // programs; each event says which one this audio belongs to.
            if (evt->audio.program < MAX_PROGRAMS) {
                // Left/right and polarity put right FIRST, so the aligner, the blender and
                // the player all get the same, corrected audio (see fixHdAudio()).
                const int16_t *hdAudio = fixHdAudio(evt->audio.data, evt->audio.count, evt->audio.program);
                ProgramInfo &pi = st.programs[evt->audio.program];
                if (pi.audioValues == 0)
                    LOGI("First %s audio decoded! %lld ms after tuning",
                         hdName(evt->audio.program).c_str(), msSinceTune());
                if (st.audioMs < 0) st.audioMs = msSinceTune();   // 9d
                pi.onAir = true;
                pi.audioValues += (long long)evt->audio.count;
                // 11c: HD1's audio also goes to the aligner (analog is only ever
                // compared with HD1 - the sub-channels have no analog twin).
                if (evt->audio.program == 0) {
                    long long index = g_align.pushHd(hdAudio, evt->audio.count, timelineFrame(g_iqBlockStart),
                                                     (long long)std::llround(g_iqBlockLen / align::IQ_PER_FRAME));
                    // 11d: ... and to the blender, on the same time line (Auto and Analog only;
                    // in Digital only the demodulator is off, so there is nothing to blend with).
                    if (demodWanted()) g_blend.pushHd(hdAudio, evt->audio.count, index);
                }
                // Milestone 7/9b: keep the selected program's audio for playback
                // (pushAudio ignores the other programs).
                pushAudio((int)evt->audio.program, hdAudio, evt->audio.count);
            }
            break;
        default:
            break;   // lots of other events (images, data...) - ignore for now
    }
}


// ======================= Milestone 6b: auto-gain =======================
// A port of nrsc5's do_auto_gain() (external/nrsc5/src/nrsc5.c). nrsc5 only
// runs it when it controls the dongle itself, which we don't (pipe mode),
// so we do the same thing here, before streaming starts.
//
// Idea: try gains with a "higher or lower?" (binary) search. At each gain,
// grab a short burst of samples and measure how much of the 0..255 range the
// signal uses. Keep the HIGHEST gain whose signal still uses less than half
// the range (below -6 dBFS) - loud enough to rise above the dongle's own
// rounding noise, with room to spare so it never clips.
//
// M10c changes:
//  - The target is now -4 dBFS instead of nrsc5's -6. An 8-bit dongle
//    rounds every sample; the more of the 0..255 range the signal uses, the
//    smaller that rounding noise is compared to the signal -> better MER.
//    (KKGO: nrsc5's rule chose 3.7 dB -> MER 11.7 / 8.2; 8.7 dB with peak
//    -4.5 dBFS gave 14.2 / 11.5.) Kotlin's gain watchdog steps down at once
//    if it ever gets too close to 0 dBFS.
//  - [hintTenths] = the gain that ended up working for this frequency last
//    time (GainMemory.kt), or -1. One quick check at that gain: if the peak
//    is still in a sensible range, use it and skip the search (faster).
//    *usedHint says whether that happened.
//
// Returns the chosen gain (tenths of dB), or -1 on error.
static const float AUTO_TARGET_DBFS = -4.0f;    // search: highest gain whose peak stays below this
static const float HINT_MAX_DBFS = -1.5f;       // remembered gain: louder than this = no (= the watchdog's limit)
static const float HINT_MIN_DBFS = -10.0f;      //   ... quieter than this = signals got weaker, search again

// Sets [gain] and measures the peak (dBFS) of a short burst of samples.
// Returns false if the dongle didn't answer.
static bool measurePeak(std::vector<uint8_t> &buf, int gain, float *peakDb) {
    if (rtlsdr_set_tuner_gain(g_dev, gain) != 0) {
        LOGE("auto-gain: set_tuner_gain(%d) failed", gain);
        return false;   // (nrsc5 would retry forever here; we give up instead)
    }
    int n = 0;
    if (rtlsdr_read_sync(g_dev, buf.data(), (int)buf.size(), &n) != 0 || n <= 0) {
        LOGE("auto-gain: read_sync failed");
        return false;
    }
    // Skip the first quarter (may still be from the previous gain).
    uint8_t minS = 255, maxS = 0;
    for (int j = n / 4; j < n; j++) {
        if (buf[j] > maxS) maxS = buf[j];
        if (buf[j] < minS) minS = buf[j];
    }
    *peakDb = 20.0f * log10f((maxS - minS + 1) / 256.0f);
    LOGI("auto-gain: try %.1f dB -> peak %.1f dBFS", gain / 10.0f, *peakDb);
    return true;
}

static int doAutoGain(float *chosenPeakDbfs, int hintTenths, bool *usedHint) {
    *usedHint = false;
    int count = rtlsdr_get_tuner_gains(g_dev, nullptr);
    if (count <= 0) return -1;
    std::vector<int> gains(count);
    count = rtlsdr_get_tuner_gains(g_dev, gains.data());
    if (count <= 0) return -1;

    rtlsdr_set_tuner_gain_mode(g_dev, 1);              // manual gain
    std::vector<uint8_t> buf(128 * 256);                 // same size nrsc5 uses (~11 ms)
    float db = 0.0f;

    // M10c: first try the remembered gain (if it's one of this tuner's steps).
    if (hintTenths >= 0 && std::find(gains.begin(), gains.end(), hintTenths) != gains.end()) {
        if (!measurePeak(buf, hintTenths, &db)) return -1;
        if (db < HINT_MAX_DBFS && db > HINT_MIN_DBFS) {
            LOGI("auto-gain: remembered %.1f dB is fine (peak %.1f dBFS)", hintTenths / 10.0f, db);
            *chosenPeakDbfs = db;
            *usedHint = true;
            return hintTenths;
        }
        LOGI("auto-gain: remembered %.1f dB doesn't fit now - searching", hintTenths / 10.0f);
    }

    // The search: "higher or lower?" until we've found the highest gain
    // whose peak is still below the target.
    int low = 0, high = count - 1;
    int bestGain = gains[0];
    float bestDb = 0.0f;

    while (low <= high) {
        int mid = (low + high) / 2;
        int gain = gains[mid];
        if (!measurePeak(buf, gain, &db)) return -1;

        if (db < AUTO_TARGET_DBFS) {    // still below the target: try higher
            bestGain = gain;
            bestDb = db;
            low = mid + 1;
        } else {                        // too loud: try lower
            high = mid - 1;
        }
        if (high == -1) {               // even the lowest gain is loud: use it
            bestGain = gain;
            bestDb = db;
        }
    }

    rtlsdr_set_tuner_gain(g_dev, bestGain);
    LOGI("auto-gain: chose %.1f dB (peak %.1f dBFS)", bestGain / 10.0f, bestDb);
    *chosenPeakDbfs = bestDb;
    return bestGain;
}

// ======================= Milestone 4: streaming =======================

// Forget everything the HD decoder told us (a new tune, or 12a step 3: the HD decoder
// switched off in Analog only - no stale "synced", station name, programs or pictures).
// Takes g_statusMutex.
static void forgetHdStatus() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = DecoderStatus();          // forget the previous station
    for (std::atomic<int> &g : g_hdGainDb) g = 0;   // 14a step 2: ... and its TX audio gains
    // 9d step 3: ... and its pictures
    for (ProgramPictures &pp : g_pictures) pp = ProgramPictures();
    g_stationLogo = Picture();
    g_otherLots = 0;
    g_lastLot.clear();
    g_pictureGen++;                      // tells Kotlin "pictures changed"
}

// Milestone 6: create the nrsc5 decoder in "pipe" mode (we feed it samples).
// 12a step 3: also when the HD is switched back on in the middle of a stream (on the
// streaming thread then - the only thread that feeds nrsc5). False = it failed.
static bool openHd() {
    if (nrsc5_open_pipe(&g_nrsc5) != 0 || g_nrsc5 == nullptr) {
        g_nrsc5 = nullptr;
        LOGE("nrsc5_open_pipe failed");
        return false;
    }
    nrsc5_set_mode(g_nrsc5, g_mode);         // FM, or AM (9c)
    nrsc5_set_callback(g_nrsc5, onNrsc5Event, nullptr);
    g_hdOn = true;
    return true;
}

// librtlsdr calls this every time a block of samples arrives
// (about 6 times per second, 262,144 bytes each). It runs on the
// streaming thread, so it must be quick.
//
// Each sample is 2 bytes: I and Q, each 0..255 with "zero" at 127.5.
static void onSamples(unsigned char *buf, uint32_t len, void * /* ctx */) {
    g_bytes += len;

    // Signal level: RMS distance from the center value, 0 (silence) .. ~128.
    // Just a sanity check that the data is real radio noise/signal, not zeros.
    double sumSquares = 0;
    uint8_t minS = 255, maxS = 0;
    for (uint32_t i = 0; i < len; i++) {
        float v = buf[i] - 127.5f;
        sumSquares += v * v;
        if (buf[i] < minS) minS = buf[i];
        if (buf[i] > maxS) maxS = buf[i];
    }
    if (len > 0) {
        g_level = sqrtf((float)(sumSquares / len));
        // Same measurement nrsc5's auto-gain uses: 0 dBFS = using the full 0..255 range.
        float peak = 20.0f * log10f((maxS - minS + 1) / 256.0f);
        g_peakDbfs = peak;
        // M10c: remember the loudest block for the gain watchdog. (Only this
        // thread raises it; the watchdog resets it - if both happen at the same
        // moment, one block's value is lost, which doesn't matter.)
        if (peak > g_peakMax.load()) g_peakMax = peak;
    }

    // Milestone 6: hand the samples to nrsc5. It decodes right here, on this
    // thread (nrsc5's own rtl-sdr mode does exactly the same).
    g_iqBlockStart = g_iqPos.load();          // 11c: where this block sits in the IQ stream
    g_iqBlockLen = len / 2;
    // 12a step 3: Analog only -> the HD decoder is off; switched back -> a fresh one (it finds
    // the HD again in ~1-2 s, like a tune; the aligner anchors the new HD audio afresh).
    if (hdWanted()) {
        if (g_nrsc5 == nullptr && !g_hdOpenFailed) {
            if (openHd()) LOGI("HD decoder on");
            else g_hdOpenFailed = true;
        }
    } else if (g_nrsc5 != nullptr) {
        nrsc5_close(g_nrsc5);
        g_nrsc5 = nullptr;
        g_hdOn = false;
        forgetHdStatus();
        g_hdLoadPct = 0;
        g_hdLoad.reset();
        LOGI("HD decoder off (Analog only)");
    }
    // Build 9: does nrsc5 get this block, or is its search resting (hdsearch.hpp)?
    // "In sync" = nrsc5 says so now, or it reported a sync since the last block (one that
    // came and went inside a single block counts too).
    bool hdSynced;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        hdSynced = g_status.synced || g_status.syncCount != g_hdSyncsSeen;
        g_hdSyncsSeen = g_status.syncCount;
    }
    const bool restApplies = g_nrsc5 != nullptr && g_mode == NRSC5_MODE_FM && g_audioSource.load() == SOURCE_AUTO
                             && g_program.load() == 0 && g_hdRestAllowed.load();
    const double blockAtS = (double)g_iqBlockStart / SAMPLE_RATE;      // seconds of samples since the tune
    const bool feedHd = g_hdSearch.step(blockAtS, restApplies, hdSynced);
    const int hdRest = g_hdSearch.state();
    g_hdRest = hdRest;
    g_hdLookInS = (int)std::ceil(g_hdSearch.lookIn(blockAtS));
    if (feedHd && !g_hdFed && g_nrsc5 != nullptr) {
        // The samples start again after a rest: with a fresh decoder (see g_hdSearch above).
        // What the old one told us stays on the screen - the station's HD name, its
        // programs, its logo.
        nrsc5_close(g_nrsc5);
        g_nrsc5 = nullptr;
        if (!openHd()) { g_hdOpenFailed = true; g_hdOn = false; }
    }
    g_hdFed = feedHd;
    if (hdRest != g_hdRestWas) {
        // Into the first rest, and out of resting for good - not every look (one each 30 s).
        if (hdRest == hdsearch::RESTING && g_hdRestWas == hdsearch::SEARCHING)
            LOGI("HD search rests: no HD for %.0f s - from now a %.0f s look every %.0f s",
                 hdsearch::SEARCH_S, hdsearch::LOOK_S, hdsearch::REST_S + hdsearch::LOOK_S);
        else if (hdRest == hdsearch::SEARCHING)
            LOGI("HD search back on (%s)", hdSynced ? "a look found HD" : "the setting or the program changed");
        g_hdRestWas = hdRest;
    }
    if (g_nrsc5 != nullptr) {
        // 11d: time the HD decode too ("decode N %" next to "demod M %" in the details).
        // (Build 9: a block nrsc5 does not get counts as no time used, so the figure falls
        // to 0 % while the search rests.)
        Clock::time_point t0 = Clock::now();
        if (feedHd) nrsc5_pipe_samples_cu8(g_nrsc5, buf, len);
        double blockMs = len / 2.0 / SAMPLE_RATE * 1000.0;
        double usedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (blockMs > 0) g_hdLoad.add(usedMs, blockMs, g_hdLoadPct);   // 9i: 1 s average
    }
    g_iqPos += len / 2;

    // M11: analog FM too (FM band only). 11c: also in Digital only, for the aligner.
    // 11d: no longer - Digital only means digital only (the demodulator, the aligner
    // and the blend are off, saving 10-20 % of a core); Auto and Analog only run it.
    if (g_mode == NRSC5_MODE_FM && demodWanted()) {
        demodAnalog(buf, len);
    } else if (g_mode == NRSC5_MODE_FM) {
        // Build 6: Digital only - no analog audio, the clock correction's resampler stands
        // still. The dongle's clock keeps drifting from the station's all the same, and the
        // HD audio keeps being anchored on the time line: count the frames the resampler
        // WOULD have taken out, so the time line still fits when the analog comes back.
        g_idleSlip += g_drift.clock() * (double)(len / 2) / align::IQ_PER_FRAME;
        g_slip = g_stretch.slip() + g_idleSlip;
    }
}

// Body of the streaming thread. rtlsdr_read_async() only returns when
// streaming is cancelled (by us, or because the dongle was unplugged).
static void streamThreadMain() {
    LOGI("streaming thread started");
    int r = rtlsdr_read_async(g_dev, onSamples, nullptr, 0, 0);  // 0,0 = default buffers
    LOGI("rtlsdr_read_async returned %d", r);
    g_streaming = false;
}

// Kotlin (RadioEngine): external fun startStreamNative(freqHz: Int, gainTenthsDb: Int, hintTenthsDb: Int): String
// gainTenthsDb: tuner gain in tenths of a dB (0 = 0.0 dB, 496 = 49.6 dB),
//               or negative = automatic (nrsc5-style auto-gain, doAutoGain()).
// hintTenthsDb (M10c): automatic only - the gain remembered for this
//               frequency (try it first), or -1 = none.
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_startStreamNative(
        JNIEnv* env,
        jobject /* this */,
        jint freqHz,
        jint gainTenthsDb,
        jint hintTenthsDb) {
    if (g_dev == nullptr) return env->NewStringUTF("Stream: dongle not open");
    if (g_streaming)      return env->NewStringUTF("Stream: already running");

    // 9d: the stopwatch for "time to sync" / "time to audio" starts here,
    // so it includes auto-gain (the listener waits for that too).
    g_tuneTicks = (long long)Clock::now().time_since_epoch().count();

    int r = rtlsdr_set_sample_rate(g_dev, SAMPLE_RATE);
    if (r < 0) {
        LOGE("rtlsdr_set_sample_rate failed: %d", r);
        return env->NewStringUTF("Stream: set_sample_rate FAILED");
    }
    rtlsdr_set_center_freq(g_dev, (uint32_t)freqHz);
    rtlsdr_reset_buffer(g_dev);             // throw away stale data

    // 14c: did the driver just switch to direct sampling (see g_direct)? Ask it AFTER
    // tuning - it decides per frequency. Then every gain call is skipped: they would only
    // write to a tuner that is switched off (and leave the dongle's I2C line to the tuner
    // open while streaming, which only adds noise).
    const bool direct = rtlsdr_get_direct_sampling(g_dev) > 0;
    if (direct != g_direct.load())
        LOGI("%s", direct ? "Direct sampling ON (tuner bypassed below 24 MHz) - no tuner gain"
                          : "Direct sampling off - the tuner is back in use");
    g_direct = direct;

    std::string gainText;
    char gbuf[96];
    if (direct) {
        gainText = "no tuner gain (direct sampling)";
        g_gainTenths = -1;                                // nothing to show, nothing to watch
        g_gainAuto = gainTenthsDb < 0;                    // (what the setting says; unused)
        g_gainRemembered = false;
        g_gainSearchMs = -1;
    } else if (gainTenthsDb >= 0) {
        rtlsdr_set_tuner_gain_mode(g_dev, 1);             // 1 = manual gain
        rtlsdr_set_tuner_gain(g_dev, gainTenthsDb);       // picks the nearest supported step
        int actual = rtlsdr_get_tuner_gain(g_dev);
        snprintf(gbuf, sizeof(gbuf), "gain %.1f dB (manual)", actual / 10.0f);
        gainText = gbuf;
        g_gainTenths = actual;                            // 9d
        g_gainAuto = false;
        g_gainRemembered = false;                         // M10c
        g_gainSearchMs = -1;
    } else {
        float peak = 0.0f;
        bool usedHint = false;
        Clock::time_point searchStart = Clock::now();     // M10c: time just the gain part
        int chosen = doAutoGain(&peak, hintTenthsDb, &usedHint);   // takes well under a second
        if (chosen < 0) {
            rtlsdr_set_tuner_gain_mode(g_dev, 0);         // fall back to tuner AGC
            gainText = "gain auto FAILED - using tuner AGC";
        } else {
            snprintf(gbuf, sizeof(gbuf), "gain %.1f dB (auto%s, peak %.1f dBFS)",
                     chosen / 10.0f, usedHint ? ", remembered" : "", peak);
            gainText = gbuf;
        }
        g_gainTenths = chosen;                            // 9d (-1 = tuner AGC)
        g_gainAuto = true;
        g_gainRemembered = usedHint;                      // M10c
        g_gainSearchMs = (int)msSince(searchStart);
        rtlsdr_reset_buffer(g_dev);                       // discard auto-gain leftovers
        LOGI("auto-gain took %d ms (%lld ms since tuning)", g_gainSearchMs.load(), msSinceTune());
    }
    g_peakMax = -99.0f;                                   // M10c: the watchdog starts fresh

    forgetHdStatus();                        // the previous station's HD status and pictures
    g_program = 0;                           // 9b: a new station starts on HD1
    // 9c: AM below 30 MHz (the AM band is 0.53-1.7 MHz; the V4's driver switches its
    // HF upconverter in by itself below 28.8 MHz; a V3 goes to direct sampling instead,
    // see g_direct). AM = HD only: no analog demodulator,
    // no aligner, no blend - whatever the "HD Radio" setting says (Kotlin sends
    // Digital only for AM too; this is the safety net). The stream thread isn't running
    // yet, so plain writes are fine here.
    g_mode = freqHz < AM_BELOW_HZ ? NRSC5_MODE_AM : NRSC5_MODE_FM;
    if (g_mode == NRSC5_MODE_AM) g_audioSource = SOURCE_HD;
    clearAudio();                            // and any old audio
    g_fm.reset();                            // M11: analog demodulator starts fresh (no old station in its filters)
    g_fmCarrierDb = -99.0f;
    g_fmOffsetHz = 0.0f;
    g_fmQuietDb = -99.0f;                    // 9k
    g_fmPilot = 0; g_fmPilotLevel = 0.0f; g_fmStereo = 0.0f;   // 11b
    {   // M12: no RDS of the old station; step 2: no "same station?" verdict either
        std::lock_guard<std::mutex> lock(g_rdsMutex);
        g_rds = rds::Status(); g_hdCall.clear(); g_sameCall.clear();
        g_rdsCallSure = false; g_rdsTextCall.clear(); g_rdsTextPi = 0; g_rdsPiSinceMs = -1; g_rdsPiSeen = 0;   // 14b step 4
    }
    g_sameStation = blend::SAME_UNKNOWN;
    g_blend.setSameStation(blend::SAME_UNKNOWN);
    g_rdsSerial = g_fm.rds().serial();
    g_rdsCallHeld = false;                   // 14b step 4
    g_fmLoadPct = 0;
    g_fmLoad.reset();                        // 9i
    g_align.reset();                         // 11c: alignment starts fresh too
    g_blend.reset();                         // 11d: and the blend (analog first, HD when it's good)
    g_stretch.reset();                       // build 6: and the clock correction (the crystal's error is learnt anew)
    g_drift.reset();
    g_driftRate = 0; g_idleSlip = 0; g_slip = 0.0;
    g_tunedHz = (int)freqHz;
    g_hdLoadPct = 0;
    g_hdLoad.reset();                        // 9i
    g_iqPos = 0;
    // 12a step 3: the HD decoder only if it can be heard (not in Analog only); onSamples
    // opens / closes it later when the setting changes.
    g_hdOpenFailed = false;
    g_hdOn = false;
    g_hdSearch.reset();                      // build 9: a new station gets its full 30 s of HD search
    g_hdSyncsSeen = 0; g_hdRestWas = hdsearch::SEARCHING; g_hdFed = true;
    g_hdRest = hdsearch::SEARCHING; g_hdLookInS = 0;
    if (hdWanted() && !openHd()) return env->NewStringUTF("Stream: nrsc5_open_pipe FAILED");

    uint32_t actualRate = rtlsdr_get_sample_rate(g_dev);
    uint32_t actualFreq = rtlsdr_get_center_freq(g_dev);
    LOGI("Streaming: rate=%u Hz, freq=%u Hz", actualRate, actualFreq);

    g_bytes = 0;
    g_level = 0.0f;
    g_streaming = true;
    g_streamThread = std::thread(streamThreadMain);

    std::string msg = "Tuned to " + std::to_string(actualFreq / 1000) + " kHz, "
                    + gainText + ", rate " + std::to_string(actualRate) + " samples/s";
    return env->NewStringUTF(msg.c_str());
}

// Stops streaming and WAITS for the thread to finish.
static void stopStreaming() {
    if (g_streamThread.joinable()) {
        if (g_streaming && g_dev != nullptr) {
            LOGI("rtlsdr_cancel_async()");
            rtlsdr_cancel_async(g_dev);
        }
        g_streamThread.join();   // wait until rtlsdr_read_async() has returned
        LOGI("streaming thread stopped");
    }
    g_streaming = false;

    // Streaming has stopped, so nobody is feeding nrsc5 any more -> safe to close.
    if (g_nrsc5 != nullptr) {
        nrsc5_close(g_nrsc5);
        g_nrsc5 = nullptr;
        LOGI("nrsc5_close()");
    }
    g_hdOn = false;
    g_hdRest = hdsearch::SEARCHING; g_hdLookInS = 0;      // build 9: nothing rests while nothing plays
}

// Kotlin (RadioEngine): external fun stopStreamNative()
// Call before closeDongleNative() (closeDongleNative also does it, as a safety net).
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_stopStreamNative(
        JNIEnv* /* env */,
        jobject /* this */) {
    stopStreaming();
}

// Kotlin polls these once per second (the "pull" model - C never calls Kotlin).
extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_isDongleOpenNative(JNIEnv*, jobject) {
    return g_dev != nullptr ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getStreamBytesNative(JNIEnv*, jobject) {
    return (jlong)g_bytes.load();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_isStreamingNative(JNIEnv*, jobject) {
    return g_streaming ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getSignalLevelNative(JNIEnv*, jobject) {
    return g_level.load();
}

// Kotlin (RadioEngine): external fun getStatusNative(): String
// A readable summary of what nrsc5 has decoded so far (polled once per second).
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getStatusNative(JNIEnv* env, jobject) {
    DecoderStatus st;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        st = g_status;   // copy, so we hold the lock only briefly
    }
    const int selected = g_program;
    char buf[512];
    std::string out;
    // 9d: gain on the same line as the peak (they belong together).
    if (g_direct) snprintf(buf, sizeof(buf), "Peak: %.1f dBFS   Gain: none (direct sampling)\n",
                           g_peakDbfs.load());                                  // 14c
    else if (g_gainTenths < 0) snprintf(buf, sizeof(buf), "Peak: %.1f dBFS   Gain: tuner AGC\n",
                                   g_peakDbfs.load());
    else snprintf(buf, sizeof(buf), "Peak: %.1f dBFS   Gain: %.1f dB (%s)\n",
                  g_peakDbfs.load(), g_gainTenths.load() / 10.0f, g_gainAuto.load() ? "auto" : "manual");
    out += buf;
    if (st.syncCount == 0) {
        // 9d: "+=" (it used to be "=", which hid the Peak line while searching)
        snprintf(buf, sizeof(buf), "HD: searching... (%.1f s)", msSinceTune() / 1000.0);
        out += buf;
    } else {
        // 9d: sync state with the grace period, plus the service mode (e.g. MP3).
        std::string mode = modeName(st.psmi);
        std::string state;
        if (st.synced) {
            state = "SYNCED";
        } else if (syncShown(st)) {
            snprintf(buf, sizeof(buf), "SYNCED, holding - lost %.1f s ago",
                     msSince(st.lostSyncAt) / 1000.0);
            state = buf;
        } else {
            state = "lost sync";
        }
        snprintf(buf, sizeof(buf), "HD: %s (%s%soffset %.0f Hz, synced %d time%s)\n",
                 state.c_str(), mode.c_str(), mode.empty() ? "" : ", ",
                 st.freqOffsetHz, st.syncCount, st.syncCount == 1 ? "" : "s");
        out += buf;

        // 9d: how long this tune took (answers "are stations slower to start?")
        out += "Time to sync: ";
        snprintf(buf, sizeof(buf), "%.2f s", st.syncMs / 1000.0);
        out += buf;
        out += "   to audio: ";
        if (st.audioMs >= 0) { snprintf(buf, sizeof(buf), "%.2f s", st.audioMs / 1000.0); out += buf; }
        else out += "-";
        out += "\n";

        snprintf(buf, sizeof(buf), "MER: %.1f / %.1f dB (lower / upper)\n",
                 st.merLower, st.merUpper);
        out += buf;
        // 9d: BER now / average / best / worst since the tune (like nrsc5-dui)
        if (st.berCount > 0) {
            snprintf(buf, sizeof(buf), "BER: %.4f%%  avg %.4f%%  min %.4f%%  max %.4f%%",
                     st.ber * 100.0f, st.berSum / st.berCount * 100.0,
                     st.berMin * 100.0f, st.berMax * 100.0f);
            out += buf;
        } else {
            out += "BER: -";
        }
        // 9d: bit rate + damaged packets of the program we're playing
        const ProgramInfo &sp = st.programs[selected];
        if (sp.kbps > 0) {
            snprintf(buf, sizeof(buf), "\n%s audio: %.1f kbps, CRC errors %lld",
                     hdName(selected).c_str(), sp.kbps, sp.crcErrors);
            out += buf;
        }
    }
    if (!st.stationName.empty()) out += "\nStation: " + st.stationName;
    if (!st.slogan.empty())      out += "\nSlogan: " + st.slogan;

    // 9b: one line per program on the air, e.g.
    //   ▶ HD1  Adult Hits  "MPS"  12.3 s  (▶ = the one playing)
    bool any = false;
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        const ProgramInfo &pi = st.programs[i];
        if (!pi.onAir) continue;
        if (!any) { out += "\nPrograms:"; any = true; }
        out += std::string("\n ") + (i == selected ? "▶ " : "   ") + hdName(i);
        if (pi.type >= 0)        out += "  " + typeName(pi.type);
        if (!pi.sigName.empty()) out += "  \"" + pi.sigName + "\"";
        // stereo 16-bit at 44,100 Hz = 88,200 values per second of audio
        snprintf(buf, sizeof(buf), "  %.1f s", pi.audioValues / 88200.0);
        out += buf;
        if (pi.kbps > 0) { snprintf(buf, sizeof(buf), "  %.0f kbps", pi.kbps); out += buf; }   // 9d
    }

    // Song on the program that's playing (debug screen only - since 9b step 2
    // the notification gets the song from getProgramsNative instead).
    const ProgramInfo &sel = st.programs[selected];
    if (!sel.title.empty() || !sel.artist.empty()) {
        // "artist - title", or just one of them if the other is empty
        std::string song = sel.artist.empty() ? sel.title
                         : sel.title.empty()  ? sel.artist
                         : sel.artist + " - " + sel.title;
        out += "\n" + hdName(selected) + " now playing: " + song;
    }

    // 9d step 3: pictures, e.g. "Pictures: HD1 logo 12.3 KB, art 35.0 KB (lot 1234)"
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        const Picture *logo = chosenLogo(selected);
        const Picture *art = chosenArt(selected);
        const ProgramPictures &pp = g_pictures[selected];
        out += "\nPictures: " + hdName(selected) + " logo ";
        if (logo) { snprintf(buf, sizeof(buf), "%.1f KB", logo->bytes.size() / 1024.0); out += buf; }
        else out += "none";
        out += ", art ";
        if (art) {
            snprintf(buf, sizeof(buf), "%.1f KB (lot %d)", art->bytes.size() / 1024.0, art->lot);
            out += buf;
        } else {
            out += "none";
        }
        out += pp.wantedArt == ART_WANT_NONE ? ", XHDR: no picture"
             : pp.wantedArt >= 0 ? ", XHDR: lot " + std::to_string(pp.wantedArt) : "";
        snprintf(buf, sizeof(buf), ", %zu album art kept, %d other files",
                 pp.recentArt.size(), g_otherLots);
        out += buf;
        if (!g_lastLot.empty()) out += "\nLast LOT: " + g_lastLot;
    }
    return toJString(env, out);   // 9b: safe for any station text
}

// ======================= Milestone 9b: programs (HD1..HD8) =======================

// Kotlin (RadioEngine): external fun setProgramNative(program: Int)
// Chooses which program is played (0 = HD1 ... 7 = HD8).
// 9b step 3: the AudioPlayer keeps running. The old program's audio that is
// still waiting in the ring plays to the end, fading out over its last 20 ms;
// the new program's audio is added right after it, fading in (see pushAudio).
// No gap, no click.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setProgramNative(
        JNIEnv* /* env */, jobject /* this */, jint program) {
    if (program < 0 || program >= MAX_PROGRAMS) return;
    std::lock_guard<std::mutex> lock(g_audioMutex);   // same lock pushAudio checks under
    if (program == g_program) return;

    // Fade out the last (up to) 20 ms of the old program that haven't been
    // played yet. They sit just before the write position; "& ~1" keeps an even
    // number (whole left+right pairs).
    size_t n = std::min(FADE_VALUES, g_ringCount) & ~(size_t)1;
    size_t frames = n / 2;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (g_ringWrite + AUDIO_RING_VALUES - n + i) % AUDIO_RING_VALUES;
        size_t frame = i / 2;                          // 0 = oldest of the tail
        float gain = 1.0f - (float)(frame + 1) / frames;   // ... down to 0 at the end
        g_ring[idx] = (int16_t)(g_ring[idx] * gain);
    }

    g_program = program;
    g_fadeInLeft = FADE_VALUES;                       // pushAudio fades the new one in
    // 11d: in Auto, HD1 comes from the blender. Back to HD1 -> it starts afresh
    // (its play position is stale) and fades in; off to HD2+ -> the HD ring is
    // empty (pushAudio skipped HD1) and fills with the new program at once.
    if (g_audioSource.load() == SOURCE_AUTO) {
        if (program == 0) { g_blend.restart(); g_sourceFadeLeft = FADE_VALUES; }
        else g_ringRead = g_ringWrite = g_ringCount = 0;
    }
    LOGI("Selected program %s (faded %zu values)", hdName(program).c_str(), n);
}

// Kotlin (RadioEngine): external fun getProgramNative(): Int - the program being played
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getProgramNative(JNIEnv*, jobject) {
    return g_program.load();
}

// Kotlin (RadioEngine): external fun getProgramsNative(): String
// The programs on the air, for building the HD1/HD2/... buttons. One line per
// program, fields separated by TAB characters:
//   number  type name  SIG name  artist  title
// e.g. "1\tTop 40\tSPS1\tDua Lipa\tHoudini". Empty text = nothing found yet.
// (Tabs and new-lines can't appear inside the fields - we replace them with spaces.)
static std::string clean(const std::string &s) {
    std::string r = s;
    for (char &c : r) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return r;
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getProgramsNative(JNIEnv* env, jobject) {
    std::string out;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        for (int i = 0; i < MAX_PROGRAMS; i++) {
            const ProgramInfo &pi = g_status.programs[i];
            if (!pi.onAir) continue;
            if (!out.empty()) out += "\n";
            out += std::to_string(i) + "\t" + clean(typeName(pi.type)) + "\t" + clean(pi.sigName)
                 + "\t" + clean(pi.artist) + "\t" + clean(pi.title);
        }
    }
    return toJString(env, out);
}

// ======================= Milestone 9d: signal details =======================

// Kotlin (RadioEngine): external fun getSignalNative(): String
// Everything the now-playing screen needs about the signal, as "key=value"
// lines (easy to read in Kotlin, and new keys can be added later without
// breaking anything). Numbers use a dot, e.g. "merLower=12.3".
//   synced      1/0  - what to SHOW (includes the 1.5 s grace period)
//   locked      1/0  - the real, raw sync state
//   everSynced  1/0  - synced at least once since the tune
//   mode        e.g. "MP3" (empty = not known yet)
//   offsetHz, merLower, merUpper (dB, true sides), ber/berAvg/berMin/berMax (0..1, -1 = none yet)
//   merCount         - MER readings since tuning (M10c step 2)
//   kbps, crcErrors  - of the program being played (kbps 0 = not measured yet)
//   gainDb (-1 = tuner AGC), gainAuto 1/0, peakDbfs
//   gainRemembered 1/0, gainSearchMs (M10c: how auto-gain found its start)
//   syncMs, audioMs  - ms from tuning to first sync / first audio (-1 = not yet)
//   tunedMs          - ms since tuning
//   station, slogan  - text from the station (may be empty)
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getSignalNative(JNIEnv* env, jobject) {
    DecoderStatus st;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        st = g_status;
    }
    const ProgramInfo &sp = st.programs[g_program.load()];
    char buf[768];
    snprintf(buf, sizeof(buf),
             "synced=%d\nlocked=%d\neverSynced=%d\nmode=%s\noffsetHz=%.0f\n"
             "merLower=%.1f\nmerUpper=%.1f\nmerCount=%d\n"
             "ber=%.6f\nberAvg=%.6f\nberMin=%.6f\nberMax=%.6f\n"
             "kbps=%.1f\ncrcErrors=%lld\n"
             "gainDb=%.1f\ngainAuto=%d\npeakDbfs=%.1f\n"
             "gainRemembered=%d\ngainSearchMs=%d\n"
             "syncMs=%lld\naudioMs=%lld\ntunedMs=%lld\n",
             syncShown(st) ? 1 : 0, st.synced ? 1 : 0, st.syncCount > 0 ? 1 : 0,
             modeName(st.psmi).c_str(), st.freqOffsetHz,
             st.merLower, st.merUpper, st.merCount,
             st.berCount > 0 ? st.ber : -1.0f,
             st.berCount > 0 ? st.berSum / st.berCount : -1.0,
             st.berCount > 0 ? st.berMin : -1.0f,
             st.berCount > 0 ? st.berMax : -1.0f,
             sp.kbps, sp.crcErrors,
             g_gainTenths.load() < 0 ? -1.0f : g_gainTenths.load() / 10.0f, g_gainAuto.load() ? 1 : 0, g_peakDbfs.load(),
             g_gainRemembered.load() ? 1 : 0, g_gainSearchMs.load(),
             st.syncMs, st.audioMs, msSinceTune());
    std::string out = buf;
    // M11: analog FM. 11d: audioSource = hd / analog / auto; demodOn = the
    // demodulator runs (Analog only, Auto); hdLoadPct = nrsc5's own decode time.
    int src = g_audioSource.load();
    snprintf(buf, sizeof(buf), "am=%d\ndirect=%d\naudioSource=%s\ndemodOn=%d\nfmCarrierDb=%.1f\nfmQuietDb=%.1f\nfmOffsetHz=%.0f\nfmLoadPct=%d\nhdLoadPct=%d\n",
             g_mode == NRSC5_MODE_AM ? 1 : 0,          // 9c
             g_direct.load() ? 1 : 0,                  // 14c: direct sampling (no tuner gain)
             src == SOURCE_ANALOG ? "analog" : src == SOURCE_AUTO ? "auto" : "hd", demodWanted() ? 1 : 0,
             g_fmCarrierDb.load(), g_fmQuietDb.load(), g_fmOffsetHz.load(), g_fmLoadPct.load(), g_hdLoadPct.load());
    out += buf;
    // 11b: stereo. fmStereoMode = the setting (auto / mono / stereo); fmPilot = a 19 kHz
    // pilot is there; fmPilotPct = its level in % of 75 kHz (stations send 8-10);
    // fmStereoPct = the stereo amount playing (100 = full stereo, 0 = mono).
    {
        int sm = g_stereoMode.load();
        snprintf(buf, sizeof(buf), "fmStereoMode=%s\nfmPilot=%d\nfmPilotPct=%.1f\nfmStereoPct=%d\n",
                 sm == fm::STEREO_MONO ? "mono" : sm == fm::STEREO_FULL ? "stereo" : "auto",
                 g_fmPilot.load(), g_fmPilotLevel.load() * 100.0f, (int)std::lround(g_fmStereo.load() * 100.0f));
        out += buf;
    }
    // 11d: the blend (Auto only; see blend.hpp). blendState: analog / toHd / hd /
    // toAnalog, or "sub" while a sub-channel (HD2+) plays live, "off" outside Auto.
    {
        blend::Status b = g_blend.status();
        const char *state = src != SOURCE_AUTO ? "off" : g_program.load() != 0 ? "sub"
                          : b.state == blend::HD ? "hd" : b.state == blend::TO_HD ? "toHd"
                          : b.state == blend::TO_ANALOG ? "toAnalog" : "analog";
        snprintf(buf, sizeof(buf),
                 "blendState=%s\nblendAligned=%d\nblendMismatch=%d\nblendToHd=%d\nblendToAnalog=%d\n"
                 "blendHdSecs=%.0f\nblendSinceSecs=%.0f\nblendMatchDb=%.1f\nblendReason=%s\n",
                 state, b.aligned ? 1 : 0, b.mismatch ? 1 : 0, b.toHd, b.toAnalog,
                 b.hdFrames / 44100.0, b.hdSince / 44100.0, b.matchDb, b.reason.c_str());
        out += buf;
    }
    // 11c: alignment (see aligner.hpp). alignMs = how far the analog lags HD1 in
    // real time; alignFrames = the same as a difference of frame indices (for 11d).
    {
        align::Status a = g_align.status();
        const char *state = a.state == 2 ? "ok" : a.state == 3 ? "nomatch" : a.state == 1 ? "measuring" : "waiting";
        snprintf(buf, sizeof(buf),
                 "alignState=%s\nalignMs=%.1f\nalignFrames=%lld\nalignCorr=%.3f\nalignGainDb=%.1f\n"
                 "alignCount=%d\nalignTries=%d\nalignJumps=%d\n",
                 state, a.dMs, a.offsetFrames, a.corr, a.gainDb, a.accepted, a.attempts, a.jumps);
        out += buf;
    }
    // Build 6: the clock correction (drift.hpp). clockState: off (nothing known, or the
    // demodulator is off) / guess (from the tuning error) / measured (from the audio);
    // clockPpm = the dongle's clock error as best known (> 0 = fast), clockUsePpm = the
    // correction applied right now (the same plus a little steering), clockGuessPpm = what
    // the tuning error alone says, clockErr = how far the analog is from its place (frames).
    // Build 8: clockUnused = which of the two witnesses was NOT believed, if any - "audio"
    // (its figure is too far from the tuning error's and it has no strong case) or "tuning"
    // (the audio proved the tuning error wrong) - and clockUnusedPpm = what that one says;
    // clockScatter = how much more this station's measurements scatter than usual (1 = as usual).
    {
        drift::Status d = g_drift.status();
        bool on = g_mode == NRSC5_MODE_FM && demodWanted();
        snprintf(buf, sizeof(buf), "clockState=%s\nclockPpm=%.1f\nclockUsePpm=%.1f\nclockGuessPpm=%.1f\nclockPoints=%d\nclockErr=%.1f\n"
                 "clockUnused=%s\nclockUnusedPpm=%.1f\nclockScatter=%.1f\n",
                 !on || d.state == 0 ? "off" : d.state == 2 ? "measured" : "guess",
                 on ? d.clockPpm : 0.0, on ? d.ppm : 0.0, on && d.haveGuess ? d.guessPpm : 0.0, on ? d.points : 0, on ? d.errorFrames : 0.0,
                 !on || d.state == 0 || d.unused == 0 ? "" : d.unused == 1 ? "audio" : "tuning",
                 on && d.state != 0 && d.unused != 0 ? d.unusedPpm : 0.0, on ? d.scatter : 1.0);
        out += buf;
    }
    // M12: RDS (analog FM only; all empty / 0 while the demodulator is off). rdsSync = the
    // blocks line up; rdsBler = % of blocks with errors (last ~1.5 s); rdsPi = the station's
    // code in hex ("" until confirmed); rdsCall = call letters worked out from it (North
    // America; "" if unknown); rdsPs = the 8-character name / scrolling text, rdsPsSecs =
    // how long it has stayed the same; rdsRt = RadioText; rdsTitle / rdsArtist = RT+ tags;
    // rdsPty = program type number (-1 unknown); rdsGroups = groups received.
    {
        rds::Status r;
        if (g_mode == NRSC5_MODE_FM && demodWanted()) { std::lock_guard<std::mutex> lock(g_rdsMutex); r = g_rds; }
        char pi[8] = "";
        if (r.piOk) snprintf(pi, sizeof(pi), "%04X", r.pi);
        snprintf(buf, sizeof(buf), "rdsSync=%d\nrdsBler=%d\nrdsPi=%s\nrdsPsSecs=%d\nrdsPty=%d\nrdsGroups=%ld\n",
                 r.synced ? 1 : 0, r.blerPct, pi, (int)r.psSecs, r.pty, r.groups);
        out += buf;
        out += "rdsCall=" + clean(r.call) + "\n";
        out += "rdsPs=" + clean(r.ps) + "\n";
        out += "rdsRt=" + clean(r.rt) + "\n";
        out += "rdsTitle=" + clean(r.title) + "\n";
        out += "rdsArtist=" + clean(r.artist) + "\n";
    }
    // M12 step 2: is the HD the same station as the analog? 0 = can't tell, 1 = yes (RDS PI
    // = the HD call sign), 2 = no (another station's HD). hdSameCall = the analog's call
    // letters (kept until the next tune, also while the demodulator is off).
    {
        std::string sameCall, hdName;     // hdName: the HD's own name, kept even while the HD is off
        { std::lock_guard<std::mutex> lock(g_rdsMutex); sameCall = g_sameCall; hdName = g_hdCall; }
        out += "hdSame=" + std::to_string(g_mode == NRSC5_MODE_FM ? g_sameStation.load() : 0) + "\n";
        out += "hdSameCall=" + clean(sameCall) + "\n";
        out += "hdOtherName=" + clean(hdName) + "\n";
    }
    out += std::string("hdOn=") + (g_hdOn.load() ? "1" : "0") + "\n";   // 12a step 3: off in Analog only
    {   // Build 9: the HD search on a station without HD (hdsearch.hpp): on / rest / look,
        // and while it rests the seconds until the next look.
        int rest = g_hdRest.load();
        out += std::string("hdSearch=") + (rest == hdsearch::RESTING ? "rest" : rest == hdsearch::LOOKING ? "look" : "on") + "\n";
        out += "hdLookIn=" + std::to_string(g_hdLookInS.load()) + "\n";
    }
    out += "station=" + clean(st.stationName) + "\n";   // clean(): no new-lines inside a value
    out += "slogan=" + clean(st.slogan);
    return toJString(env, out);                         // station text -> must use toJString
}

// ======================= Milestone 9e step 2: manual gain =======================

// Kotlin (RadioEngine): external fun setGainNative(gainTenthsDb: Int): Int
// Changes the tuner gain WHILE playing (no retune, no gap) - for the manual
// gain setting. librtlsdr picks the nearest step the tuner supports.
// Returns the gain now set (tenths of a dB), or -1 if the dongle isn't open.
// (Going back to "Auto" needs a restart of the stream instead, because
// doAutoGain() has to measure before streaming starts.)
// Must run on the engine thread (never at the same time as a tune).
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setGainNative(
        JNIEnv* /* env */, jobject /* this */, jint gainTenthsDb) {
    if (g_dev == nullptr || gainTenthsDb < 0) return -1;
    if (g_direct) return -1;                            // 14c: direct sampling - the tuner is off
    rtlsdr_set_tuner_gain_mode(g_dev, 1);               // 1 = manual gain
    rtlsdr_set_tuner_gain(g_dev, gainTenthsDb);
    int actual = rtlsdr_get_tuner_gain(g_dev);
    g_gainTenths = actual;                              // shown in the signal details
    g_gainAuto = false;
    LOGI("Manual gain: asked %.1f dB, now %.1f dB", gainTenthsDb / 10.0f, actual / 10.0f);
    return actual;
}

// ======================= Milestone 10c: gain watchdog =======================
// The watchdog itself is Kotlin (GainKeeper.kt, driven by RadioService once
// a second). These are its hands and eyes.

// Kotlin (RadioEngine): external fun takePeakMaxNative(): Float
// The loudest block's peak (dBFS) since the last call, then starts over.
// -99 = no samples arrived meanwhile.
extern "C" JNIEXPORT jfloat JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_takePeakMaxNative(JNIEnv*, jobject) {
    return g_peakMax.exchange(-99.0f);
}

// Kotlin (RadioEngine): external fun getGainStepsNative(): IntArray
// The gain steps THIS dongle's tuner has (tenths of a dB, low to high).
// Empty if the dongle isn't open.
extern "C" JNIEXPORT jintArray JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getGainStepsNative(JNIEnv* env, jobject) {
    std::vector<int> gains;
    if (g_dev != nullptr) {
        int count = rtlsdr_get_tuner_gains(g_dev, nullptr);
        if (count > 0) {
            gains.resize(count);
            count = rtlsdr_get_tuner_gains(g_dev, gains.data());
            gains.resize(count > 0 ? count : 0);
        }
    }
    jintArray out = env->NewIntArray((jsize)gains.size());
    if (out != nullptr && !gains.empty()) {
        std::vector<jint> values(gains.begin(), gains.end());
        env->SetIntArrayRegion(out, 0, (jsize)values.size(), values.data());
    }
    return out;
}

// Kotlin (RadioEngine): external fun setAutoGainNative(gainTenthsDb: Int): Int
// Like setGainNative, but it's the watchdog moving the AUTO gain: the gain
// stays "auto" in the details. Live, no gap. Returns the gain now set, or -1.
// Must run on the engine thread (never at the same time as a tune).
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setAutoGainNative(
        JNIEnv* /* env */, jobject /* this */, jint gainTenthsDb) {
    if (g_dev == nullptr || !g_streaming || gainTenthsDb < 0) return -1;
    if (g_direct) return -1;                            // 14c: direct sampling - the tuner is off
    int before = g_gainTenths.load();
    rtlsdr_set_tuner_gain(g_dev, gainTenthsDb);         // still in manual mode since doAutoGain
    int actual = rtlsdr_get_tuner_gain(g_dev);
    g_gainTenths = actual;
    g_gainAuto = true;
    g_peakMax = -99.0f;                                 // measure the new gain from scratch
    LOGI("Gain watchdog: %.1f -> %.1f dB", before / 10.0f, actual / 10.0f);
    return actual;
}

// ======================= Milestone 9d step 3: pictures =======================

// Kotlin (RadioEngine): external fun getPictureGenNative(): Int
// Goes up whenever any picture changes (or on a retune). Cheap to call every second.
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getPictureGenNative(JNIEnv*, jobject) {
    return g_pictureGen.load();
}

// A picture's bytes -> a new Java byte[] (or null if there's no picture).
static jbyteArray toJBytes(JNIEnv *env, const Picture *p) {
    if (p == nullptr || p->bytes.empty()) return nullptr;
    jbyteArray arr = env->NewByteArray((jsize)p->bytes.size());
    if (arr != nullptr)
        env->SetByteArrayRegion(arr, 0, (jsize)p->bytes.size(),
                                reinterpret_cast<const jbyte *>(p->bytes.data()));
    return arr;
}

// Kotlin (RadioEngine): external fun getLogoNative(program: Int): ByteArray?
// The program's logo file (PNG/JPEG), else the station-wide logo, else null.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getLogoNative(JNIEnv* env, jobject, jint program) {
    if (program < 0 || program >= MAX_PROGRAMS) return nullptr;
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return toJBytes(env, chosenLogo(program));
}

// Kotlin (RadioEngine): external fun getArtNative(program: Int): ByteArray?
// The album art that goes with the current song (following XHDR), or null.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getArtNative(JNIEnv* env, jobject, jint program) {
    if (program < 0 || program >= MAX_PROGRAMS) return nullptr;
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return toJBytes(env, chosenArt(program));
}

// ======================= Milestone 7: audio for Kotlin =======================

// M11: after a source switch, fade the new source in over 20 ms (same idea as
// the program switch), so it doesn't start with a click. Needs g_audioMutex.
static void sourceFadeIn(int16_t *values, size_t n) {
    for (size_t i = 0; i < n && g_sourceFadeLeft > 0; i++, g_sourceFadeLeft--) {
        size_t frame = (FADE_VALUES - g_sourceFadeLeft) / 2;
        values[i] = (int16_t)(values[i] * (float)(frame + 1) / (FADE_VALUES / 2));
    }
}

// Kotlin (RadioEngine): external fun readAudioNative(out: ShortArray, timeoutMs: Int): Int
// Copies up to out.size audio values into `out`. If none are ready, waits up
// to timeoutMs for some to arrive. Returns how many values were copied (0 = none).
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_readAudioNative(
        JNIEnv* env, jobject /* this */, jshortArray out, jint timeoutMs) {
    const jsize capacity = env->GetArrayLength(out);
    static thread_local std::vector<int16_t> tmp;   // reused between calls (no re-allocating)
    if ((jsize)tmp.size() < capacity) tmp.resize(capacity);

    size_t n = 0;
    // 11d: in Auto, HD1 comes out of the blender (its own lock and wake-up); HD2+
    // play live from the HD ring, exactly as in Digital only.
    if (g_audioSource.load() == SOURCE_AUTO && g_program.load() == 0) {
        align::Status al = g_align.status();              // fetched first: the blender's lock is a leaf
        n = g_blend.read(tmp.data(), (size_t)capacity / 2, timeoutMs, al) * 2;
        std::lock_guard<std::mutex> lock(g_audioMutex);   // (only for g_sourceFadeLeft)
        sourceFadeIn(tmp.data(), n);
    } else {
        std::unique_lock<std::mutex> lock(g_audioMutex);
        if (g_audioSource.load() == SOURCE_ANALOG) {
            // M11: the analog ring -> stereo values (11b: it holds L R pairs already).
            g_audioCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [] { return g_fmRingCount > 0; });
            size_t frames = std::min((size_t)capacity / 2, g_fmRingCount);
            for (size_t i = 0; i < frames; i++) {
                tmp[2 * i] = g_fmRing[g_fmRingRead * 2];
                tmp[2 * i + 1] = g_fmRing[g_fmRingRead * 2 + 1];
                g_fmRingRead = (g_fmRingRead + 1) % FM_RING_FRAMES;
            }
            g_fmRingCount -= frames;
            n = frames * 2;
        } else {
            g_audioCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [] { return g_ringCount > 0; });
            n = std::min((size_t)capacity, g_ringCount);
            for (size_t i = 0; i < n; i++) {
                tmp[i] = g_ring[g_ringRead];
                g_ringRead = (g_ringRead + 1) % AUDIO_RING_VALUES;
            }
            g_ringCount -= n;
        }
        sourceFadeIn(tmp.data(), n);
    }
    // Copy into the Kotlin array outside the lock (keeps the lock short).
    if (n > 0) env->SetShortArrayRegion(out, 0, (jsize)n, tmp.data());
    return (jint)n;
}

// Kotlin (RadioEngine): external fun getAudioBufferedNative(): Int  - audio values waiting in the ring
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getAudioBufferedNative(JNIEnv*, jobject) {
    // 11d: Auto on HD1 = the blender (its output is stereo: values = frames * 2)
    if (g_audioSource.load() == SOURCE_AUTO && g_program.load() == 0) return (jint)(g_blend.available() * 2);
    std::lock_guard<std::mutex> lock(g_audioMutex);
    // M11: the ring being played (analog: frames -> stereo values)
    if (g_audioSource.load() == SOURCE_ANALOG) return (jint)(g_fmRingCount * 2);
    return (jint)g_ringCount;
}

// Kotlin (RadioEngine): external fun setAudioSourceNative(source: Int)
// M11: 0 = digital only (HD ring), 1 = analog only (our FM demodulator),
// 11d: 2 = auto (the blend). Works live: the next readAudioNative() serves the
// other source, faded in. The rings are emptied so old audio isn't played first.
// Must run on the engine thread (like every source/program change).
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setAudioSourceNative(JNIEnv*, jobject, jint source) {
    int wanted = source == SOURCE_ANALOG ? SOURCE_ANALOG : source == SOURCE_AUTO ? SOURCE_AUTO : SOURCE_HD;
    if (g_streaming && g_mode == NRSC5_MODE_AM) wanted = SOURCE_HD;   // 9c: AM is HD only
    int before = g_audioSource.load();
    if (before == wanted) return;
    if (before == SOURCE_HD && g_streaming) {
        // 11d: the demodulator was off (Digital only) and starts now, in the middle of
        // the stream. Its frame counter must stay in step with the IQ time line (the
        // aligner and the blender rely on frame n = IQ time n * 33.75, less what the
        // clock correction has taken out), so jump the
        // analog counters forward to "now" BEFORE the streaming thread starts pushing.
        long long frame = timelineFrame(g_iqPos.load());
        g_align.skipAnalogTo(frame);
        g_blend.skipAnalogTo(frame);
    }
    if (wanted == SOURCE_AUTO) g_blend.restart();        // play from the newest analog, blend afresh
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_audioSource = wanted;
    g_fmRingRead = g_fmRingWrite = g_fmRingCount = 0;    // both rings start fresh: no stale audio
    g_ringRead = g_ringWrite = g_ringCount = 0;
    g_fadeInLeft = 0;
    g_sourceFadeLeft = FADE_VALUES;
    if (wanted != SOURCE_HD && before == SOURCE_HD) {
        g_fmCarrierDb = -99.0f;                          // "measuring" until the first block
        g_fmQuietDb = -99.0f;                            // 9k
        g_fmPilot = 0; g_fmPilotLevel = 0.0f; g_fmStereo = 0.0f;   // 11b
    }
    LOGI("Audio source: %s", wanted == SOURCE_ANALOG ? "analog" : wanted == SOURCE_AUTO ? "auto (blend)" : "HD");
}

// Kotlin (RadioEngine): external fun setBlendUnmatchedNative(playHd: Boolean)
// 11d: the "If the HD signal doesn't match the analog" setting - true = play the
// HD anyway (unaligned, like a car radio), false = stay on the analog.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setBlendUnmatchedNative(JNIEnv*, jobject, jboolean playHd) {
    g_blend.setUnmatchedPlaysHd(playHd == JNI_TRUE);
    LOGI("Unmatched HD: %s", playHd ? "play it anyway" : "stay on analog");
}

// Kotlin (RadioEngine): external fun setStereoModeNative(mode: Int)
// 11b: the "FM stereo" setting - 0 = auto (stereo when the station sends it, blended
// to mono as the signal gets noisy, like a car radio), 1 = always mono (the L-R work
// is skipped), 2 = stereo whenever there is a pilot, never blended. Works live: the
// demodulator picks it up at its next block. Any thread.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setStereoModeNative(JNIEnv*, jobject, jint mode) {
    g_stereoMode = mode == 1 ? fm::STEREO_MONO : mode == 2 ? fm::STEREO_FULL : fm::STEREO_AUTO;
    LOGI("FM stereo: %s", mode == 1 ? "always mono" : mode == 2 ? "always stereo" : "auto (blend to mono when noisy)");
}

// Kotlin (RadioEngine): external fun setDeemphasisNative(us: Int)
// 11b step 2: FM de-emphasis, 75 us (the Americas) or 50 us (the rest of the world),
// from Settings -> Band (region). Works live, like the stereo setting. Any thread.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setDeemphasisNative(JNIEnv*, jobject, jint us) {
    int v = us == 50 ? 50 : 75;
    if (g_deemphUs.exchange(v) != v) LOGI("FM de-emphasis: %d us", v);
}

// Kotlin (RadioEngine): external fun setChannelCallsNative(calls: String, known: Boolean)
// 13b: the call letters licensed on the frequency about to be tuned (from the FCC list in
// the app, Stations.kt), separated by spaces; known = false: no list for this frequency.
// Called before startStreamNative (which doesn't clear it). Any thread.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setChannelCallsNative(JNIEnv *env, jobject, jstring calls, jboolean known) {
    std::vector<std::string> list;
    if (calls != nullptr) {
        const char *text = env->GetStringUTFChars(calls, nullptr);    // plain letters: no special characters to worry about
        if (text != nullptr) {
            std::string word;
            for (const char *p = text; ; p++) {
                if (*p == ' ' || *p == 0) { if (!word.empty()) list.push_back(word); word.clear(); if (*p == 0) break; }
                else word += *p;
            }
            env->ReleaseStringUTFChars(calls, text);
        }
    }
    LOGI("FCC list: %s", known == JNI_TRUE ? (std::to_string(list.size()) + " call sign(s) licensed on this frequency").c_str()
                                           : "none for this frequency");
    std::lock_guard<std::mutex> lock(g_rdsMutex);
    g_channelCalls = std::move(list);
    g_channelKnown = known == JNI_TRUE;
}

// Kotlin (RadioEngine): external fun measureAlignmentNative(): Int
// 11c: one alignment measurement if it's time and there is enough audio (the
// aligner itself decides: first ~8 s after tuning, then every 5 s). Takes
// ~10-40 ms -> call it on the engine thread, never on the UI thread.
// Returns 0 = nothing measured this time, 1 = a result was accepted, 2 = no match.
extern "C" JNIEXPORT jint JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_measureAlignmentNative(JNIEnv*, jobject) {
    if (!g_streaming || g_mode != NRSC5_MODE_FM || !demodWanted()) return 0;   // 11d: off in Digital only
    align::Result r;
    if (!g_align.measure(r)) return 0;
    if (r.accepted) {
        // Build 6: every accepted measurement also tells the clock correction how the analog
        // moves against the HD (drift.hpp).
        align::Status al = g_align.status();
        g_drift.addMeasurement(r.at, r.offsetFine, r.corr, r.epoch, al.state == 2, al.offsetFrames);
        drift::Status d = g_drift.status();
        char note[80] = "";                         // build 8: a witness that was not believed (drift.hpp)
        if (d.state != 0 && d.unused != 0)
            snprintf(note, sizeof(note), "; the %s says %+.1f ppm - not used", d.unused == 1 ? "audio" : "tuning offset", d.unusedPpm);
        LOGI("Alignment: analog lags HD1 by %.1f ms (offset %lld frames, r %.3f), analog %+.1f dB%s; dongle clock %+.1f ppm%s%s",
             r.dMs, r.offsetFrames, r.corr, r.gainDb, r.usedHint ? "" : " (full search)",
             d.clockPpm, d.state == 2 ? " (measured)" : d.state == 1 ? " (from the tuning offset)" : " (not corrected)", note);
        return 1;
    }
    LOGI("Alignment: no match (r %.3f%s)", r.corr, r.usedHint ? ", near the expected value" : ", full search");
    return 2;
}

// Tests only (the desktop harness; the app never calls it): clock correction on / off.
// Off = the analog audio goes to the aligner and the blender exactly as it comes from the
// demodulator, as in every build before 6.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setClockCorrectionNative(JNIEnv*, jobject, jboolean on) {
    g_drift.setEnabled(on == JNI_TRUE);
    LOGI("Clock correction: %s", on == JNI_TRUE ? "on" : "OFF (test)");
}

// Tests only (the desktop harness; the app never calls it): the rest of the HD search on / off.
// Off = nrsc5 gets every block in Auto whether it finds HD or not, as in every build before 9.
extern "C" JNIEXPORT void JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_setHdRestNative(JNIEnv*, jobject, jboolean on) {
    g_hdRestAllowed = on == JNI_TRUE;
    LOGI("HD search rest: %s", on == JNI_TRUE ? "on" : "OFF (test)");
}

// Kotlin (RadioEngine): external fun getAudioDroppedNative(): Long - values dropped because the ring was full
extern "C" JNIEXPORT jlong JNICALL
Java_io_github_derek20la_hidefradio_RadioEngine_getAudioDroppedNative(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    return (jlong)g_audioDropped;
}
