package io.github.derek20la.hidefradio

import android.content.Context

/**
 * Milestone 9e step 1: your list of presets, saved in app storage
 * (SharedPreferences "presets", as the text Preset.encodeList() makes).
 * The order of the list = the order of the tiles on screen (13a: change it by
 * holding a tile and dragging it).
 *
 * Every function reads the list, changes it and saves it again - the list is
 * tiny, so that's quick and keeps things simple.
 */
object Presets {
    private const val PREFS = "presets"
    private const val KEY_LIST = "list"

    fun load(context: Context): List<Preset> =
        Preset.decodeList(prefs(context).getString(KEY_LIST, "") ?: "")

    private fun store(context: Context, list: List<Preset>) {
        prefs(context).edit().putString(KEY_LIST, Preset.encodeList(list)).apply()
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Where [freqHz] + [program] is in the list, or -1 if it isn't a preset. */
    fun indexOf(context: Context, freqHz: Int, program: Int): Int =
        load(context).indexOfFirst { it.matches(freqHz, program) }

    /** Adds a preset at [index] (default: at the end). Replaces an old one for the same station + program. */
    fun add(context: Context, preset: Preset, index: Int = -1) {
        val list = load(context).filterNot { it.matches(preset.freqHz, preset.program) }.toMutableList()
        if (index in 0..list.size) list.add(index, preset) else list.add(preset)
        store(context, list)
    }

    fun remove(context: Context, index: Int) {
        val list = load(context).toMutableList()
        if (index !in list.indices) return
        list.removeAt(index)
        store(context, list)
    }

    /** Your name for a preset ("" = back to the automatic name). */
    fun rename(context: Context, index: Int, name: String) {
        val list = load(context).toMutableList()
        if (index !in list.indices) return
        list[index] = list[index].copy(name = name.trim())
        store(context, list)
    }

    /** 13a: moves the preset at [from] to position [to] (a tile was dragged there). */
    fun moveTo(context: Context, from: Int, to: Int) {
        val list = load(context)
        val moved = Preset.move(list, from, to)
        if (moved != list) store(context, moved)
    }

    /**
     * While we're tuned to a preset, keep its call sign and program name up
     * to date. (9e step 2: step 1 only filled in EMPTY names, so KYSR HD2
     * saved early stayed "Top 40" - the program type that arrives first -
     * instead of "TikTok Radio", the station's name that arrives later.)
     *   [label]  = the program's name right now
     *   [sure]   = true if [label] is a real name (the station's SIG name or
     *              yours), false if it's just the program type ("Top 40").
     *              A type only fills an empty label; it never replaces a name.
     * Your own preset name (Rename) is never touched.
     */
    fun refresh(context: Context, freqHz: Int, program: Int,
                station: String, label: String, sure: Boolean) {
        val list = load(context).toMutableList()
        val i = list.indexOfFirst { it.matches(freqHz, program) }
        if (i < 0) return
        val p = list[i]
        val newLabel = when {
            label.isBlank() -> p.label
            sure -> label
            else -> p.label.ifBlank { label }
        }
        // M12 step 2: HD info for this frequency -> it isn't an analog-only station (any more).
        val newP = p.copy(station = station.ifBlank { p.station }, label = newLabel, analog = false)
        if (newP != p) {
            list[i] = newP
            store(context, list)
        }
    }

    /**
     * M12 step 2: an analog-only preset that was saved before RDS had named the station
     * (or while it only had a scrolling PS) gets the name once RDS gives one.
     */
    fun refreshAnalog(context: Context, freqHz: Int, name: String) {
        if (name.isBlank()) return
        val list = load(context).toMutableList()
        val i = list.indexOfFirst { it.analog && it.matches(freqHz, 0) }
        if (i < 0 || list[i].station.isNotBlank()) return
        list[i] = list[i].copy(station = name)
        store(context, list)
    }
}
