package io.github.derek20la.hidefradio

import android.content.Context

/**
 * Milestone 10c: the auto gain that worked for each frequency, remembered
 * (SharedPreferences "gain_memory", key = frequency in Hz, value = tenths
 * of a dB). The next time you tune there, auto-gain checks that gain first
 * and skips its search if it still fits (see doAutoGain in native-lib.cpp).
 *
 * RadioService saves it once the station has been synced and the gain
 * has stayed the same for a while (the watchdog is happy with it).
 */
object GainMemory {
    private const val PREFS = "gain_memory"

    /** The remembered gain for [freqHz] in tenths of a dB, or -1 = none. */
    fun get(context: Context, freqHz: Int): Int =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(freqHz.toString(), -1)

    /** Remembers [gainTenths] for [freqHz] (only writes if it's different). */
    fun set(context: Context, freqHz: Int, gainTenths: Int) {
        if (get(context, freqHz) == gainTenths) return
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putInt(freqHz.toString(), gainTenths)
            .apply()
    }

    // ---- 13a step 2: Settings > Storage > Clear saved data ----

    /** For how many frequencies a gain is remembered. */
    fun count(context: Context): Int =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).all.size

    /** Forgets them all: auto-gain searches afresh on every station (and remembers again). */
    fun clear(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().clear().apply()
    }
}
