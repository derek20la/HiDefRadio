package io.github.derek20la.hidefradio

import android.content.Context

/**
 * Milestone 9b step 3b: names YOU give to programs, e.g. "Deep Tracks" for
 * KLOS 95.5 HD2 (which the station itself only calls "SPS1").
 *
 * Saved in a small key/value file in app storage (SharedPreferences
 * "program_names"), one entry per frequency + program:
 *   "95500000_1" -> "Deep Tracks"      (95.5 MHz, program 1 = HD2)
 * so they survive restarts. A name you give always wins over the station's.
 */
object ProgramNames {
    private const val PREFS = "program_names"

    private fun key(freqHz: Int, program: Int) = "${freqHz}_$program"

    /** Your name for this program, or null if you haven't named it. */
    fun get(context: Context, freqHz: Int, program: Int): String? =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(key(freqHz, program), null)
            ?.takeIf { it.isNotBlank() }

    /** Saves a name. An empty name (or null) removes it -> back to the station's name. */
    fun set(context: Context, freqHz: Int, program: Int, name: String?) {
        val editor = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
        val clean = name?.trim().orEmpty()
        if (clean.isEmpty()) editor.remove(key(freqHz, program))
        else editor.putString(key(freqHz, program), clean)
        editor.apply()
    }

    /**
     * The name to show: yours if you set one, otherwise the station's.
     * 9d step 4: [station] (the call sign, e.g. "KRTH") lets Program.nameFor()
     * skip a program name that just repeats it.
     */
    fun label(context: Context, freqHz: Int, program: Program, station: String = ""): String =
        get(context, freqHz, program.number) ?: program.nameFor(station)
}
