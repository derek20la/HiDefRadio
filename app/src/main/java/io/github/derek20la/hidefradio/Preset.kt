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

    /** "101.1 HD2" (FM, in MHz), "1260 HD1" (AM, kHz), or "99.5 FM" (M12: no HD). */
    val freqText: String
        get() {
            val freq = if (freqHz >= 30_000_000)
                FmBand.mhzText(freqHz)          // 9i: "87.75" when needed
            else
                "${freqHz / 1000}"
            return if (analog) "$freq FM" else "$freq $hdName"
        }

    /** M12 step 2: the big letters on a tile without a logo: the call sign, else "HD2" / "FM". */
    val initials: String
        get() = station.ifEmpty { if (analog) "FM" else hdName }

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

        // Tabs and new-lines would break the format -> replace them with spaces.
        private fun clean(s: String): String =
            s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim()
    }
}
