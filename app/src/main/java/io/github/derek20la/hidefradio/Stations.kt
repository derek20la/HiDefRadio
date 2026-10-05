package io.github.derek20la.hidefradio

import android.content.Context
import java.io.BufferedReader
import java.util.Locale

/**
 * 13b: one station from the built-in FCC list (see [Stations]).
 *
 * @param freqHz    its frequency (101_100_000 or 1_260_000)
 * @param call      call letters as the FCC writes them: "KRTH", "KGB-FM", "KFUG-LP", "K266CG", "XHITZ-FM"
 * @param service   FM = full power, FL = low power (LPFM), FX = translator, FB = booster, AM
 * @param fccClass  the station class ("B", "C1", "A", "LP1", "D"...; "" if none)
 * @param hd        the FCC has an HD Radio (hybrid digital) notification on file for it (FM only)
 * @param country   "US", "CA" (Canada), "MX" (Mexico)...
 * @param kw        power in kW (FM: effective radiated power; AM: by day)
 * @param nightKw   AM only: power at night (-1 = none / unknown)
 * @param haatM     FM only: antenna height above average terrain, metres
 * @param directional AM only: "d" / "n" / "dn" = directional antenna by day / at night
 */
data class Station(
    val freqHz: Int,
    val call: String,
    val service: String,
    val fccClass: String,
    val hd: Boolean,
    val city: String,
    val state: String,
    val country: String,
    val lat: Double,
    val lon: Double,
    val kw: Double,
    val nightKw: Double,
    val haatM: Int,
    val directional: String,
) {
    /** The letters alone: "KGB-FM" -> "KGB", "KFUG-LP" -> "KFUG". */
    val baseCall: String get() = Stations.baseCall(call)

    /** "Los Angeles, CA" - other countries get their name: "Tijuana, BN, Mexico". */
    val place: String
        get() = listOf(city, state, Stations.countryName(country)).filter { it.isNotBlank() }.joinToString(", ")

    /** "51 kW" / "250 W" - AM: "20 kW day, 7.5 kW night". */
    val power: String
        get() = when {
            kw < 0 && nightKw < 0 -> ""
            service != "AM" -> Stations.powerText(kw)
            nightKw < 0 -> Stations.powerText(kw) + " day only"
            kw < 0 -> Stations.powerText(nightKw) + " night only"
            kw == nightKw -> Stations.powerText(kw)
            else -> Stations.powerText(kw) + " day, " + Stations.powerText(nightKw) + " night"
        }

    /** What kind of station: "class B", "translator", "low power (LPFM)", "booster". */
    val kind: String
        get() = when (service) {
            "FX" -> "translator"
            "FL" -> "low power (LPFM)"
            "FB" -> "booster"
            else -> if (fccClass.isEmpty()) "" else "class $fccClass"
        }
}

/**
 * 13b: the built-in station list - who is licensed on which frequency.
 *
 * The data comes from the FCC (public domain; tools/fcc/make_stations.py turns the
 * FCC's FM and AM Query downloads into the two small text files in assets/stations/).
 * It covers the US, plus the Canadian and Mexican stations the FCC keeps on file.
 * Nothing is looked up on the internet - the list is inside the app and gets a
 * refresh with app updates.
 *
 * What the app does with it:
 *  - Call letters for stations whose RDS code (PI) can't be turned back into letters.
 *    iHeart stations send the PI with its first digit replaced by 1, which leaves nine
 *    possible calls - but normally only ONE of the nine is licensed on the frequency
 *    you are tuned to. [callsOn] hands the native code that frequency's calls.
 *  - Telling a call sign from a name that only looks like one: KTWV's HD Radio name
 *    is "WAVE"; nobody called WAVE is licensed on 94.7, so it is just a name.
 *  - The "Station" row in the signal details: city of license, power, class.
 *  - Later: distances for a DX log (every station has its coordinates).
 *
 * The files are grouped by frequency ("@10110" = 101.1 MHz, "@1260" = 1260 kHz) and
 * sorted, so finding one frequency means reading the file only up to that group
 * (a few milliseconds). Nothing is kept in memory but the last answers.
 *
 * The parsing ([parse], [search]) is plain Kotlin, so it can be tested on a PC.
 */
object Stations {
    private const val ASSET_FM = "stations/fm.txt"
    private const val ASSET_AM = "stations/am.txt"

    // The last answers (the details row asks again every second). Under the lock.
    private val lock = Any()
    private var cachedHz = -1
    private var cached: List<Station> = emptyList()
    private val elsewhereCache = HashMap<String, Station?>()
    private var infoLine: String? = null

    /**
     * The stations licensed on [hz], or null if the list can't be read
     * (which shouldn't happen - it is part of the app).
     */
    fun on(context: Context, hz: Int): List<Station>? = synchronized(lock) {
        if (hz == cachedHz) return cached
        val am = FmBand.isAm(hz)
        val found = try {
            context.assets.open(if (am) ASSET_AM else ASSET_FM).bufferedReader().use { parse(it, am, key(hz)) }
        } catch (e: Exception) {
            return null
        }
        cachedHz = hz
        cached = found
        found
    }

    /**
     * The station on [hz] that [name] talks about, or null. [name] is whatever the
     * station calls itself: "KRTH", "KGB-FM", "101.5 KGB" - any word of it that is
     * a call licensed on this frequency counts ("WAVE" on 94.7: none).
     */
    fun find(context: Context, hz: Int, name: String): Station? {
        val list = on(context, hz) ?: return null
        for (word in words(name)) list.firstOrNull { it.baseCall == word }?.let { return it }
        return null
    }

    /**
     * [name]'s station wherever it is licensed (same band as [hz]) - for a call that
     * is NOT licensed on the frequency it was heard on: a translator or booster
     * carrying it. Reads the whole list (~20 ms), so each answer is remembered.
     */
    fun elsewhere(context: Context, hz: Int, name: String): Station? = synchronized(lock) {
        val am = FmBand.isAm(hz)
        for (word in words(name)) {
            if (word.length < 3) continue
            val cacheKey = (if (am) "AM " else "FM ") + word
            if (!elsewhereCache.containsKey(cacheKey)) {
                if (elsewhereCache.size > 50) elsewhereCache.clear()
                elsewhereCache[cacheKey] = try {
                    context.assets.open(if (am) ASSET_AM else ASSET_FM).bufferedReader().use { search(it, am, word) }
                } catch (e: Exception) {
                    null
                }
            }
            val found = elsewhereCache[cacheKey]
            if (found != null) return found
        }
        null
    }

    /**
     * For the native code: the US call letters licensed on [hz], letters only,
     * separated by spaces ("KHYL KWYE KRTH ..."). Translators' own calls (K266CG)
     * are left out - no station identifies with those over RDS or HD Radio.
     * "" = nobody; null = the list can't be read (the native code then works without it).
     */
    fun callsOn(context: Context, hz: Int): String? {
        val list = on(context, hz) ?: return null
        return list.map { it.baseCall }.filter { isUsCall(it) }.distinct().joinToString(" ")
    }

    /** "FCC data of 2026-10-01" for the About section ("" if the list can't be read). */
    fun info(context: Context): String = synchronized(lock) {
        infoLine?.let { return it }
        val text = try {
            headerInfo(context.assets.open(ASSET_FM).bufferedReader().use { it.readLine() } ?: "")
        } catch (e: Exception) {
            ""
        }
        infoLine = text
        text
    }

    // ------------------------------------------------------------ plain Kotlin (testable)

    /** The file's group number for a frequency: FM in 10 kHz units (10110), AM in kHz (1260). */
    fun key(hz: Int): Int = if (FmBand.isAm(hz)) hz / 1000 else (hz + 5000) / 10_000

    /**
     * Reads the group "@[wantedKey]" out of a station file. The groups are sorted,
     * so reading stops at the first group past the wanted one.
     */
    fun parse(reader: BufferedReader, am: Boolean, wantedKey: Int): List<Station> {
        val out = ArrayList<Station>()
        var inGroup = false
        while (true) {
            val line = reader.readLine() ?: break
            if (line.isEmpty() || line[0] == '#') continue
            if (line[0] == '@') {
                val k = line.substring(1).trim().toIntOrNull() ?: continue
                if (k > wantedKey) break
                inGroup = k == wantedKey
            } else if (inGroup) {
                station(line, am, wantedKey)?.let { out.add(it) }
            }
        }
        return out
    }

    /** The first station in the whole file whose letters are [base] ("KGB"), or null. */
    fun search(reader: BufferedReader, am: Boolean, base: String): Station? {
        var k = 0
        while (true) {
            val line = reader.readLine() ?: return null
            if (line.isEmpty() || line[0] == '#') continue
            if (line[0] == '@') { k = line.substring(1).trim().toIntOrNull() ?: 0; continue }
            // cheap test first: the line starts with the letters, then "|" or "-"
            if (!line.startsWith(base) || line.length <= base.length) continue
            val next = line[base.length]
            if (next != '|' && next != '-') continue
            val s = station(line, am, k) ?: continue
            // a station's own licence, not its booster ("KBIG-FM1") or a translator
            if (s.service == "FB" || s.service == "FX") continue
            return s
        }
    }

    /** One line of a station file -> a [Station] (null if it is damaged). */
    fun station(line: String, am: Boolean, key: Int): Station? {
        val p = line.split('|')
        return try {
            if (am) {
                // call|AM|class|city|state|country|lat|lon|day kW|night kW|d/n
                if (p.size < 11) return null
                Station(key * 1000, p[0], p[1], p[2], false, p[3], p[4], p[5],
                        p[6].toDouble(), p[7].toDouble(),
                        p[8].toDoubleOrNull() ?: -1.0, p[9].toDoubleOrNull() ?: -1.0, 0, p[10])
            } else {
                // call|service|class|H|city|state|country|lat|lon|ERP kW|HAAT m
                if (p.size < 11) return null
                Station(key * 10_000, p[0], p[1], p[2], p[3] == "H", p[4], p[5], p[6],
                        p[7].toDouble(), p[8].toDouble(),
                        p[9].toDoubleOrNull() ?: -1.0, -1.0, p[10].toIntOrNull() ?: 0, "")
            }
        } catch (e: NumberFormatException) {
            null
        }
    }

    /** "KGB-FM" -> "KGB", "kfug-lp" -> "KFUG". */
    fun baseCall(call: String): String = call.substringBefore('-').trim().uppercase(Locale.US)

    /** The words of a station name as possible calls: "101.5 KGB-FM" -> ["101.5", "KGB"]. */
    fun words(name: String): List<String> =
        name.split(' ', '/', ',').map { baseCall(it) }.filter { it.isNotEmpty() }

    /** K or W + 2 or 3 more letters - what a US station's own call looks like. */
    fun isUsCall(base: String): Boolean =
        base.length in 3..4 && (base[0] == 'K' || base[0] == 'W') && base.all { it in 'A'..'Z' }

    /** 51.0 -> "51 kW", 0.25 -> "250 W", 0.028 -> "28 W". */
    fun powerText(kw: Double): String = when {
        kw < 0 -> ""
        kw >= 1.0 -> trimZeros(String.format(Locale.US, "%.2f", kw)) + " kW"
        else -> trimZeros(String.format(Locale.US, "%.1f", kw * 1000.0)) + " W"
    }

    private fun trimZeros(number: String): String =
        if (number.contains('.')) number.trimEnd('0').trimEnd('.') else number

    /** The FCC's country codes -> names; the US gets none ("Los Angeles, CA" is enough). */
    fun countryName(code: String): String = when (code) {
        "US" -> ""
        "CA" -> "Canada"
        "MX" -> "Mexico"
        "VG" -> "British Virgin Islands"
        else -> code
    }

    /** The first line of fm.txt ("# ... - date 2026-10-01 - stations 22919") -> "FCC data of 2026-10-01". */
    fun headerInfo(firstLine: String): String {
        val date = Regex("date (\\d{4}-\\d{2}-\\d{2})").find(firstLine)?.groupValues?.get(1) ?: return ""
        return "FCC data of $date"
    }
}
