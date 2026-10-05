package io.github.derek20la.hidefradio

/**
 * The radio "engine": every function that is written in C/C++ (native-lib.cpp).
 *
 * Milestone 8a: these used to live inside MainActivity. They are moved here, into
 * a Kotlin `object` (a single shared instance), so that ANY part of the app can use
 * them - next step: a background Service that keeps the radio playing with the
 * screen off.
 *
 * `external` = "implemented in C/C++". JNI finds each one by its full name, e.g.
 * Kotlin RadioEngine.openDongleNative  <->  C++ Java_io_github_derek20la_hidefradio_RadioEngine_openDongleNative
 */
object RadioEngine {

    init {
        // Load libhidefradio.so (our C++ code + all the libraries) once.
        System.loadLibrary("hidefradio")
    }

    /**
     * A native method that is implemented by the 'hidefradio' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String

    /** Opens the dongle in librtlsdr using Android's fd. Returns a status message. */
    external fun openDongleNative(fd: Int): String

    /** Closes the dongle in librtlsdr (call before closing the UsbDeviceConnection). */
    external fun closeDongleNative()

    /** True if librtlsdr has the dongle open. */
    external fun isDongleOpenNative(): Boolean

    /**
     * Tunes, sets gain + sample rate, starts nrsc5 and the streaming thread.
     * gainTenthsDb: 0 = 0.0 dB, 496 = 49.6 dB; negative = automatic (nrsc5-style).
     * hintTenthsDb (M10c): automatic only - the gain that worked on this
     * frequency last time (GainMemory); tried first, used if it still fits. -1 = none.
     */
    external fun startStreamNative(freqHz: Int, gainTenthsDb: Int, hintTenthsDb: Int): String

    /** What nrsc5 has decoded so far: sync, MER/BER, station name, song... */
    external fun getStatusNative(): String

    /** Copies decoded audio into buffer (waits up to timeoutMs). Returns values copied. */
    external fun readAudioNative(buffer: ShortArray, timeoutMs: Int): Int

    /** Audio values waiting in the native ring buffer (88,200 = 1 second). */
    external fun getAudioBufferedNative(): Int

    /** Audio values thrown away because the ring buffer was full. */
    external fun getAudioDroppedNative(): Long

    /** Stops streaming and waits for the streaming thread to end. */
    external fun stopStreamNative()

    /** Total bytes received so far (2 bytes per sample). */
    external fun getStreamBytesNative(): Long

    /** True while samples are flowing. */
    external fun isStreamingNative(): Boolean

    /** Signal level of the latest block of samples (0 = nothing, ~128 = max). */
    external fun getSignalLevelNative(): Float

    // ---- Milestone 9b: programs (HD1 ... HD8) ----

    /** Play program [program] (0 = HD1 ... 7 = HD8). Fades over smoothly (9b step 3). */
    external fun setProgramNative(program: Int)

    /** The program being played (0 = HD1). Goes back to 0 on every tune. */
    external fun getProgramNative(): Int

    /**
     * The programs on the air: one line each, TAB-separated
     * (number, type, SIG name, artist, title). Use Program.parseList() on it.
     */
    external fun getProgramsNative(): String

    // ---- Milestone 9d: signal details ----

    /**
     * Signal details for the now-playing screen, as "key=value" lines
     * (synced, mode, merLower, merUpper, ber, berAvg, berMin, berMax, kbps,
     * crcErrors, gainDb, peakDbfs, syncMs, audioMs, station, slogan ...).
     * "synced" already includes the 1.5 s grace period after a sync loss.
     */
    external fun getSignalNative(): String

    // ---- Milestone 9d step 3: pictures ----

    /** Goes up whenever any logo or album art changes (or on a retune). */
    external fun getPictureGenNative(): Int

    /** The logo file (PNG/JPEG bytes) of [program] (else the station-wide one), or null. */
    external fun getLogoNative(program: Int): ByteArray?

    /** The album art that goes with [program]'s current song, or null. */
    external fun getArtNative(program: Int): ByteArray?

    // ---- Milestone 9e step 2: settings ----

    /**
     * Manual gain, changed while playing (no retune). 197 = 19.7 dB; the tuner
     * uses its nearest step. Returns the gain now set, or -1 if the dongle isn't open.
     */
    external fun setGainNative(gainTenthsDb: Int): Int

    // ---- Milestone 10c: gain watchdog (see GainKeeper) ----

    /** The loudest peak (dBFS) since the last call, then starts over. -99 = no samples. */
    external fun takePeakMaxNative(): Float

    /** The gain steps this dongle's tuner has, in tenths of a dB, low to high. */
    external fun getGainStepsNative(): IntArray

    /**
     * The watchdog moves the AUTO gain (it stays "auto"). Live, no gap.
     * Engine thread only. Returns the gain now set, or -1.
     */
    external fun setAutoGainNative(gainTenthsDb: Int): Int

    // ---- Milestone 11 (11a): analog FM ----

    /**
     * Which audio to play: 0 = digital only (HD, as before), 1 = analog only
     * (our own FM demodulator, mono), 2 = auto (11d: the blend - analog at once,
     * HD1 when it's good and aligned). Works live, faded in. Engine thread.
     */
    external fun setAudioSourceNative(source: Int)

    /**
     * 11d: in Auto, if the HD signal never lines up with the analog (it's another
     * station's HD): true = play the HD anyway, false = stay on the analog.
     */
    external fun setBlendUnmatchedNative(playHd: Boolean)

    /**
     * 11b: the "FM stereo" setting - 0 = auto (stereo when the station sends a
     * pilot, blended to mono as the signal gets noisy, like a car radio - the
     * default), 1 = always mono (skips the L-R work), 2 = always stereo (never
     * blended). Works live. Any thread. 11b step 2: Settings -> "FM stereo".
     */
    external fun setStereoModeNative(mode: Int)

    /**
     * 11b step 2: FM de-emphasis in microseconds - 75 (the Americas, the default)
     * or 50 (the rest of the world). Follows Settings -> Band (region). Works live. Any thread.
     */
    external fun setDeemphasisNative(us: Int)

    /**
     * 13b: the call letters licensed on the frequency about to be tuned, from the
     * built-in FCC list (Stations.callsOn): "KHYL KWYE KRTH", separated by spaces.
     * [known] = false: there is no list for this frequency (AM, another region,
     * list unreadable) - the native code then works as it did before the list.
     * Used for the call letters of "1xxx" RDS codes and for the "is the HD the
     * same station?" check. Call it BEFORE startStreamNative. Any thread.
     */
    external fun setChannelCallsNative(calls: String, known: Boolean)

    /**
     * 11c: one alignment measurement (analog vs HD1 time offset and loudness)
     * if it's time and there is enough audio - see aligner.hpp. Takes 10-40 ms
     * -> engine thread only. Returns 0 = nothing measured, 1 = a result was
     * accepted, 2 = no match. The results come back through getSignalNative()
     * (Signal.alignState etc.) and Logcat ("Alignment: ...").
     */
    external fun measureAlignmentNative(): Int
}
