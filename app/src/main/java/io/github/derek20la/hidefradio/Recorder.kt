package io.github.derek20la.hidefradio

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.DocumentsContract
import android.text.format.Formatter
import android.util.Log
import android.widget.Toast
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 1.3 (build 34): recording - the ⏺ button on the main screen.
 *
 * Two kinds, chosen in Settings -> Recording -> "⏺ records":
 *  - AUDIO: what you hear (HD, analog or the blend), as an .aac file (AacWriter.kt),
 *    about 1 MB a minute. The copy is taken where the audio goes to the speaker
 *    (AudioPlayer.tap), so it keeps recording while the sound is paused or muted.
 *  - IQ: the raw samples from the dongle / rtl_tcp server, untouched (cpp/iqrecord.hpp), as
 *    a .cu8 file - about 180 MB a minute, so it stops by itself after the IQ limit
 *    (Settings, 60 s unless you choose otherwise).
 *  - or both at once.
 *
 * WHERE THE FILES GO: a folder you pick once (Android's folder picker, [Recordings]). The app
 * gets access to that folder only - it still has no storage permission.
 *
 * WHEN A RECORDING ENDS: ⏺ again; another station (or anything else that restarts the
 * stream); the radio stopped; or for IQ the limit, a full storage, or storage too slow for
 * 3 MB a second. Switching HD1 -> HD2 does NOT end it (same station, same file).
 *
 * One per RadioService; main thread only, except where noted. Finishing a file (encoding
 * the last audio, writing the rest of the IQ buffer) can take a moment, so that is done on
 * a short-lived thread of its own; the message ("Saved ...") comes when it's done.
 */
class Recorder(private val context: Context, private val player: AudioPlayer) {

    private val handler = Handler(Looper.getMainLooper())

    private var aac: AacWriter? = null
    private var audioUri: Uri? = null
    private var audioName = ""
    private var audioStartMs = 0L

    private var iqOn = false
    private var iqUri: Uri? = null
    private var iqName = ""
    private var iqStartMs = 0L
    private var iqLimitS = 0

    /** Something is being recorded (the ⏺ button is red). */
    val recording: Boolean
        get() = aac != null || iqOn

    /** How long the recording has been going (the oldest of the two), for the timer. */
    fun elapsedMs(): Long {
        val now = SystemClock.elapsedRealtime()
        val starts = listOfNotNull(audioStartMs.takeIf { aac != null }, iqStartMs.takeIf { iqOn })
        return starts.minOrNull()?.let { now - it } ?: 0L
    }

    /** "IQ" while only IQ is being recorded (the screen shows its limit next to the timer). */
    fun iqOnly(): Boolean = iqOn && aac == null
    fun iqLimitS(): Int = iqLimitS

    /**
     * Starts recording [what] (Settings.REC_AUDIO / REC_IQ / REC_BOTH). [hz], [station] and
     * [program] (0 = HD1) only go into the file names. Returns null when it started, else the
     * message to show (no folder, the folder is gone, the radio isn't playing yet ...).
     */
    fun start(what: String, hz: Int, station: String, program: Int): String? {
        if (recording) return null
        val folder = Recordings.folder(context) ?: return context.getString(R.string.rec_no_folder)
        // A recording that ended while nobody looked (the service stopped meanwhile): free it.
        RadioEngine.takeIqRecordNative()

        val time = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(Date())
        val freq = if (FmBand.isAm(hz)) "${FmBand.freqText(hz)}kHz" else "${FmBand.freqText(hz)}MHz"
        val wantAudio = what != Settings.REC_IQ
        val wantIq = what != Settings.REC_AUDIO
        var problem: String? = null

        if (wantIq) {
            iqLimitS = Settings.recIqLimitS(context)
            val name = "HiDefRadio_${time}_${freq}_${IQ_RATE}sps.cu8"
            val uri = Recordings.create(context, folder, name, MIME_IQ)
            if (uri == null) {
                problem = context.getString(R.string.rec_folder_gone)
            } else {
                val r = try {
                    val pfd = context.contentResolver.openFileDescriptor(uri, "w")
                    if (pfd == null) -1 else RadioEngine.startIqRecordNative(pfd.detachFd(), iqLimitS)
                } catch (e: Exception) {
                    Log.e(TAG, "IQ file", e)
                    -1
                }
                if (r == 0) {
                    iqOn = true
                    iqUri = uri
                    iqName = Recordings.nameOf(context, uri) ?: name
                    iqStartMs = SystemClock.elapsedRealtime()
                } else {
                    Recordings.delete(context, uri)               // an empty file helps nobody
                    problem = context.getString(when (r) {
                        1 -> R.string.rec_not_playing
                        3 -> R.string.rec_iq_no_memory
                        else -> R.string.rec_cant_write
                    })
                }
            }
        }

        if (wantAudio && problem == null) {
            val tag = clean(station).let { if (it.isEmpty()) "" else "_$it" } +
                      (if (program > 0) "_HD${program + 1}" else "")
            val name = "HiDefRadio_${time}_${freq}$tag.aac"
            val uri = Recordings.create(context, folder, name, MIME_AUDIO)
            val stream = try {
                uri?.let { context.contentResolver.openOutputStream(it, "w") }
            } catch (e: Exception) {
                Log.e(TAG, "Audio file", e)
                null
            }
            if (uri == null || stream == null) {
                uri?.let { Recordings.delete(context, it) }
                problem = context.getString(if (uri == null) R.string.rec_folder_gone else R.string.rec_cant_write)
            } else {
                val writer = AacWriter(stream)
                writer.start()
                aac = writer
                audioUri = uri
                audioName = Recordings.nameOf(context, uri) ?: name
                audioStartMs = SystemClock.elapsedRealtime()
                player.tap = { buffer, count -> writer.feed(buffer, count) }
            }
        }

        if (problem != null) {
            stop(quiet = true)                      // the half that did start, if any: removed again
            return problem
        }
        handler.removeCallbacks(watcher)
        handler.postDelayed(watcher, WATCH_MS)
        Log.i(TAG, "Recording started: ${if (aac != null) audioName else ""} ${if (iqOn) iqName else ""}")
        return null
    }

    /**
     * Ends everything that is being recorded and says what was saved ("Saved 3.2 MB: ...").
     * [quiet] = a start that only half worked: no message, and the files are removed again.
     */
    fun stop(quiet: Boolean = false) {
        handler.removeCallbacks(watcher)
        val writer = aac
        val wasIq = iqOn
        if (writer == null && !wasIq) return
        player.tap = null
        val audio = writer?.let { Saved(audioName, audioUri) }
        val iq = if (wasIq) Saved(iqName, iqUri) else null
        aac = null
        iqOn = false
        Thread({
            writer?.finish()
            if (wasIq) RadioEngine.stopIqRecordNative()      // waits until the file is complete
            val iqResult = if (wasIq) RadioEngine.takeIqRecordNative() else null
            handler.post {
                if (quiet) {
                    audio?.uri?.let { Recordings.delete(context, it) }
                    iq?.uri?.let { Recordings.delete(context, it) }
                } else {
                    report(audio?.copy(bytes = writer?.bytes ?: 0L, failed = writer?.failed == true),
                           iq?.copy(bytes = iqResult?.get(0) ?: 0L, reason = (iqResult?.get(1) ?: 1L).toInt()))
                }
            }
        }, "HiDefRecStop").start()
    }

    /** Once a second while recording: did the IQ stop by itself (limit ...), did the audio fail? */
    private val watcher = object : Runnable {
        override fun run() {
            if (iqOn && RadioEngine.getIqRecordNative()[0] == STATE_ENDED) {
                val result = RadioEngine.takeIqRecordNative()
                iqOn = false
                if (result != null) {
                    report(null, Saved(iqName, iqUri, bytes = result[0], reason = result[1].toInt()),
                           audioGoesOn = aac != null)
                }
            }
            val writer = aac
            if (writer != null && writer.failed) {
                player.tap = null
                aac = null
                report(Saved(audioName, audioUri, bytes = writer.bytes, failed = true), null)
            }
            if (recording) handler.postDelayed(this, WATCH_MS)
        }
    }

    /** What one finished file was. */
    private data class Saved(
        val name: String, val uri: Uri?,
        val bytes: Long = 0, val failed: Boolean = false, val reason: Int = REASON_BY_YOU,
    )

    /** Says what was saved, in one message. Empty files are deleted. */
    private fun report(audio: Saved?, iq: Saved?, audioGoesOn: Boolean = false) {
        val lines = mutableListOf<String>()
        if (audio != null) {
            lines += when {
                audio.bytes == 0L -> { audio.uri?.let { Recordings.delete(context, it) }
                                       context.getString(R.string.rec_audio_failed_empty) }
                audio.failed -> context.getString(R.string.rec_audio_failed, size(audio.bytes), audio.name)
                else -> context.getString(R.string.rec_saved, size(audio.bytes), audio.name)
            }
        }
        if (iq != null) {
            if (iq.bytes == 0L) iq.uri?.let { Recordings.delete(context, it) }
            lines += when {
                iq.bytes == 0L -> context.getString(R.string.rec_iq_failed_empty)
                iq.reason == REASON_LIMIT ->
                    context.getString(R.string.rec_iq_limit, limitText(context, iqLimitS), size(iq.bytes), iq.name)
                iq.reason == REASON_TOO_SLOW -> context.getString(R.string.rec_iq_too_slow, size(iq.bytes), iq.name)
                iq.reason == REASON_WRITE_FAILED -> context.getString(R.string.rec_iq_write_failed, size(iq.bytes), iq.name)
                else -> context.getString(R.string.rec_saved, size(iq.bytes), iq.name)
            }
            if (audioGoesOn) lines += context.getString(R.string.rec_audio_goes_on)
        }
        if (lines.isNotEmpty()) Toast.makeText(context, lines.joinToString("\n"), Toast.LENGTH_LONG).show()
    }

    private fun size(bytes: Long): String = Formatter.formatShortFileSize(context, bytes)

    companion object {
        private const val TAG = "HiDefRadio"
        private const val WATCH_MS = 1_000L
        const val IQ_RATE = 1_488_375                 // samples a second (native-lib.cpp SAMPLE_RATE)
        const val IQ_BYTES_PER_S = IQ_RATE * 2        // 2,976,750: I and Q, one byte each
        private const val MIME_AUDIO = "audio/aac"
        // Raw samples have no MIME type of their own. With this one, Android keeps the name
        // exactly as given (".cu8" - it doesn't add ".bin").
        private const val MIME_IQ = "application/octet-stream"

        // cpp/iqrecord.hpp: State and Reason
        private const val STATE_ENDED = 2L
        private const val REASON_BY_YOU = 1
        private const val REASON_LIMIT = 2
        private const val REASON_WRITE_FAILED = 3
        private const val REASON_TOO_SLOW = 4

        /** "KRTH-FM" stays, "K-Love 99.5/HD" -> "K-Love_99.5_HD": safe in a file name everywhere. */
        fun clean(text: String): String =
            text.trim().replace(Regex("[^A-Za-z0-9.+-]+"), "_").trim('_').take(40)

        /** 60 -> "60 s", 300 -> "5 min" (for the messages). */
        fun limitText(context: Context, seconds: Int): String =
            if (seconds % 60 == 0 && seconds >= 120) context.getString(R.string.rec_minutes, seconds / 60)
            else context.getString(R.string.rec_seconds, seconds)
    }
}

/**
 * 1.3 (build 34): the recordings folder - picked once with Android's folder picker
 * (ACTION_OPEN_DOCUMENT_TREE: Settings -> Recording -> Recordings folder, or the first ⏺).
 * Android then lets the app create files in that folder (and only there) for good, even
 * after a restart; the folder's address is kept in the settings (Settings.KEY_REC_FOLDER).
 */
object Recordings {

    /** The chosen folder, or null: none chosen yet, or the app can't write there any more. */
    fun folder(context: Context): Uri? {
        val text = Settings.prefs(context).getString(Settings.KEY_REC_FOLDER, null) ?: return null
        val uri = Uri.parse(text)
        val granted = context.contentResolver.persistedUriPermissions
            .any { it.uri == uri && it.isWritePermission }
        return if (granted) uri else null
    }

    /** Keeps [uri] (from the folder picker) as the recordings folder. False = Android refused. */
    fun setFolder(context: Context, uri: Uri): Boolean {
        val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        return try {
            context.contentResolver.takePersistableUriPermission(uri, flags)
            // Let go of the old folder (an app may only keep a limited number of them).
            val old = Settings.prefs(context).getString(Settings.KEY_REC_FOLDER, null)?.let { Uri.parse(it) }
            if (old != null && old != uri) {
                try { context.contentResolver.releasePersistableUriPermission(old, flags) } catch (e: Exception) { }
            }
            Settings.prefs(context).edit().putString(Settings.KEY_REC_FOLDER, uri.toString()).apply()
            true
        } catch (e: SecurityException) {
            Log.e("HiDefRadio", "Folder permission", e)
            false
        }
    }

    /** The folder's name as Android shows it ("Recordings", "HiDef"), or null. */
    fun folderName(context: Context): String? {
        val tree = folder(context) ?: return null
        val doc = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        return nameOf(context, doc) ?: tree.lastPathSegment
    }

    /** A new, empty file in [tree] (Android adds " (1)" if the name is taken). null = failed. */
    fun create(context: Context, tree: Uri, name: String, mime: String): Uri? = try {
        val parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        DocumentsContract.createDocument(context.contentResolver, parent, mime, name)
    } catch (e: Exception) {                  // folder deleted, SD card taken out, ...
        Log.e("HiDefRadio", "Create $name", e)
        null
    }

    fun delete(context: Context, uri: Uri) {
        try { DocumentsContract.deleteDocument(context.contentResolver, uri) } catch (e: Exception) { }
    }

    /** A file's or folder's name as shown, or null. */
    fun nameOf(context: Context, uri: Uri): String? = try {
        context.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
                                      null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
    } catch (e: Exception) {
        null
    }
}
