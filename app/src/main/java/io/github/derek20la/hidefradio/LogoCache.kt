package io.github.derek20la.hidefradio

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Milestone 9d step 3 (decision 17): remembers every station logo we receive,
 * one file per frequency + program, e.g. "101.1-HD2.img" in the app's private
 * storage (files/logos/).
 *
 * Why: logos take a while to arrive after tuning (stations repeat them every
 * few minutes). With the cache the screen shows the logo straight away, and
 * (9e) the preset tiles show it as a thumbnail even when you're not tuned in.
 *
 * The files are whatever the station sent (PNG or JPEG) - Android works that
 * out by itself when it decodes them, so they all end in ".img".
 */
object LogoCache {

    private const val TAG = "HiDefRadio"

    /**
     * 9e step 1: goes up by one whenever a logo is saved, so the preset tiles
     * know when to reload their thumbnails. (The service and the screen run in
     * the same app process, so they see the same number.)
     */
    @Volatile var version = 0
        private set

    /** 101_100_000 Hz, program 1 -> "101.1-HD2"; AM (9c) 1_260_000 Hz -> "1260k-HD1". */
    fun key(freqHz: Int, program: Int): String {
        val freq = if (freqHz >= 30_000_000)
            FmBand.mhzText(freqHz)                          // FM: MHz (9i: "87.75" too)
        else
            "${freqHz / 1000}k"                               // AM: kHz
        return "$freq-HD${program + 1}"
    }

    private fun file(context: Context, freqHz: Int, program: Int): File =
        File(File(context.filesDir, "logos"), key(freqHz, program) + ".img")

    /** The saved logo, or null if we've never received one for this frequency + program. */
    fun load(context: Context, freqHz: Int, program: Int): ByteArray? {
        val f = file(context, freqHz, program)
        return try {
            if (f.isFile) f.readBytes() else null
        } catch (e: Exception) {
            null
        }
    }

    /** Saves a logo - but only if it's new or different (saves flash-memory writes). */
    fun save(context: Context, freqHz: Int, program: Int, bytes: ByteArray) {
        if (bytes.isEmpty()) return
        val f = file(context, freqHz, program)
        try {
            if (f.isFile && f.length() == bytes.size.toLong() && f.readBytes().contentEquals(bytes))
                return                                        // same as before
            f.parentFile?.mkdirs()
            f.writeBytes(bytes)
            version++
            Log.i(TAG, "Saved logo ${f.name} (${bytes.size} bytes)")
        } catch (e: Exception) {
            Log.e(TAG, "Could not save logo ${f.name}", e)
        }
    }

    // ---- 13a step 2: Settings > Storage > Clear saved data ----

    private fun files(context: Context): List<File> =
        File(context.filesDir, "logos").listFiles()?.filter { it.isFile } ?: emptyList()

    /** How many logos are saved. */
    fun count(context: Context): Int = files(context).size

    /** How much room they take, in bytes. */
    fun bytes(context: Context): Long = files(context).sumOf { it.length() }

    /**
     * Deletes every saved logo and returns how many there were. The tiles and the
     * notification notice through [version]. (A station you're listening to sends
     * its logos again, so those come back by themselves - fresh ones.)
     */
    fun clear(context: Context): Int {
        var deleted = 0
        for (f in files(context)) {
            try {
                if (f.delete()) deleted++
            } catch (e: Exception) {
                Log.e(TAG, "Could not delete logo ${f.name}", e)
            }
        }
        version++
        Log.i(TAG, "Cleared $deleted saved logos")
        return deleted
    }
}
