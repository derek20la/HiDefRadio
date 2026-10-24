package io.github.derek20la.hidefradio


/**
 * Milestone 9e step 1: one preset = a frequency + a program (e.g. 101.1 HD2).
 *
 * Besides the frequency and program we remember what the station called
 * itself when you saved it, so the tile has a name even when the radio is off:
 *   station = the call sign, e.g. "KRTH"
 *   label   = the program's name at the time, e.g. "Channel Q" or "Top 40"
 *   name    = YOUR name for the preset ("" = none; set with long-press > Rename)
 *   analog  = M12 step 2: saved from an analog-only FM station (no HD - the name came
 *             from RDS); the tile then says "99.5 FM" instead of "99.5 HD1". Cleared
 *             as soon as HD shows up on that frequency (Presets.refresh).
 *             Build 19: also an analog-only AM station ("1260 AM"; AM sends no RDS, so it
 *             has no name until you give it one - long-press > Rename).
 *
 * This file is plain Kotlin (no Android), so it can be tested on a computer.
 * Presets.kt does the saving/loading on the phone.
 */
data class Preset(
    val freqHz: Int,
    val program: Int,            // 0 = HD1, 1 = HD2 ...
    val station: String = "",
    val label: String = "",
    val name: String = "",
    val analog: Boolean = false,     // M12 step 2
) {
    /** "HD1", "HD2", ... */
    val hdName: String
        get() = "HD${program + 1}"

    /** "101.1 HD2" (FM, in MHz), "1260 HD1" (AM, kHz), or "99.5 FM" / "1260 AM" (M12, build 19: no HD). */
    val freqText: String
        get() {
            val freq = if (freqHz >= 30_000_000)
                FmBand.mhzText(freqHz)          // 9i: "87.75" when needed
            else
                "${freqHz / 1000}"
            return if (analog) "$freq $band" else "$freq $hdName"
        }

    /** Build 19: "FM", or "AM" for an analog-only AM preset (saved from the analog AM demodulator). */
    private val band: String
        get() = if (freqHz >= 30_000_000) "FM" else "AM"

    /** M12 step 2: the big letters on a tile without a logo: the call sign, else "HD2" / "FM". */
    val initials: String
        get() = station.ifEmpty { if (analog) band else hdName }

    /**
     * The name on the tile:
     *   1. your own name, if you gave it one;
     *   2. for HD2 and up: the program's name ("Channel Q"), because the call
     *      sign is the same for every program of a station;
     *   3. the call sign ("KRTH");
     *   4. the program's name, or "" if we know nothing (saved while off).
     */
    val title: String
        get() = when {
            name.isNotBlank() -> name
            program > 0 && label.isNotBlank() -> label
            station.isNotBlank() -> station
            else -> label
        }

    /** Is this preset for [freqHz] + [program]? */
    fun matches(freqHz: Int, program: Int): Boolean =
        this.freqHz == freqHz && this.program == program

    companion object {
        /**
         * The whole list -> text, to save it. One preset per line, fields
         * separated by TAB (same idea as getProgramsNative):
         *   101100000 <TAB> 1 <TAB> KRTH <TAB> Channel Q <TAB> (your name) [<TAB> A]
         * M12 step 2: a 6th field "A" = an analog-only station (older lists have 5 fields).
         */
        fun encodeList(list: List<Preset>): String =
            list.joinToString("\n") { p ->
                listOf(p.freqHz.toString(), p.program.toString(),
                       clean(p.station), clean(p.label), clean(p.name)).joinToString("\t") +
                    (if (p.analog) "\tA" else "")
            }

        /** Text -> the list again. Broken lines are skipped. */
        fun decodeList(text: String): List<Preset> =
            text.lines()
                .filter { it.isNotBlank() }
                .mapNotNull { line ->
                    val f = line.split('\t')
                    val freq = f[0].toIntOrNull() ?: return@mapNotNull null
                    val program = f.getOrNull(1)?.toIntOrNull() ?: return@mapNotNull null
                    Preset(freq, program,
                           station = f.getOrElse(2) { "" },
                           label = f.getOrElse(3) { "" },
                           name = f.getOrElse(4) { "" },
                           analog = f.getOrNull(5) == "A")
                }

        /**
         * 13a: the list with the preset at [from] taken out and put back in at [to]
         * (drag & drop of a tile). The others close the gap / make room, keeping their
         * order. Wrong positions -> the list as it was.
         */
        fun move(list: List<Preset>, from: Int, to: Int): List<Preset> {
            if (from !in list.indices || to !in list.indices || from == to) return list
            val result = list.toMutableList()
            result.add(to, result.removeAt(from))
            return result
        }

        /**
         * Build 22: the favorites as a FILE (Settings -> Export favorites), to keep, to move to a
         * new phone, or to move between the Play install and the GitHub install (they can't share
         * data: two signing keys). The same lines encodeList() saves, after two header lines:
         *   # HiDef Radio favorites 1
         *   # 12 favorites, exported 2026-10-24 by 1.1-beta1 (build 22). Logos are not included ...
         *   101100000 <TAB> 0 <TAB> KRTH <TAB> Adult Hits <TAB> <TAB>
         *   ...
         * "1" is the format's version (a later app can tell old files from new ones). Lines that
         * start with "#" are comments. Plain text on purpose: it opens in any text editor, and
         * people can share a list of their city's stations.
         */
        const val FILE_HEADER = "# HiDef Radio favorites 1"

        fun exportText(list: List<Preset>, date: String, version: String): String =
            FILE_HEADER + "\n" +
                "# ${list.size} favorites, exported $date by $version. Logos are not included - " +
                "each one comes back the next time that station's HD is received.\n" +
                encodeList(list) + "\n"

        /**
         * Build 22: a file's text -> its favorites, or null when it isn't a favorites file at all
         * (no header and no line that reads as a favorite). Comment lines and broken lines are
         * skipped; a file listing the same station + program twice keeps the first one.
         */
        fun importText(text: String): List<Preset>? {
            val found = decodeList(text.lines().filterNot { it.trimStart().startsWith("#") }.joinToString("\n"))
                .filter { it.freqHz > 0 && it.program in 0..7 }
            val unique = found.filterIndexed { i, p -> found.indexOfFirst { it.matches(p.freqHz, p.program) } == i }
            return if (unique.isEmpty() && !text.trimStart().startsWith(FILE_HEADER)) null else unique
        }

        /**
         * Build 22: "Add the new ones" - your list as it is (order, names, everything), then the
         * file's favorites that you don't have yet (same frequency + program = you have it), in
         * the file's order.
         */
        fun merge(current: List<Preset>, incoming: List<Preset>): List<Preset> =
            current + incoming.filter { p -> current.none { it.matches(p.freqHz, p.program) } }

        // Tabs and new-lines would break the format -> replace them with spaces.
        private fun clean(s: String): String =
            s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim()
    }
}
