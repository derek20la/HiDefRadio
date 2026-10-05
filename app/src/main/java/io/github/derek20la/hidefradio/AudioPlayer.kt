package io.github.derek20la.hidefradio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Process
import android.os.SystemClock
import android.util.Log

/**
 * Plays the decoded HD Radio audio (Milestone 7).
 *
 * The "pull" model: a background thread repeatedly PULLS audio out of the
 * native ring buffer (via [readAudio]) and writes it to an Android AudioTrack.
 * AudioTrack.write() blocks while its own buffer is full, which naturally
 * paces the loop to the speed the phone plays audio at.
 *
 * @param readAudio      copies up to buffer.size audio values into buffer,
 *                       waiting up to timeoutMs; returns how many were copied.
 * @param bufferedValues how many audio values are waiting in the native buffer.
 */
class AudioPlayer(
    private val readAudio: (buffer: ShortArray, timeoutMs: Int) -> Int,
    private val bufferedValues: () -> Int,
) {
    @Volatile private var running = false
    private var thread: Thread? = null

    /** How many times the loop found no audio waiting (a gap / silence). */
    @Volatile var underruns = 0
        private set

    /** True once pre-buffering is done and sound is playing. */
    @Volatile var playing = false
        private set

    // ---- Milestone 8c: audio focus (set by RadioService) ----
    /** Playback volume: 1.0 = normal, e.g. 0.2 = "ducked" under a notification sound. */
    @Volatile var volume = 1.0f

    /** Muted = keep playing silently (live radio keeps going), e.g. during a Snap video. */
    @Volatile var muted = false

    /**
     * M10a: paused by YOU (the ⏸ button, headphones unplugged...). Works like
     * [muted]: the radio keeps decoding silently, so ▶ is instantly live again.
     * Kept separate from [muted], so audio focus and pause can't undo each other.
     */
    @Volatile var paused = false

    /**
     * M10b: when the sound started (SystemClock.elapsedRealtime), 0 = not yet.
     * RadioService uses it for "sound after ... s" in the signal details.
     */
    @Volatile var startedAtMs = 0L
        private set

    fun start() {
        if (running) return
        running = true
        underruns = 0
        startedAtMs = 0L
        thread = Thread({ playLoop() }, "HiDefAudio").apply { start() }
    }

    /** Stops playback and waits (up to 1 s) for the audio thread to finish. */
    fun stop() {
        running = false
        thread?.join(1000)
        thread = null
    }

    private fun playLoop() {
        // Ask Android to treat this thread as audio (fewer interruptions).
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)

        val minBytes = AudioTrack.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build())
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)   // 16-bit samples
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build())
            .setTransferMode(AudioTrack.MODE_STREAM)                // we keep feeding it
            .setBufferSizeInBytes(maxOf(minBytes * 4, QUARTER_SECOND_BYTES))
            .build()

        try {
            // Pre-buffer: wait until PREBUFFER_VALUES of audio are saved up before
            // starting, so small gaps in the radio data don't cause clicks.
            // (M10b: 0.25 s, was 0.5 s. nrsc5 hands out audio steadily - 2 frames
            // every ~93 ms - so a shorter cushion is enough. If "gaps" in the
            // debug text start counting up, make it longer again.)
            while (running && bufferedValues() < PREBUFFER_VALUES) {
                Thread.sleep(20)
            }
            if (!running) return
            track.play()
            playing = true
            startedAtMs = SystemClock.elapsedRealtime()
            Log.i(TAG, "Audio playback started")

            val buffer = ShortArray(4096)   // ~46 ms of stereo audio per chunk
            var appliedVolume = -1f
            while (running) {
                // Apply mute / duck changes. We keep writing audio even when
                // muted (at volume 0), so the radio stays "live" and un-muting
                // is instant - no catching up, no re-buffering.
                val targetVolume = if (muted || paused) 0f else volume
                if (targetVolume != appliedVolume) {
                    track.setVolume(targetVolume)
                    appliedVolume = targetVolume
                }

                val n = readAudio(buffer, 200)   // wait up to 200 ms for audio
                if (n > 0) {
                    track.write(buffer, 0, n)    // blocks until AudioTrack has room
                } else {
                    underruns++                  // nothing arrived in 200 ms
                }
            }
        } finally {
            playing = false
            try { track.stop() } catch (e: IllegalStateException) { /* never started */ }
            track.release()
            Log.i(TAG, "Audio playback stopped")
        }
    }

    companion object {
        private const val TAG = "HiDefRadio"
        const val SAMPLE_RATE = 44_100                         // HD Radio audio rate
        private const val PREBUFFER_VALUES = SAMPLE_RATE * 2 / 4   // 0.25 s, stereo (M10b: was 0.5 s)
        private const val QUARTER_SECOND_BYTES = SAMPLE_RATE * 2 * 2 / 4
    }
}
