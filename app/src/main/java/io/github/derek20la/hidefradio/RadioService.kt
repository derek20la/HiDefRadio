package io.github.derek20la.hidefradio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.view.KeyEvent
import android.content.SharedPreferences
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.Icon
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * Milestone 8: the radio runs HERE, in a "foreground service", instead of in the
 * screen (MainActivity). A foreground service shows a notification and Android
 * lets it keep running while the screen is off or you use other apps.
 *
 * The service owns everything that must stay alive while playing:
 *   the USB connection, the native engine (RadioEngine) and the AudioPlayer.
 * MainActivity only asks for USB permission, starts/stops this service and
 * displays the status.
 *
 * Commands (Intents) it understands:
 *   ACTION_START + EXTRA_DEVICE (a UsbDevice we have permission for) -> play
 *   ACTION_STOP                                                     -> stop
 *
 * Milestone 9a: MainActivity changes station by calling tune(freqHz).
 * The last frequency is remembered (saveFrequency / loadFrequency).
 * Milestone 9b: MainActivity switches programs (HD1, HD2...) with selectProgram().
 * Milestone 9d: signal() for the now-playing screen; logo()/art() pictures,
 * and every logo received is saved in LogoCache.
 * Milestone 9e: tune(freq, program) for presets - switches to e.g. HD2 as soon
 * as the station has it on the air; the last program is remembered too.
 * Milestone 10a: a MediaSession - Android's "remote control" for media apps.
 * It gives us the media notification (picture, ⏮ ⏸ ⏭, Stop), the lock
 * screen player, and the buttons on headphones, Bluetooth and car radios.
 *   ⏸ = pause: the tuner keeps running silently (instant, live ▶), and
 *       stops by itself after a while (setting, default 5 min).
 *   ⏮ ⏭ = previous / next preset (no presets: previous / next channel).
 * Headphones unplugged ("becoming noisy") -> pause.
 * Milestone 10b: the slow native work (retuning with auto-gain, stopping)
 * runs on a separate "engine" thread, so the screen and the media buttons
 * never freeze while a station is being tuned.
 * Milestone 10c: a gain watchdog (GainKeeper) keeps adjusting the AUTO gain
 * while you listen (by the peak level, and by trying neighbouring steps' MER), and the gain that worked is remembered per frequency
 * (GainMemory), so the next tune can skip the search.
 */
class RadioService : Service() {

    private lateinit var usbManager: UsbManager
    private var connection: UsbDeviceConnection? = null   // kept open while playing
    private var device: UsbDevice? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private val handler = Handler(Looper.getMainLooper())

    private val audioPlayer = AudioPlayer(
        readAudio = { buffer, timeoutMs -> RadioEngine.readAudioNative(buffer, timeoutMs) },
        bufferedValues = { RadioEngine.getAudioBufferedNative() },
    )

    // ---- Milestone 8c: audio focus ----
    // Android's "talking stick" for sound: we ask for it when we start playing,
    // and Android tells us (onAudioFocusChange) when another app wants to play.
    private lateinit var audioManager: AudioManager
    private var focusRequest: AudioFocusRequest? = null

    // Text shown by MainActivity: the "Opened ... / Tuner ..." part, and
    // the "Tuned to ... kHz, gain ..." line (replaced on every tune).
    private var dongleInfo = ""
    @Volatile private var streamInfo = ""       // M10b: written on the engine thread

    /** Milestone 9a: the frequency we're tuned to, in Hz (e.g. 101_100_000). */
    var freqHz = DEFAULT_FREQ_HZ
        private set

    // For the sample-rate numbers in statusText().
    @Volatile private var lastBytes = 0L
    @Volatile private var lastTimeMs = 0L
    @Volatile private var streamStartMs = 0L

    // ---- Milestone 10b: the engine thread ----
    // ONE background thread does all the slow native work, one job after the
    // other: stop the old stream, auto-gain + start the new one, gain changes,
    // shutting down. Because it's one thread, two of those can never run at
    // the same time (librtlsdr doesn't like that), and the main thread - which
    // draws the screen and answers the media buttons - never has to wait.
    private val engine: ExecutorService =
        Executors.newSingleThreadExecutor { job -> Thread(job, "HiDefEngine") }

    // Every retune gets a number. A retune job that finds a newer number has
    // been handed out skips itself: tapping ⏭ ⏭ ⏭ quickly does ONE retune.
    private val retuneSeq = AtomicInteger(0)

    /**
     * True from the moment a retune is asked for until the new station is
     * streaming. Meanwhile signal()/programs()/logo()/art() report "nothing
     * yet", so the screen never shows the OLD station's name or logo next
     * to the NEW frequency (and no old logo is saved under the new one).
     */
    @Volatile var retuning = false
        private set

    // When the last retune was asked for (for "sound after ... s").
    @Volatile private var tuneStartMs = 0L

    // ---- Milestone 10a: MediaSession, pause ----
    private lateinit var session: MediaSession

    /** Paused by you (⏸). The tuner keeps running; the sound is off. */
    var paused = false
        private set

    // True after another app took the audio "for good" (e.g. you started a
    // video) while the setting says "mute": we're silent, like paused.
    private var mutedForGood = false

    // 9h: the pause came from headphones / Bluetooth disconnecting (not from you),
    // and when the auto-stop will turn the radio off (elapsedRealtime; 0 = not
    // counting down). Both only for the "why is it silent" line on the screen.
    private var pausedByUnplug = false
    private var stopAtMs = 0L

    // ⏮ ⏭ pressed: where we're about to tune. The retune waits SKIP_DELAY_MS,
    // so pressing ⏭ three times quickly tunes once, to the third preset.
    // pendingFreqHz = 0 -> nothing pending.
    private var pendingFreqHz = 0
    private var pendingProgram = 0
    private var pendingIndex = -1          // index in the preset list, -1 = not a preset

    // ---- Milestone 10c: gain watchdog ----
    /** Watches the AUTO gain (null while the radio is off). Main thread only. */
    var gainKeeper: GainKeeper? = null
        private set
    // True while a watchdog gain change waits for / runs on the engine thread.
    @Volatile private var gainBusy = false

    // ------------------------------------------------------------------ lifecycle

    override fun onCreate() {
        super.onCreate()
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        createNotificationChannel()
        createSession()                                                   // M10a
        ContextCompat.registerReceiver(
            this, detachReceiver,
            IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED),
            ContextCompat.RECEIVER_NOT_EXPORTED)
        // M10a: headphones unplugged / Bluetooth disconnected -> pause.
        // (A message from Android itself, so "not exported" still receives it.)
        ContextCompat.registerReceiver(
            this, noisyReceiver,
            IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY),
            ContextCompat.RECEIVER_NOT_EXPORTED)
        Settings.writeDefaults(this)    // 14b step 3: BEFORE listening, see Settings.writeDefaults
        Settings.prefs(this).registerOnSharedPreferenceChangeListener(settingsListener)   // 9e
        instance = this
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                // Android requires startForeground() within a few seconds of
                // startForegroundService(), so do it first.
                goForeground("Starting…")
                val dev = IntentCompat.getParcelableExtra(intent, EXTRA_DEVICE, UsbDevice::class.java)
                if (dev != null && connection == null) {
                    if (!startRadio(dev)) stopEverything()
                }
            }
            ACTION_STOP -> stopEverything()
            // M10a: the notification's buttons (Android 12 and older use these;
            // Android 13+ sends them to the MediaSession callback instead).
            ACTION_PLAY, ACTION_PAUSE, ACTION_NEXT, ACTION_PREVIOUS -> {
                if (connection == null) {
                    stopEverything()        // an old notification; the radio isn't running
                } else when (intent.action) {
                    ACTION_PLAY -> play()
                    ACTION_PAUSE -> pause()
                    ACTION_NEXT -> skipPreset(forward = true)
                    ACTION_PREVIOUS -> skipPreset(forward = false)
                }
            }
        }
        // If Android kills us, don't restart automatically (we'd have no dongle).
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopRadio()
        unregisterReceiver(detachReceiver)
        unregisterReceiver(noisyReceiver)
        Settings.prefs(this).unregisterOnSharedPreferenceChangeListener(settingsListener)
        session.release()                                                 // M10a
        engine.shutdown()                                                 // M10b
        instance = null
        super.onDestroy()
    }

    // We don't use "binding"; MainActivity reads status via RadioService.instance.
    override fun onBind(intent: Intent?): IBinder? = null

    // ------------------------------------------------------------------ radio

    /** Opens the dongle and starts streaming + decoding + playing. */
    private fun startRadio(dev: UsbDevice): Boolean {
        val conn = usbManager.openDevice(dev)
        if (conn == null) {
            Log.e(TAG, "openDevice failed")
            return false
        }
        connection = conn
        device = dev

        val fd = conn.fileDescriptor
        val openResult = RadioEngine.openDongleNative(fd)
        Log.i(TAG, openResult)
        dongleInfo = "Opened ${dev.productName ?: "USB device"}\nfd = $fd\n\n$openResult"
        if (!RadioEngine.isDongleOpenNative()) {
            conn.close()
            connection = null
            return false
        }

        gainKeeper = GainKeeper(RadioEngine.getGainStepsNative())   // M10c

        freqHz = loadFrequency(this)    // the station from last time
        val program = loadProgram(this) // 9e: ...and the program from last time (e.g. HD2)
        restartStream(program)          // M10b: auto-gain + stream + audio, on the engine thread
        wantProgram(program)

        applyFocusMode()                // 9e: ask for audio focus (unless "keep playing" is set)
        session.isActive = true         // M10a: headphone / Bluetooth / car buttons now come to us

        // Keep the CPU running with the screen off (the USB + decoding thread
        // must never sleep). Released in stopRadio().
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "HiDefRadio::radio").apply {
            setReferenceCounted(false)
            acquire()
        }

        handler.post(notificationUpdater)
        handler.postDelayed(gainWatcher, GAIN_TICK_MS)   // M10c
        handler.postDelayed(alignWatcher, ALIGN_TICK_MS) // 11c
        return true
    }

    /**
     * Tunes to [hz]: gain, sample rate, a new nrsc5 decoder, streaming thread.
     * M10b: runs on the engine thread (auto-gain alone can take a moment).
     */
    private fun startStream(hz: Int) {
        // 9e: the gain comes from the settings (-1 = auto, the default).
        // M10c: on auto, the gain remembered for this frequency is tried first.
        val gain = Settings.gainTenths(this)
        val hint = if (gain < 0) GainMemory.get(this, hz) else -1
        sourceOverride = -1                     // M12 step 2: a new station = the setting again
        RadioEngine.setAudioSourceNative(Settings.audioSourceFor(this, hz))   // M11: digital, analog or auto (11d); 9c: AM = HD
        RadioEngine.setBlendUnmatchedNative(Settings.unmatchedPlaysHd(this))   // 11d
        RadioEngine.setStereoModeNative(Settings.stereoMode(this))             // 11b step 2
        RadioEngine.setDeemphasisNative(Settings.deemphasisUs(this))           // 11b step 2: 75 / 50 us by region
        // 13b: who is licensed on this frequency (the built-in FCC list) - FM in the
        // Americas region only; elsewhere the native code works without the list.
        val calls = if (!FmBand.isAm(hz) && Settings.band(this) == FmBand.AMERICAS) Stations.callsOn(this, hz) else null
        RadioEngine.setChannelCallsNative(calls ?: "", calls != null)
        streamInfo = RadioEngine.startStreamNative(hz, gain, hint)
        Log.i(TAG, streamInfo)
        lastBytes = 0L
        lastTimeMs = SystemClock.elapsedRealtime()
        streamStartMs = lastTimeMs
    }

    /**
     * Milestone 9a: change station. Called by MainActivity (on the main thread).
     * Keeps the dongle open and keeps audio focus; only the stream restarts:
     * stop audio -> stop stream -> start stream on the new frequency -> start audio.
     * If the radio isn't playing, the frequency is just remembered for next time.
     *
     * 9e step 1: [program] = which program to play (a preset can be HD2). A new
     * station always starts on HD1, and HD2 only "exists" once the station's
     * signal has been decoded, so we remember the wish and switch as soon as
     * that program is on the air (see wantProgram).
     */
    fun tune(newFreqHz: Int, program: Int = 0) {
        clearPendingSkip()                      // M10a: this tune beats a pending ⏮ ⏭
        if (paused) play()                      // M10a: changing station = you want to hear it
        saveFrequency(this, newFreqHz)
        saveProgram(this, program)
        val sameStation = newFreqHz == freqHz && (retuning || RadioEngine.isStreamingNative())
        freqHz = newFreqHz
        if (connection == null) return

        if (!sameStation) {
            Log.i(TAG, "Tuning to $newFreqHz Hz")
            restartStream(program)
        }
        wantProgram(program)                    // 9e: same station -> just switch program
        updateNotification()
    }

    /**
     * Starts the stream again on freqHz (a retune, gain back to "auto", or the
     * first start). M10b: returns straight away - the work is done on the
     * engine thread:
     *   stop audio -> stop the old stream -> auto-gain + new stream -> audio.
     * 10b step 2: [program] (e.g. 3 = HD4 for a preset) is selected right away,
     * before the station has even announced it. Only that program's audio gets
     * into the player, so you never hear a moment of HD1 first. (Stations
     * announce their programs at slightly different times - KKGO's HD4 comes
     * about half a second after HD1-3 - and HD1's sound could win that race.)
     */
    private fun restartStream(program: Int = 0) {
        val seq = retuneSeq.incrementAndGet()
        val hz = freqHz
        retuning = true
        tuneStartMs = SystemClock.elapsedRealtime()
        engine.execute {
            if (seq != retuneSeq.get()) return@execute   // a newer tune is waiting: skip this one
            audioPlayer.stop()                  // 1. stop playing the old station
            RadioEngine.stopStreamNative()      // 2. stop USB streaming (+ close nrsc5)
            if (seq != retuneSeq.get()) return@execute   // (asked again meanwhile, or stopping)
            startStream(hz)                     // 3. new frequency: auto-gain, new nrsc5, stream
            if (program > 0) RadioEngine.setProgramNative(program)   // 10b step 2 (native starts on HD1)
            audioPlayer.start()                 // 4. pre-buffers 0.25 s of the new station, then plays
            if (seq == retuneSeq.get()) {
                retuning = false                // the new station's status is live now
                handler.post { updateNotification() }
            }
        }
    }

    /**
     * M10b: how long from asking for a station until its sound started (ms),
     * or -1 while it hasn't started yet. Shown in the signal details.
     */
    fun soundMs(): Long {
        val started = audioPlayer.startedAtMs
        return if (!retuning && started >= tuneStartMs && started > 0) started - tuneStartMs else -1L
    }

    // ------------------------------------------------------------------ settings (9e step 2)

    /**
     * Called by Android whenever a setting changes (you change it on the
     * settings screen while the radio plays) - so changes work straight away.
     * Kept in a variable: Android only keeps a "weak" link to this listener.
     */
    private val settingsListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        when (key) {
            Settings.KEY_GAIN -> applyGain()
            Settings.KEY_FOCUS -> if (connection != null) {
                applyFocusMode()
                updatePauseTimer()
                updateNotification()
            }
            Settings.KEY_PAUSE_STOP -> updatePauseTimer()   // M10a: new time counts from now
            // M11: digital <-> analog <-> auto, live (the native side fades the new source in).
            Settings.KEY_HD_MODE -> if (connection != null) {
                sourceOverride = -1                                      // M12 step 2: the setting wins again
                val source = Settings.audioSourceFor(this, freqHz)       // 9c: AM stays HD only
                engine.execute { RadioEngine.setAudioSourceNative(source) }
            }
            Settings.KEY_HD_MISMATCH -> if (connection != null) {         // 11d
                val playHd = Settings.unmatchedPlaysHd(this)
                engine.execute { RadioEngine.setBlendUnmatchedNative(playHd) }
            }
            // 11b step 2: stereo / mono and the region's de-emphasis, live (no restart needed).
            Settings.KEY_FM_STEREO -> if (connection != null) {
                val mode = Settings.stereoMode(this)
                engine.execute { RadioEngine.setStereoModeNative(mode) }
            }
            Settings.KEY_BAND -> if (connection != null) {
                val us = Settings.deemphasisUs(this)
                engine.execute { RadioEngine.setDeemphasisNative(us) }
            }
        }
    }

    /**
     * Manual gain: change it live (no gap). Back to auto: auto-gain has to
     * measure before streaming, so restart the stream on the same station
     * (about 3 s of silence), and keep the program you were on.
     */
    private fun applyGain() {
        if (connection == null) return
        val gain = Settings.gainTenths(this)
        if (gain >= 0) {
            // M10b: on the engine thread, so it can't collide with an auto-gain in progress.
            engine.execute { RadioEngine.setGainNative(gain) }
        } else {
            val program = currentProgram()
            Log.i(TAG, "Gain back to auto - restarting the stream")
            restartStream(program)
            wantProgram(program)
        }
    }

    /** Audio focus according to the setting: ask for it, or ("keep playing") don't. */
    private fun applyFocusMode() {
        abandonAudioFocus()
        mutedForGood = false
        if (Settings.focusMode(this) == Settings.FOCUS_MIX) {
            // Don't take part in audio focus at all: other apps can't mute us,
            // and we don't mute them. Everything plays at once.
            audioPlayer.muted = false
            audioPlayer.volume = 1.0f
            Log.i(TAG, "Audio focus: not used (keep playing with other apps)")
        } else {
            requestAudioFocus()
        }
    }

    // ------------------------------------------------------------------ programs (9b)

    /** The programs on the air right now (HD1, HD2, ...), for the buttons. None while retuning (M10b). */
    fun programs(): List<Program> =
        if (retuning) emptyList() else Program.parseList(RadioEngine.getProgramsNative())

    /**
     * The program being played: 0 = HD1, 1 = HD2 ...
     * 9e: while we're waiting for a wanted program (e.g. a preset for HD2 that
     * hasn't appeared yet) this already says HD2, so the screen shows the
     * right preset, logo and button straight away.
     */
    fun currentProgram(): Int =
        if (wantedProgram >= 0) wantedProgram else RadioEngine.getProgramNative()

    // ---- 9e step 1: switching to a program as soon as it's on the air ----
    // -1 = not waiting for anything.
    private var wantedProgram = -1
    private var wantedSinceMs = 0L

    /** Play [program] as soon as the station has it on the air. */
    private fun wantProgram(program: Int) {
        handler.removeCallbacks(wantedChecker)
        wantedProgram = program
        wantedSinceMs = SystemClock.elapsedRealtime()
        wantedChecker.run()
    }

    /**
     * Checks 5 times a second: is the wanted program on the air yet? Yes -> switch.
     * Gives up (and stays on HD1) when the station has been playing for
     * WANTED_GRACE_MS without it - it probably isn't broadcasting that program
     * any more - or after WANTED_TIMEOUT_MS in any case (no signal at all).
     */
    private val wantedChecker = object : Runnable {
        override fun run() {
            val p = wantedProgram
            if (p < 0) return
            if (retuning) {                             // M10b: the new station isn't streaming yet
                handler.postDelayed(this, 200)
                return
            }
            if (programs().any { it.number == p }) {
                RadioEngine.setProgramNative(p)         // fades like a button tap
                wantedProgram = -1
                Log.i(TAG, "Wanted program HD${p + 1} is on the air - switched")
                updateNotification()
                return
            }
            val s = signal()
            val waitedMs = SystemClock.elapsedRealtime() - wantedSinceMs
            val playingFor = if (s.audioMs >= 0) s.tunedMs - s.audioMs else 0L
            if (playingFor > WANTED_GRACE_MS || waitedMs > WANTED_TIMEOUT_MS) {
                // 10b step 2: after a retune the missing program was already selected
                // (silence) -> fall back to HD1, which every HD station has.
                if (RadioEngine.getProgramNative() == p) RadioEngine.setProgramNative(0)
                Log.i(TAG, "HD${p + 1} not on the air - staying on HD${RadioEngine.getProgramNative() + 1}")
                wantedProgram = -1
                updateNotification()
                return
            }
            handler.postDelayed(this, 200)
        }
    }

    // ------------------------------------------------------------------ gain watchdog (M10c)

    /** Once a second while the radio is on. */
    private val gainWatcher = object : Runnable {
        override fun run() {
            watchGain()
            handler.postDelayed(this, GAIN_TICK_MS)
        }
    }

    /**
     * M10c: hands the last second's loudest peak to the GainKeeper; if it
     * wants another gain, sets it on the engine thread (live, no gap).
     * Only for the AUTO gain - a gain you set by hand is left alone.
     */
    private fun watchGain() {
        val peak = RadioEngine.takePeakMaxNative()    // always take it, so each reading = 1 second
        val keeper = gainKeeper ?: return
        if (gainBusy) return                           // the last change hasn't happened yet
        val now = SystemClock.elapsedRealtime()
        val s = signal()                               // (Signal.NONE while retuning)
        if (retuning || !s.gainAuto || s.gainDb < 0 || !RadioEngine.isStreamingNative()) {
            keeper.restart(now)                        // new station / manual gain: start over
            return
        }
        val gain = Math.round(s.gainDb * 10)
        val newGain = keeper.tick(now, gain, peak, s.locked, s.merCount, s.merLower, s.merUpper)
        if (newGain != GainKeeper.NO_CHANGE) {
            // (M10c step 2: "trying" = a MER try starts; the keeper decides after ~5 s.)
            Log.i(TAG, "Gain watchdog: peak %.1f dBFS, MER %.1f / %.1f -> %.1f dB%s".format(Locale.US,
                peak, s.merLower, s.merUpper, newGain / 10.0, if (keeper.tryingTo >= 0) " (trying)" else ""))
            gainBusy = true
            val seq = retuneSeq.get()
            engine.execute {
                // Skip it if a retune (or Stop) came in meanwhile - that's a new station.
                if (seq == retuneSeq.get()) RadioEngine.setAutoGainNative(newGain)
                gainBusy = false
            }
            return
        }
        // Happy for a while on a station that decodes -> remember this gain for next time.
        if (s.locked && keeper.stableForMs(now) >= REMEMBER_GAIN_AFTER_MS) {
            GainMemory.set(this, freqHz, gain)          // only writes if it's different
        }
    }

    // ------------------------------------------------------------------ alignment (11c)

    /**
     * Every 2 s while the radio is on: let the native aligner measure the
     * analog-vs-HD1 offset if it has enough audio (it decides; a measurement
     * takes 10-40 ms, so it runs on the engine thread, in line with retunes).
     * Results: Signal.alignState/alignMs/... (details row) and Logcat.
     */
    private val alignWatcher = object : Runnable {
        override fun run() {
            if (!retuning && RadioEngine.isStreamingNative()) {
                val seq = retuneSeq.get()
                engine.execute {
                    if (seq == retuneSeq.get()) RadioEngine.measureAlignmentNative()
                }
            }
            handler.postDelayed(this, ALIGN_TICK_MS)
        }
    }

    /**
     * M12 step 2: the HD on this frequency is another station's (RDS PI vs the HD call
     * sign, Signal.otherHd) and you tapped "tap to listen": play the OTHER one of the two -
     * its HD (like Digital only) if you hear the analog now, else the analog (like Analog
     * only). Lasts until the next tune or a change of the "HD Radio" setting; tapping
     * again switches back. -1 = no override (the setting decides).
     */
    @Volatile private var sourceOverride = -1

    fun listenToOtherStation() {
        if (connection == null || retuning) return
        val source = if (signal().analog) Settings.SOURCE_HD else Settings.SOURCE_ANALOG
        sourceOverride = source
        Log.i(TAG, "Other station's HD: now playing the " + if (source == Settings.SOURCE_HD) "HD" else "analog")
        engine.execute { RadioEngine.setAudioSourceNative(source) }
    }

    /** 9d: sync, MER, BER, bit rate, station name... for the now-playing screen. */
    fun signal(): Signal = if (retuning) Signal.NONE else Signal.parse(RadioEngine.getSignalNative())

    // ------------------------------------------------------------------ pictures (9d step 3)

    /** Goes up whenever a logo or album art changes (or on a retune). */
    fun pictureGen(): Int = RadioEngine.getPictureGenNative()

    /** [program]'s logo (PNG/JPEG bytes), or null if none has arrived yet. */
    fun logo(program: Int): ByteArray? = if (retuning) null else RadioEngine.getLogoNative(program)

    /** The album art for [program]'s current song, or null. */
    fun art(program: Int): ByteArray? = if (retuning) null else RadioEngine.getArtNative(program)

    // The pictureGen() we last saved logos for (-1 = never).
    private var savedPictureGen = -1

    /**
     * Decision 17: save every program's logo in LogoCache, so it can be shown
     * straight away next time (and on presets later). Called every 2 s by the
     * notification updater; does nothing unless the pictures changed.
     */
    private fun saveNewLogos() {
        val gen = pictureGen()
        if (gen == savedPictureGen) return
        savedPictureGen = gen
        for (program in programs()) {
            val bytes = logo(program.number) ?: continue
            LogoCache.save(this, freqHz, program.number, bytes)   // only writes if it's new
        }
    }

    /**
     * Milestone 9b: switch to another program of the same station (e.g. HD2).
     * All programs are decoded all the time, so no retune is needed. Since
     * step 3 the audio keeps playing: the native engine fades the old program
     * out and the new one in (20 ms each) - no gap, no click.
     */
    fun selectProgram(program: Int) {
        if (connection == null || program == currentProgram()) return
        Log.i(TAG, "Switching to HD${program + 1}")
        if (paused) play()                      // M10a: picking a program = you want to hear it
        handler.removeCallbacks(wantedChecker)  // 9e: a tap beats a wanted program
        wantedProgram = -1
        RadioEngine.setProgramNative(program)
        saveProgram(this, program)              // 9e: remembered for next time
        updateNotification()
    }

    /** Shuts everything down in the safe order. Safe to call twice. */
    private fun stopRadio() {
        handler.removeCallbacks(notificationUpdater)
        handler.removeCallbacks(gainWatcher)    // M10c
        handler.removeCallbacks(alignWatcher)   // 11c
        gainKeeper = null
        handler.removeCallbacks(wantedChecker)  // 9e
        wantedProgram = -1
        clearPendingSkip()                      // M10a
        handler.removeCallbacks(pauseStopper)
        paused = false
        mutedForGood = false
        pausedByUnplug = false                  // 9h
        stopAtMs = 0L                           // 9h
        audioPlayer.paused = false
        session.isActive = false                // media buttons go to other apps again
        retuneSeq.incrementAndGet()             // M10b: cancel retunes still waiting
        retuning = false
        val conn = connection
        connection = null                       // "radio off" for everything else from now on
        device = null
        // M10b: the native shutdown runs on the engine thread, AFTER whatever it's
        // doing right now (e.g. a retune) - never at the same time. We wait for it
        // (a few seconds at most), because the fd must stay open until it's done.
        try {
            engine.submit(Runnable {
                audioPlayer.stop()                      // 0. stop playing
                if (conn != null) {
                    RadioEngine.stopStreamNative()      // 1. stop USB streaming (+ close nrsc5)
                    RadioEngine.closeDongleNative()     // 2. close librtlsdr + libusb
                }
            }).get(5, TimeUnit.SECONDS)
        } catch (e: Exception) {
            Log.e(TAG, "Engine shutdown didn't finish", e)
        }
        abandonAudioFocus()
        conn?.close()                           // 3. close Android's connection (the fd)
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    /** Stop the radio, remove the notification and end the service. */
    private fun stopEverything() {
        stopRadio()
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    /** Dongle unplugged -> stop cleanly. */
    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val dev = IntentCompat.getParcelableExtra(
                intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            if (dev != null && dev.deviceName == device?.deviceName) {
                Log.i(TAG, "Dongle unplugged - stopping service")
                stopEverything()
            }
        }
    }

    // ------------------------------------------------------------------ pause, presets (M10a)

    /**
     * Is sound coming out? False while paused by you, or while another app
     * has the audio (muted). The notification shows ⏸ when true, ▶ when false.
     */
    fun isPlaying(): Boolean = connection != null && !paused && !audioPlayer.muted

    /**
     * ⏸: silence the radio but keep the tuner running, so ▶ is instantly live.
     * 9h: unplugged = true when headphones / Bluetooth disconnected (the screen
     * says so).
     */
    fun pause(unplugged: Boolean = false) {
        if (connection == null || paused) return
        paused = true
        pausedByUnplug = unplugged
        audioPlayer.paused = true
        Log.i(TAG, "Paused")
        updatePauseTimer()
        updateNotification()
    }

    /**
     * ▶: sound on again. Also asks for audio focus again, so ▶ while another
     * app plays (e.g. after a video took over) takes the sound back.
     */
    fun play() {
        if (connection == null) return
        paused = false
        pausedByUnplug = false                  // 9h
        audioPlayer.paused = false
        applyFocusMode()        // "keep playing along" setting -> just un-mutes
        Log.i(TAG, "Play")
        updatePauseTimer()
        updateNotification()
    }

    /** The in-app ⏸/▶ button. */
    fun togglePause() {
        if (isPlaying()) pause() else play()
    }

    /** 9h: the in-app ⏹ button - the same as ⏹ in the notification. */
    fun stopFromScreen() = stopEverything()

    /**
     * 9h: why no sound is coming out, for the line under the station name:
     * SILENT_NO (playing, or the radio is off), SILENT_PAUSED (you pressed ⏸),
     * SILENT_UNPLUGGED (headphones / Bluetooth disconnected), SILENT_OTHER_APP
     * (another app took the sound for good), SILENT_INTERRUPTED (a call or
     * another short interruption - we get the sound back by ourselves).
     */
    fun silentReason(): Int = when {
        connection == null -> SILENT_NO
        paused -> if (pausedByUnplug) SILENT_UNPLUGGED else SILENT_PAUSED
        audioPlayer.muted -> if (mutedForGood) SILENT_OTHER_APP else SILENT_INTERRUPTED
        else -> SILENT_NO
    }

    /** 9h: milliseconds until the auto-stop turns the radio off, or -1 if it isn't counting. */
    fun stopsInMs(): Long =
        if (stopAtMs > 0) maxOf(0L, stopAtMs - SystemClock.elapsedRealtime()) else -1L

    /**
     * (Re)starts the auto-stop countdown while we're silent - paused by you,
     * or another app took the audio for good - and cancels it otherwise.
     * A short interruption (a call, a navigation voice) doesn't count.
     */
    private fun updatePauseTimer() {
        handler.removeCallbacks(pauseStopper)
        stopAtMs = 0L                                           // 9h
        val minutes = Settings.pauseStopMinutes(this)
        if (connection != null && (paused || mutedForGood) && minutes > 0) {
            handler.postDelayed(pauseStopper, minutes * 60_000L)
            stopAtMs = SystemClock.elapsedRealtime() + minutes * 60_000L
        }
    }

    private val pauseStopper = Runnable {
        Log.i(TAG, "Silent for ${Settings.pauseStopMinutes(this)} min - stopping the radio")
        stopEverything()
    }

    /**
     * ⏮ ⏭: the previous / next preset, in the order of your preset tiles
     * (wrapping around). Not on a preset right now -> ⏭ = the first preset,
     * ⏮ = the last. No presets at all -> the previous / next channel, like
     * ◀ ▶ on the screen.
     * The retune waits SKIP_DELAY_MS for more presses; meanwhile the
     * notification already shows where we're going.
     */
    fun skipPreset(forward: Boolean) {
        if (connection == null) return
        val presets = Presets.load(this)
        if (presets.isEmpty()) {
            val from = if (pendingFreqHz > 0) pendingFreqHz else freqHz
            pendingFreqHz = Settings.band(this).step(from, forward)
            pendingProgram = 0
            pendingIndex = -1
        } else {
            val from = if (pendingFreqHz > 0) pendingIndex
                       else presets.indexOfFirst { it.matches(freqHz, currentProgram()) }
            val size = presets.size
            val to = when {
                from < 0 || from >= size -> if (forward) 0 else size - 1
                forward -> (from + 1) % size
                else -> (from - 1 + size) % size
            }
            pendingIndex = to
            pendingFreqHz = presets[to].freqHz
            pendingProgram = presets[to].program
        }
        Log.i(TAG, "Skip ${if (forward) "next" else "previous"} -> $pendingFreqHz Hz, HD${pendingProgram + 1}")
        handler.removeCallbacks(skipTuner)
        handler.postDelayed(skipTuner, SKIP_DELAY_MS)
        updateNotification()
    }

    private val skipTuner = Runnable {
        val hz = pendingFreqHz
        val program = pendingProgram
        clearPendingSkip()
        if (hz > 0) tune(hz, program)
    }

    private fun clearPendingSkip() {
        handler.removeCallbacks(skipTuner)
        pendingFreqHz = 0
        pendingIndex = -1
    }

    /**
     * The frequency to show on the screen's display: where a ⏮ ⏭ press is
     * about to take us, or else the station we're on.
     */
    fun displayFreqHz(): Int = if (pendingFreqHz > 0) pendingFreqHz else freqHz

    /** Headphones unplugged (or Bluetooth disconnected): pause, don't blast the speaker. */
    private val noisyReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == AudioManager.ACTION_AUDIO_BECOMING_NOISY && isPlaying()) {
                Log.i(TAG, "Headphones unplugged - pausing")
                pause(unplugged = true)
            }
        }
    }

    // ------------------------------------------------------------------ audio focus

    /** Ask Android for audio focus: "I'm about to play music". */
    private fun requestAudioFocus() {
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener(focusListener, handler)
            .build()
        focusRequest = request
        val result = audioManager.requestAudioFocus(request)
        // Not granted (e.g. during a phone call): start muted; we'll be told
        // with AUDIOFOCUS_GAIN when it's our turn.
        audioPlayer.muted = (result != AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
        audioPlayer.volume = 1.0f
        Log.i(TAG, "Audio focus request result: $result")
    }

    private fun abandonAudioFocus() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        focusRequest = null
    }

    /** Android tells us here when another app starts or stops playing. */
    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_GAIN -> {
                // Our turn again (e.g. the Snap video ended): back to normal.
                audioPlayer.muted = false
                audioPlayer.volume = 1.0f
                mutedForGood = false
                Log.i(TAG, "Audio focus: GAIN -> normal")
                updatePauseTimer()          // M10a
                updateNotification()        // M10a: ▶ -> ⏸
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                // Short sound (notification, navigation voice): just get quieter.
                audioPlayer.volume = DUCK_VOLUME
                Log.i(TAG, "Audio focus: LOSS_TRANSIENT_CAN_DUCK -> duck")
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS -> {
                if (change == AudioManager.AUDIOFOCUS_LOSS &&
                    Settings.focusMode(this) == Settings.FOCUS_STOP) {
                    // 9e setting "Stop the radio": another app took over for good
                    // (e.g. you started music or a video) -> stop, like pressing Stop.
                    // Posted, so we don't shut down in the middle of Android's call.
                    Log.i(TAG, "Audio focus: LOSS -> stop (setting)")
                    handler.post { stopEverything() }
                } else {
                    // Another app is playing (a video, a call, another music app).
                    // Live radio can't "pause", so we mute and keep decoding; when
                    // the other app finishes, Android gives us focus back (GAIN)
                    // and the station is simply there again, live.
                    audioPlayer.muted = true
                    Log.i(TAG, "Audio focus: LOSS ($change) -> mute")
                    // M10a: taken for good (not just a call or a voice prompt)
                    // -> counts as paused for the auto-stop setting.
                    mutedForGood = change == AudioManager.AUDIOFOCUS_LOSS
                    updatePauseTimer()
                    updateNotification()    // ⏸ -> ▶ (tap ▶ to take the sound back)
                }
            }
        }
    }

    // ------------------------------------------------------------------ status

    /** Everything MainActivity shows. Called by MainActivity about once a second. */
    fun statusText(): String {
        val nowMs = SystemClock.elapsedRealtime()
        val bytes = RadioEngine.getStreamBytesNative()
        val seconds = (nowMs - lastTimeMs) / 1000.0
        val samplesPerSec = if (seconds > 0) (bytes - lastBytes) / 2.0 / seconds else 0.0
        lastBytes = bytes
        lastTimeMs = nowMs
        val totalSeconds = (nowMs - streamStartMs) / 1000.0
        val avgPerSec = if (totalSeconds > 0) bytes / 2.0 / totalSeconds else 0.0

        if (!RadioEngine.isStreamingNative()) return "$dongleInfo\n\n$streamInfo\n\nStream stopped."

        val bufferedSec = RadioEngine.getAudioBufferedNative() / (AudioPlayer.SAMPLE_RATE * 2.0)
        val audioState = when {
            !audioPlayer.playing -> "buffering..."
            paused -> "PAUSED (tuner still running)"
            audioPlayer.muted -> "MUTED (another app is playing)" +
                if (mutedForGood) " - for good" else ""
            audioPlayer.volume < 1.0f -> "PLAYING (quieter for a moment)"
            else -> "PLAYING"
        }
        return dongleInfo + "\n\n" + streamInfo + "\n\n" +
            "Last second: %.3f M samples/s\n".format(samplesPerSec / 1_000_000) +
            "Average: %.4f M samples/s\n".format(avgPerSec / 1_000_000) +
            "Total received: %.1f MB\n".format(bytes / 1_000_000.0) +
            "Signal level: %.1f".format(RadioEngine.getSignalLevelNative()) +
            "\n\n" + RadioEngine.getStatusNative() +
            "\n\nAudio: %s, buffer %.1f s, gaps %d, dropped %d".format(
                audioState, bufferedSec, audioPlayer.underruns, RadioEngine.getAudioDroppedNative())
    }

    // ------------------------------------------------------------------ notification + MediaSession

    private fun createNotificationChannel() {
        // "Low" importance = shows in the shade without making a sound.
        val channel = NotificationChannel(
            CHANNEL_ID, "Radio playback", NotificationManager.IMPORTANCE_LOW)
        channel.description = "Shows while HiDef Radio is playing"
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    /** Tapping the notification (or the lock-screen player) opens the app. */
    private fun openAppIntent(): PendingIntent = PendingIntent.getActivity(
        this, 0,
        Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE)

    /**
     * M10a: the MediaSession. Android (lock screen, the media player in the
     * notification shade, headphones, Bluetooth, car radios) sends the button
     * presses here. Only "active" while the radio runs (see startRadio/stopRadio).
     */
    private fun createSession() {
        session = MediaSession(this, "HiDefRadio")
        session.setSessionActivity(openAppIntent())
        // All callbacks run on the main thread (our handler), like the rest of this class.
        session.setCallback(object : MediaSession.Callback() {
            override fun onPlay() = play()
            override fun onPause() = pause()
            override fun onStop() = stopEverything()
            override fun onSkipToNext() = skipPreset(forward = true)
            override fun onSkipToPrevious() = skipPreset(forward = false)
            override fun onCustomAction(action: String, extras: Bundle?) {
                if (action == CUSTOM_ACTION_STOP) stopEverything()   // the ⏹ button (Android 13+)
            }

            // 9j: the "Physical media buttons" setting. Hardware keys - headset,
            // Bluetooth, car, and shortcuts some phones make from the volume keys
            // (e.g. a long press with the screen off = "next track") - arrive here
            // first. The notification's buttons and the app's own buttons do NOT
            // come through here, so they always work.
            override fun onMediaButtonEvent(mediaButtonIntent: Intent): Boolean {
                val mode = Settings.mediaButtons(this@RadioService)
                if (mode == Settings.BUTTONS_ALL) return super.onMediaButtonEvent(mediaButtonIntent)
                val key = IntentCompat.getParcelableExtra(
                    mediaButtonIntent, Intent.EXTRA_KEY_EVENT, KeyEvent::class.java)
                    ?: return super.onMediaButtonEvent(mediaButtonIntent)
                val down = key.action == KeyEvent.ACTION_DOWN && key.repeatCount == 0
                if (mode == Settings.BUTTONS_PLAY_PAUSE) {
                    // Play/pause only. Handled here ourselves, because Android's own
                    // handling turns a double-click of a headset button into "next".
                    when (key.keyCode) {
                        KeyEvent.KEYCODE_MEDIA_PLAY -> { if (down) play(); return true }
                        KeyEvent.KEYCODE_MEDIA_PAUSE -> { if (down) pause(); return true }
                        KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_HEADSETHOOK -> {
                            if (down) togglePause()
                            return true
                        }
                    }
                }
                if (down) Log.i(TAG, "Media button ${KeyEvent.keyCodeToString(key.keyCode)} ignored (setting: $mode)")
                return true     // "handled" = ignored
            }
        }, handler)
    }

    /** What the notification shows: two lines of text, a line for car radios, a picture. */
    private data class NowPlaying(
        val title: String,          // line 1: the song, or the station
        val text: String,           // line 2: artist + station, or the program name
        val album: String,          // car radios / Bluetooth: station + program
        val pictureKey: String,     // what the picture was made from ("" = no picture)
    )

    // The picture last put into the notification, and what it was made from.
    private var pictureKey = ""
    private var picture: Bitmap? = null

    // What's in the notification right now - only update it when this changes.
    private var shownKey = ""

    /**
     * Collects what's playing, "song first" (Derek's choice, M10a):
     *   Bulletproof                      no song yet:  KRTH 101.1 HD2
     *   La Roux · KRTH 101.1 HD2                       Top 40
     * While a ⏮ ⏭ press is pending, it shows the preset we're going to.
     */
    private fun nowPlaying(): NowPlaying {
        if (pendingFreqHz > 0) {
            val preset = Presets.load(this).getOrNull(pendingIndex)
            val freq = preset?.freqText ?: "${formatMhz(pendingFreqHz)} HD${pendingProgram + 1}"   // M12: "99.5 FM"
            val title = preset?.title?.ifBlank { null } ?: freq
            return NowPlaying(title, "$freq · tuning…", freq,
                              "cache/$pendingFreqHz/$pendingProgram/${LogoCache.version}")
        }

        val s = signal()
        val callSign = s.station
        // M12 step 2: an analog-only FM station (no HD at all), or the analog while the HD
        // here is another station's -> what the station says over RDS:
        //   Vogue                          no tagged song:  KKLA 99.5 FM
        //   Madonna · KRTH 101.1 FM                         99.5 Find Hope Here! KKLA
        val noHd = callSign.isEmpty() && programs().isEmpty() && !FmBand.isAm(freqHz)
        if (noHd || (s.otherHd && s.analog)) {
            val rdsName = s.rdsName.ifEmpty { s.hdSameCall }
            val station = listOf(rdsName, FmBand.bandText(freqHz)).filter { it.isNotBlank() }.joinToString(" ")
            val type = Rds.ptyName(s.rdsPty, Settings.band(this) == FmBand.AMERICAS)
            val album = if (type.isNotEmpty()) "$station · $type" else station
            val (line1, line2) = when {
                s.rdsTitle.isNotBlank() ->
                    s.rdsTitle to (if (s.rdsArtist.isNotBlank()) "${s.rdsArtist} · $station" else station)
                else -> station to s.rdsRt.ifEmpty { type }
            }
            // The other station's HD logo / art would be the wrong picture -> none then.
            val key = if (s.otherHd) PICTURE_NONE else "live/${pictureGen()}/$freqHz/0/${LogoCache.version}"
            return NowPlaying(line1, line2, album, key)
        }
        val number = currentProgram()
        val program = programs().firstOrNull { it.number == number }
        val mhz = formatMhz(freqHz)
        val station = if (callSign.isNotEmpty()) "$callSign $mhz HD${number + 1}"
                      else "${FmBand.bandText(freqHz)} HD${number + 1}"      // 9c: "1260 AM HD1"
        val name = program?.let { ProgramNames.label(this, freqHz, it, callSign) }.orEmpty()
        val album = if (name.isNotEmpty()) "$station · $name" else station

        val songTitle = program?.title.orEmpty().trim()
        val artist = program?.artist.orEmpty().trim()
        val (line1, line2) = when {
            songTitle.isNotEmpty() ->
                songTitle to (if (artist.isNotEmpty()) "$artist · $station" else station)
            artist.isNotEmpty() -> artist to station
            else -> station to name.ifEmpty { if (s.slogan != callSign) s.slogan else "" }
        }
        return NowPlaying(line1, line2, album, "live/${pictureGen()}/$freqHz/$number/${LogoCache.version}")
    }

    /**
     * The picture: album art, else the program's logo, else the logo saved
     * from last time (LogoCache). Only decoded again when something changed.
     */
    private fun pictureFor(key: String): Bitmap? {
        if (key == pictureKey) return picture
        pictureKey = key
        if (key == PICTURE_NONE) {                  // M12 step 2
            picture = null
            return null
        }
        val bytes = if (pendingFreqHz > 0) {
            LogoCache.load(this, pendingFreqHz, pendingProgram)
        } else {
            val number = currentProgram()
            art(number) ?: logo(number) ?: LogoCache.load(this, freqHz, number)
        }
        picture = bytes?.let { shrink(BitmapFactory.decodeByteArray(it, 0, it.size)) }
        return picture
    }

    /** Big pictures slow Android down (they're copied to the system) - keep them ≤ 720 px. */
    private fun shrink(bitmap: Bitmap?): Bitmap? {
        if (bitmap == null) return null
        val longest = maxOf(bitmap.width, bitmap.height)
        if (longest <= MAX_PICTURE_PX) return bitmap
        val scale = MAX_PICTURE_PX.toFloat() / longest
        return Bitmap.createScaledBitmap(bitmap,
            (bitmap.width * scale).toInt().coerceAtLeast(1),
            (bitmap.height * scale).toInt().coerceAtLeast(1), true)
    }

    /** One notification button: sends [action] back to this service. */
    private fun button(iconId: Int, labelId: Int, action: String, requestCode: Int): Notification.Action {
        val intent = PendingIntent.getService(
            this, requestCode,
            Intent(this, RadioService::class.java).setAction(action),
            PendingIntent.FLAG_IMMUTABLE)
        return Notification.Action.Builder(
            Icon.createWithResource(this, iconId), getString(labelId), intent).build()
    }

    private fun buildNotification(now: NowPlaying, bitmap: Bitmap?, playing: Boolean): Notification {
        val playPause = if (playing) button(R.drawable.ic_pause, R.string.pause, ACTION_PAUSE, 11)
                        else button(R.drawable.ic_play, R.string.play, ACTION_PLAY, 12)
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)   // white "Hd" (M10d)
            .setContentTitle(now.title)
            .setContentText(now.text)
            .setLargeIcon(bitmap)
            .setContentIntent(openAppIntent())
            // Buttons 0-3. Android 12 and older show these; Android 13+ builds
            // its own from the MediaSession (same buttons, see updateSession).
            .addAction(button(R.drawable.ic_skip_previous, R.string.previous, ACTION_PREVIOUS, 10))
            .addAction(playPause)
            .addAction(button(R.drawable.ic_skip_next, R.string.next, ACTION_NEXT, 13))
            .addAction(button(R.drawable.ic_stop, R.string.stop, ACTION_STOP, 1))
            // The media look: ⏮ ⏸ ⏭ in the small view, linked to our MediaSession
            // (that's what puts it on the lock screen and in the media player).
            .setStyle(Notification.MediaStyle()
                .setMediaSession(session.sessionToken)
                .setShowActionsInCompactView(0, 1, 2))
            .setCategory(Notification.CATEGORY_TRANSPORT)
            .setVisibility(Notification.VISIBILITY_PUBLIC)    // show it on the lock screen
            .setShowWhen(false)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }

    /** Tells the MediaSession what's playing and which buttons work. */
    private fun updateSession(now: NowPlaying, bitmap: Bitmap?, playing: Boolean) {
        val metadata = MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, now.title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, now.text)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, now.album)
        if (bitmap != null) {
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ART, bitmap)
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, bitmap)
        }
        // (No duration: it's live radio, so Android shows no progress bar.)
        session.setMetadata(metadata.build())

        val state = PlaybackState.Builder()
            .setActions(PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or
                        PlaybackState.ACTION_PLAY_PAUSE or PlaybackState.ACTION_STOP or
                        PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS)
            .setState(if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED,
                      PlaybackState.PLAYBACK_POSITION_UNKNOWN, if (playing) 1f else 0f)
            // Android 13+ doesn't show a Stop button by itself - a "custom action" does.
            .addCustomAction(PlaybackState.CustomAction.Builder(
                CUSTOM_ACTION_STOP, getString(R.string.stop), R.drawable.ic_stop).build())
            .build()
        session.setPlaybackState(state)
    }

    private fun goForeground(text: String) {
        val notification = buildNotification(
            NowPlaying("HiDef Radio", text, "", ""), null, playing = true)
        // Android 10+ wants to be told what KIND of foreground service this is.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        shownKey = ""               // the next update must replace "Starting…"
    }

    /**
     * Every 2 s: check whether the song, station, picture or ⏸/▶ changed
     * (and save new logos). The notification is only redrawn when something did.
     */
    private val notificationUpdater = object : Runnable {
        override fun run() {
            updateNotification()
            saveNewLogos()          // 9d step 3
            handler.postDelayed(this, NOTIFICATION_EVERY_MS)
        }
    }

    private fun updateNotification() {
        if (connection == null) return
        val now = nowPlaying()
        val playing = isPlaying()
        val key = "$now|$playing"
        if (key == shownKey) return                 // nothing new
        shownKey = key
        val bitmap = pictureFor(now.pictureKey)
        updateSession(now, bitmap, playing)
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(now, bitmap, playing))
    }

    companion object {
        private const val TAG = "HiDefRadio"
        const val ACTION_START = "io.github.derek20la.hidefradio.START"
        const val ACTION_STOP = "io.github.derek20la.hidefradio.STOP"
        // M10a: the notification's other buttons
        const val ACTION_PLAY = "io.github.derek20la.hidefradio.PLAY"
        const val ACTION_PAUSE = "io.github.derek20la.hidefradio.PAUSE"
        const val ACTION_NEXT = "io.github.derek20la.hidefradio.NEXT"
        const val ACTION_PREVIOUS = "io.github.derek20la.hidefradio.PREVIOUS"
        private const val CUSTOM_ACTION_STOP = "stop"     // the ⏹ button on Android 13+

        // 9h: silentReason() values
        const val SILENT_NO = 0
        const val SILENT_PAUSED = 1
        const val SILENT_UNPLUGGED = 2
        const val SILENT_OTHER_APP = 3
        const val SILENT_INTERRUPTED = 4
        const val EXTRA_DEVICE = "device"
        private const val CHANNEL_ID = "radio"
        private const val NOTIFICATION_ID = 1
        private const val NOTIFICATION_EVERY_MS = 2_000L  // M10a: was 5 s (only redraws on changes now)
        private const val SKIP_DELAY_MS = 600L            // M10a: wait for more ⏮ ⏭ presses
        private const val MAX_PICTURE_PX = 720            // M10a: notification picture size limit
        private const val PICTURE_NONE = "none"           // M12 step 2: nowPlaying() wants no picture
        private const val GAIN_TICK_MS = 1_000L           // M10c: gain watchdog, once a second
        private const val ALIGN_TICK_MS = 2_000L          // 11c: ask the aligner every 2 s
        private const val REMEMBER_GAIN_AFTER_MS = 20_000L   // M10c: gain unchanged this long -> remember it

        // First-run station: 101.1 MHz KRTH (K-EARTH 101), HD1 + HD2.
        // Decodes perfectly in nrsc5-dui on Derek's Mac: MER ~14 dB, BER 0.000%.
        // After that, the last tuned frequency is remembered (see saveFrequency).
        const val DEFAULT_FREQ_HZ = 101_100_000

        // (9a's band limits and ◀ ▶ step moved to FmBand.kt in 9e - now a setting.)

        // Where the last frequency is saved (a small key/value file in app storage).
        private const val PREFS = "radio"
        private const val KEY_FREQ = "freq_hz"
        private const val KEY_LAST_FM = "last_fm_hz"   // 9c
        private const val KEY_LAST_AM = "last_am_hz"   // 9c
        private const val KEY_PROGRAM = "program"     // 9e: 0 = HD1, 1 = HD2 ...

        // 9e: how long to wait for a wanted program (e.g. a preset's HD2).
        private const val WANTED_GRACE_MS = 5_000L    // after the station's audio started
        private const val WANTED_TIMEOUT_MS = 30_000L // in any case

        fun saveFrequency(context: Context, hz: Int) {
            val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            val editor = prefs.edit()
            // 9c step 2: first file the station we're LEAVING under its own band (FM or
            // AM). Otherwise a station the app merely started on (restored at start-up,
            // never "tuned") would be forgotten: Derek was on 90.3, went to AM, and the
            // FM button then offered 101.1.
            val old = prefs.getInt(KEY_FREQ, -1)
            if (old > 0) editor.putInt(if (FmBand.isAm(old)) KEY_LAST_AM else KEY_LAST_FM, old)
            editor.putInt(KEY_FREQ, hz)
                .putInt(if (FmBand.isAm(hz)) KEY_LAST_AM else KEY_LAST_FM, hz)   // 9c
                .apply()
        }

        /**
         * 9c: the last frequency you had on AM ([am] = true) or FM - the FM / AM
         * buttons in the tuning window go back there. Never used yet: FM 101.1,
         * AM the lowest channel of your region's band.
         */
        fun lastFrequency(context: Context, am: Boolean): Int {
            val default = if (am) Settings.band(context).amChannels.first() else DEFAULT_FREQ_HZ
            return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getInt(if (am) KEY_LAST_AM else KEY_LAST_FM, default)
        }

        fun loadFrequency(context: Context): Int =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getInt(KEY_FREQ, DEFAULT_FREQ_HZ)

        /** 9e: the program you listened to last (so the app restarts on HD2 if you left it there). */
        fun saveProgram(context: Context, program: Int) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putInt(KEY_PROGRAM, program).apply()
        }

        fun loadProgram(context: Context): Int =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getInt(KEY_PROGRAM, 0)

        /** 101_100_000 -> "101.1" */
        fun formatMhz(hz: Int): String = FmBand.freqText(hz)    // 9i: "87.75" when needed; 9c: AM "1260" (kHz)

        // (9e: the tuner gain is now a setting - see Settings.gainTenths().
        // Auto = our port of nrsc5's auto-gain. Gain 0 was far too quiet: MER ~5 dB.)

        // Volume while "ducked" under a short sound (1.0 = normal).
        private const val DUCK_VOLUME = 0.2f

        /** The running service, or null. MainActivity uses this to show status. */
        @Volatile var instance: RadioService? = null
            private set
    }
}
