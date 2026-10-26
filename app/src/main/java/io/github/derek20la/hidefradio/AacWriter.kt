package io.github.derek20la.hidefradio

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Process
import android.util.Log
import java.io.BufferedOutputStream
import java.io.OutputStream
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * 1.3 (build 34): turns the radio's audio into an AAC file (.aac), on a thread of its own.
 *
 * The audio thread hands over each chunk it plays ([feed] - a copy, at once, never waits);
 * this thread gives the chunks to Android's own AAC encoder (MediaCodec - on most phones a
 * hardware encoder, so it costs very little battery) and writes what comes out to the file.
 *
 * WHY .aac (ADTS) AND NOT .m4a
 *   An .aac file is just the encoded frames one after the other, each with a 7-byte header
 *   (the "ADTS" header) that says what it is. So the file is written straight through, start
 *   to end, and is playable at every moment: if the phone runs out of battery or Android
 *   closes the app in the middle of a recording, everything up to then plays fine. (An .m4a
 *   needs its table of contents written at the END - cut off, it doesn't play at all.) It is
 *   also what phones' own FM radio apps save, and every player knows it.
 *
 * Format: AAC-LC, 44,100 Hz (the radio's own rate - nothing is resampled), stereo,
 * [BIT_RATE] = 128 kbit/s, about 1 MB a minute.
 *
 * @param out where the file goes (Recorder.kt opens it in the folder you picked).
 *            Closed by this class when the recording ends.
 */
class AacWriter(out: OutputStream) {

    private val out = BufferedOutputStream(out, 64 * 1024)
    private val queue = ArrayBlockingQueue<ShortArray>(QUEUE_CHUNKS)
    @Volatile private var running = true
    private val thread = Thread({ encodeLoop() }, "HiDefRecord")

    /** True if the encoder or the file failed - the recording has stopped (Recorder asks). */
    @Volatile var failed = false
        private set

    /** Bytes written to the file so far. */
    @Volatile var bytes = 0L
        private set

    /** Chunks thrown away because the encoder fell behind (should stay 0). */
    @Volatile var dropped = 0
        private set

    fun start() = thread.start()

    /** The audio thread: one chunk of 16-bit stereo audio ([count] values). Copies; never waits. */
    fun feed(buffer: ShortArray, count: Int) {
        if (!running || failed || count <= 0) return
        if (!queue.offer(buffer.copyOf(count))) dropped++
    }

    /**
     * Ends the recording: what was fed so far is encoded and written, then the file is
     * closed. Waits for that (well under a second) - call it off the main thread.
     */
    fun finish() {
        running = false
        thread.join(FINISH_WAIT_MS)
    }

    private fun encodeLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
        var codec: MediaCodec? = null
        try {
            val format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AAC, AudioPlayer.SAMPLE_RATE, CHANNELS).apply {
                setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                setInteger(MediaFormat.KEY_BIT_RATE, BIT_RATE)
                setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16 * 1024)
            }
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()

            val info = MediaCodec.BufferInfo()
            var chunk: ShortArray? = null      // the chunk being handed to the encoder
            var chunkPos = 0                   // how much of it is handed over already
            var framesIn = 0L                  // stereo frames given to the encoder (for the time stamps)
            var endSent = false
            var waitsAfterEnd = 0              // safety: an encoder that never says "end" (3 s)
            var done = false
            while (!done) {
                // 1. Audio in.
                if (!endSent) {
                    if (chunk == null) chunk = queue.poll(20, TimeUnit.MILLISECONDS)
                    val c = chunk
                    if (c != null) {
                        val index = codec.dequeueInputBuffer(10_000)
                        if (index >= 0) {
                            val input = codec.getInputBuffer(index)!!
                            input.clear()
                            input.order(ByteOrder.nativeOrder())
                            // Whole stereo frames only (2 values = 4 bytes each).
                            val room = input.remaining() / 4 * 2
                            val n = minOf(room, c.size - chunkPos)
                            input.asShortBuffer().put(c, chunkPos, n)
                            val timeUs = framesIn * 1_000_000L / AudioPlayer.SAMPLE_RATE
                            codec.queueInputBuffer(index, 0, n * 2, timeUs, 0)
                            framesIn += n / 2
                            chunkPos += n
                            if (chunkPos >= c.size) { chunk = null; chunkPos = 0 }
                        }
                    } else if (!running && queue.isEmpty()) {
                        // Stopped and everything is in: tell the encoder that's the end.
                        val index = codec.dequeueInputBuffer(10_000)
                        if (index >= 0) {
                            val timeUs = framesIn * 1_000_000L / AudioPlayer.SAMPLE_RATE
                            codec.queueInputBuffer(index, 0, 0, timeUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            endSent = true
                        }
                    }
                }
                // 2. AAC out, as much as is ready.
                while (true) {
                    val index = codec.dequeueOutputBuffer(info, if (endSent) 10_000 else 0)
                    if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
                        if (endSent && ++waitsAfterEnd > 300) done = true
                        break
                    }
                    if (index < 0) continue        // "format changed" etc.: nothing to write
                    val output = codec.getOutputBuffer(index)!!
                    // The "codec config" buffer describes the stream for .m4a files; an .aac
                    // file says the same in every frame's ADTS header instead.
                    if ((info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0 && info.size > 0) {
                        val frame = ByteArray(info.size)
                        output.position(info.offset)
                        output.get(frame)
                        out.write(adtsHeader(info.size))
                        out.write(frame)
                        bytes += ADTS_HEADER_BYTES + info.size
                    }
                    codec.releaseOutputBuffer(index, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) { done = true; break }
                }
            }
        } catch (e: Exception) {              // no encoder, the file can't be written, ...
            Log.e(TAG, "Audio recording failed", e)
            failed = true
        } finally {
            try { codec?.stop() } catch (e: Exception) { /* never started */ }
            codec?.release()
            try { out.close() } catch (e: Exception) { failed = true }
            queue.clear()
        }
    }

    companion object {
        private const val TAG = "HiDefRadio"
        const val BIT_RATE = 128_000                 // about 1 MB a minute
        private const val CHANNELS = 2
        private const val QUEUE_CHUNKS = 256         // ~12 s of audio waiting, at most
        private const val FINISH_WAIT_MS = 5_000L
        private const val ADTS_HEADER_BYTES = 7

        // ADTS header fields for our format.
        private const val PROFILE_LC = 2             // AAC-LC (written as profile - 1)
        private const val RATE_INDEX_44100 = 4       // index of 44,100 Hz in the ADTS rate table

        /**
         * The 7-byte ADTS header in front of each AAC frame of [size] bytes: "MPEG-4 AAC-LC,
         * 44.1 kHz, stereo, this frame is N bytes long (header included), no CRC".
         */
        fun adtsHeader(size: Int): ByteArray {
            val length = size + ADTS_HEADER_BYTES
            return byteArrayOf(
                0xFF.toByte(),                                                    // sync word ...
                0xF1.toByte(),                                                    // ... MPEG-4, no CRC
                (((PROFILE_LC - 1) shl 6) or (RATE_INDEX_44100 shl 2) or (CHANNELS shr 2)).toByte(),
                (((CHANNELS and 3) shl 6) or (length shr 11)).toByte(),
                ((length shr 3) and 0xFF).toByte(),
                (((length and 7) shl 5) or 0x1F).toByte(),
                0xFC.toByte(),                                                    // buffer fullness "variable", 1 frame
            )
        }
    }
}
