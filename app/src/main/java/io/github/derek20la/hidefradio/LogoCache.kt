package io.github.derek20la.hidefradio

import android.content.Context
import android.util.Log
import java.io.File
import java.util.Locale

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
 *
 * Build 10: a logo knows its OWNER. A frequency is not a station: on 91.1 west of
 * Los Angeles KDSC's HD logo was saved one evening, and the next morning 91X from
 * Tijuana played on the same frequency under "Classical California". So the file
 * name now carries who the picture belongs to - the HD station's name as it sent it
 * (letters and digits, upper case) and, once the app has worked it out, that
 * station's own RDS code:
 *     91.1-HD1.img                  as saved by builds 1-9 (owner not known)
 *     91.1-HD1-KDSC.img             the HD named itself KDSC
 *     91.1-HD1-KDSC-19C2.img        ... and its analog sends the RDS code 19C2
 * A saved logo is shown unless something says it is another station's: the HD on the
 * air names itself differently, or the analog you hear sends another RDS code than
 * the owner's ([fits]). No RDS, or an owner without a code = no contradiction = shown,
 * as before. The same owner is handed to the native code before a tune
 * (RadioEngine.setHdOwnerNative), which is how it knows a foreign code at once.
 * Files without an owner are shown as before and get theirs the next time that
 * station's HD is received.
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

    // ---- build 10: the owner in the file name ----

    /** Whose logo a file is: the HD station's name in file-name form ("" = not known) and its own RDS code ("" = not known). */
    data class Owner(val name: String, val pi: String)

    /**
     * A station name in file-name form: upper case, letters and digits only, at most 12
     * ("KDSC-FM" -> "KDSCFM", "Go Country" -> "GOCOUNTRY"). The native code compares names
     * the same way (ident::ownerKey in stationid.hpp) - keep the two alike.
     */
    fun ownerKey(name: String): String =
        name.uppercase(Locale.US).filter { it in 'A'..'Z' || it in '0'..'9' }.take(12)

    /** "19c2" -> "19C2"; anything that is not four hex digits -> "". */
    private fun piKey(pi: String): String {
        val p = pi.trim().uppercase(Locale.US)
        return if (p.length == 4 && p.all { it in '0'..'9' || it in 'A'..'F' }) p else ""
    }

    /** "91.1-HD1", "KDSC", "19C2" -> "91.1-HD1-KDSC-19C2.img" (no code without a name). */
    fun fileName(key: String, owner: String, pi: String): String = when {
        owner.isEmpty() -> "$key.img"
        pi.isEmpty() -> "$key-$owner.img"
        else -> "$key-$owner-$pi.img"
    }

    /** The owner written in a file name ("91.1-HD1-KDSC-19C2.img" -> KDSC, 19C2). */
    fun ownerOf(fileName: String): Owner {
        // freq - HDn - owner - code: neither the frequency ("91.1", "1260k") nor an owner has a "-" in it
        val parts = fileName.removeSuffix(".img").split('-')
        return Owner(parts.getOrNull(2).orEmpty(), piKey(parts.getOrNull(3).orEmpty()))
    }

    private fun dir(context: Context) = File(context.filesDir, "logos")

    /** Is this file the logo of `key` ("91.1-HD1"), whoever owns it? */
    private fun isFor(name: String, key: String): Boolean =
        name.endsWith(".img") && (name == "$key.img" || name.startsWith("$key-"))

    /** The file holding the logo of this frequency + program (one at most), or null. */
    private fun find(context: Context, freqHz: Int, program: Int): File? {
        val key = key(freqHz, program)
        return dir(context).listFiles()?.firstOrNull { it.isFile && isFor(it.name, key) }
    }

    /** All the saved logos of one frequency (HD1, HD2 ...). */
    private fun filesOf(context: Context, freqHz: Int): List<File> {
        val prefix = key(freqHz, 0).substringBeforeLast("HD")           // "91.1-"
        return dir(context).listFiles()?.filter { it.isFile && it.name.endsWith(".img") && it.name.startsWith(prefix + "HD") }
            ?: emptyList()
    }

    // owner() is asked every second by the screen - remember the answer until a file changes.
    private var ownerKnownFor = ""
    private var ownerKnown: Owner? = null

    /**
     * Whose HD the saved logos of this frequency belong to, read from the file names
     * (the HD1 file first). null = no saved logo there, or none that knows its owner.
     */
    @Synchronized
    fun owner(context: Context, freqHz: Int): Owner? {
        val asked = "$freqHz/$version"
        if (asked == ownerKnownFor) return ownerKnown
        val owners = filesOf(context, freqHz).sortedBy { it.name }.map { ownerOf(it.name) }.filter { it.name.isNotEmpty() }
        val found = owners.firstOrNull { it.pi.isNotEmpty() } ?: owners.firstOrNull()
        ownerKnownFor = asked
        ownerKnown = found
        return found
    }

    /**
     * May the logo saved for this frequency be shown right now? Not when something says it
     * is another station's:
     *  - the HD on the air names itself differently than the logo's owner, or
     *  - you HEAR the analog and it sends another RDS code than the owner's.
     * Everything else - no owner in the file name, no code, no RDS - is no contradiction.
     * [station] = the HD's name ("" = none received), [rdsPi] = the analog's confirmed code.
     */
    fun fits(context: Context, freqHz: Int, station: String, analogHeard: Boolean, rdsPi: String): Boolean {
        val o = owner(context, freqHz) ?: return true
        if (station.isNotEmpty() && ownerKey(station).isNotEmpty() && ownerKey(station) != o.name) return false
        if (analogHeard && o.pi.isNotEmpty() && piKey(rdsPi).isNotEmpty() && piKey(rdsPi) != o.pi) return false
        return true
    }

    /** The saved logo, or null if we've never received one for this frequency + program. */
    fun load(context: Context, freqHz: Int, program: Int): ByteArray? {
        return try {
            find(context, freqHz, program)?.readBytes()
        } catch (e: Exception) {
            null
        }
    }

    /**
     * Saves a logo - but only if it's new or different (saves flash-memory writes).
     * Build 10: [owner] = the HD station's name as it sent it, [ownerPi] = that station's own
     * RDS code in hex (either may be "" when not known yet - [setOwner] adds them later).
     * A logo saved under a new owner removes the frequency's logos of any OTHER owner:
     * they are another station's pictures.
     */
    @Synchronized
    fun save(context: Context, freqHz: Int, program: Int, bytes: ByteArray, owner: String = "", ownerPi: String = "") {
        if (bytes.isEmpty()) return
        val key = key(freqHz, program)
        val old = find(context, freqHz, program)
        // No name known at this moment: keep the owner the file already has.
        val had = old?.let { ownerOf(it.name) } ?: Owner("", "")
        val name = ownerKey(owner).ifEmpty { had.name }
        val pi = if (name == had.name) piKey(ownerPi).ifEmpty { had.pi } else piKey(ownerPi)
        val target = File(dir(context), fileName(key, name, pi))
        try {
            if (old != null && old.length() == bytes.size.toLong() && old.readBytes().contentEquals(bytes)) {
                if (old.name != target.name && old.renameTo(target)) {     // the same picture - now with its owner
                    version++
                    Log.i(TAG, "Logo ${old.name} is now ${target.name}")
                }
                return
            }
            target.parentFile?.mkdirs()
            target.writeBytes(bytes)
            if (old != null && old.name != target.name) old.delete()
            if (name.isNotEmpty())
                for (f in filesOf(context, freqHz)) {
                    val o = ownerOf(f.name).name
                    if (o.isNotEmpty() && o != name && f.delete()) Log.i(TAG, "Removed ${f.name}: this frequency's HD is $name now")
                }
            version++
            Log.i(TAG, "Saved logo ${target.name} (${bytes.size} bytes)")
        } catch (e: Exception) {
            Log.e(TAG, "Could not save logo ${target.name}", e)
        }
    }

    /**
     * Build 10: writes the owner into the names of this frequency's saved logos - for logos
     * saved before the name or the code was known (the code takes a few seconds to work
     * out; builds 1-9 saved no owner at all), and when the station's code turns out to be
     * another one than remembered. Logos of a DIFFERENT owner are left alone (they go when
     * that frequency's new station sends its own). [pi] = "" keeps the code a file has.
     */
    @Synchronized
    fun setOwner(context: Context, freqHz: Int, owner: String, pi: String) {
        val name = ownerKey(owner)
        if (name.isEmpty()) return
        var renamed = 0
        for (f in filesOf(context, freqHz)) {
            val o = ownerOf(f.name)
            if (o.name.isNotEmpty() && o.name != name) continue
            val key = f.name.removeSuffix(".img").split('-').take(2).joinToString("-")      // "91.1-HD1"
            val target = File(f.parentFile, fileName(key, name, piKey(pi).ifEmpty { o.pi }))
            if (target.name == f.name) continue
            try {
                if (f.renameTo(target)) { renamed++; Log.i(TAG, "Logo ${f.name} is now ${target.name}") }
            } catch (e: Exception) {
                Log.e(TAG, "Could not rename logo ${f.name}", e)
            }
        }
        if (renamed > 0) version++
    }

    // ---- 13a step 2: Settings > Storage > Clear saved data ----

    private fun files(context: Context): List<File> =
        dir(context).listFiles()?.filter { it.isFile } ?: emptyList()

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
