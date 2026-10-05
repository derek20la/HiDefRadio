package io.github.derek20la.hidefradio

/**
 * Milestone 9b: one audio program of the station we're tuned to (HD1 ... HD8).
 *
 * The native engine reports the programs on the air as text
 * (RadioEngine.getProgramsNative(): one line per program, fields separated by
 * TAB characters). [parseList] turns that text into a list of these objects,
 * so the rest of the app never has to deal with tabs.
 *
 * A `data class` is a class that just holds values (Kotlin writes equals(),
 * toString() etc. for us).
 */
data class Program(
    val number: Int,      // 0 = HD1, 1 = HD2 ... 7 = HD8
    val type: String,     // program type from the audio, e.g. "Rock", "None", or ""
    val sigName: String,  // the station's name for it, e.g. "TikTok Radio", "MPS", "HD-2"
    val artist: String,   // now playing (may be empty)
    val title: String,
) {
    /** "HD1", "HD2", ... */
    val hdName: String
        get() = "HD${number + 1}"

    /**
     * A short name for the button, or "" if there's nothing useful:
     *  1. the station's own name for the program ("ALT 98.7", "TikTok Radio"),
     *     unless it's a generic one like "MPS", "SPS1" or "HD-2" (KLOS, KKGO);
     *  2. otherwise the program type ("Rock"), unless it's "None".
     * (Names you choose yourself are kept in ProgramNames and win over both.)
     */
    val name: String
        get() = nameFor("")

    /**
     * 9d step 4: the same as [name], but also skips a SIG name that is just the
     * station's call sign (KRTH calls its HD1 "KRTH" - "HD1 · KRTH" under the
     * big "KRTH" tells you nothing, "HD1 · Adult Hits" does).
     * [station] = the station name, e.g. "KRTH" ("" = not known yet).
     */
    fun nameFor(station: String): String {
        val sig = sigName.trim()
        return when {
            sig.isNotEmpty() && !GENERIC_NAME.matches(sig) && !sameStation(sig, station) -> sig
            type.isNotBlank() && type != "None" && type != "Unknown" -> type
            else -> ""
        }
    }

    /** "Artist - Title", or null if the station hasn't sent a song yet. */
    val nowPlaying: String?
        get() = when {
            artist.isBlank() && title.isBlank() -> null
            artist.isBlank() -> title
            title.isBlank() -> artist
            else -> "$artist - $title"
        }

    companion object {
        // Names that don't tell you anything: MPS, SPS1..SPS7, HD1, HD-1, HD 1.
        // (IGNORE_CASE: "mps" and "Hd-2" count too.)
        private val GENERIC_NAME = Regex("(MPS|SPS\\d|HD[- ]?\\d)", RegexOption.IGNORE_CASE)

        /** "KRTH" = "KRTH", "krth-fm", "KRTH FM" (ignoring case and an "FM" ending). */
        private fun sameStation(name: String, station: String): Boolean {
            fun callSign(s: String) = s.trim().uppercase().removeSuffix("FM").trimEnd('-', ' ')
            return station.isNotBlank() && callSign(name) == callSign(station)
        }

        /** Turns RadioEngine.getProgramsNative()'s text into a list of programs. */
        fun parseList(text: String): List<Program> =
            text.lines()
                .filter { it.isNotBlank() }
                .mapNotNull { line ->
                    val f = line.split('\t')
                    // Skip a line that doesn't start with a number (shouldn't happen).
                    val number = f[0].toIntOrNull() ?: return@mapNotNull null
                    Program(
                        number = number,
                        type = f.getOrElse(1) { "" },
                        sigName = f.getOrElse(2) { "" },
                        artist = f.getOrElse(3) { "" },
                        title = f.getOrElse(4) { "" },
                    )
                }
    }
}
