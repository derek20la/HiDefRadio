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
 *
 * Build 39: on FM the remembered gain is no longer taken on its peak alone.
 * It is where the quality sweep STARTS (doAutoGainFm): it has to show HD
 * and is compared with its neighbours. And only a tested gain gets in here
 * (GainKeeper.gainTrusted). AM is as it was.
 *
 * Build 39a: for such a tested FM gain a second note is kept (SharedPreferences
 * "gain_quality"): how far the HD sideband stood out of the noise when the gain
 * was chosen. With it the next tune needs ONE look to see that the gain is still
 * good - no search. A gain remembered by build 38 or earlier has no such note
 * and is searched from once.
 */
object GainMemory {
    private const val PREFS = "gain_memory"
    private const val PREFS_QUALITY = "gain_quality"    // build 39a: key = frequency, value = gain * 1000 + quality (both tenths of a dB)

    /** The remembered gain for [freqHz] in tenths of a dB, or -1 = none. */
    fun get(context: Context, freqHz: Int): Int =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(freqHz.toString(), -1)

    /**
     * Build 39a: what is known about the remembered [gainTenths] of [freqHz]: -1 = nothing (AM, or
     * remembered by an older build), 0 = it was tested, more = tested, and its HD sideband stood
     * this many tenths of a dB over the noise floor then.
     */
    fun quality(context: Context, freqHz: Int, gainTenths: Int): Int {
        val packed = context.getSharedPreferences(PREFS_QUALITY, Context.MODE_PRIVATE).getInt(freqHz.toString(), -1)
        return if (packed >= 0 && packed / 1000 == gainTenths) packed % 1000 else -1   // a note for another gain doesn't count
    }

    /**
     * Remembers [gainTenths] for [freqHz] (only writes if it's different).
     * Build 39a: [qualityTenths] = see [quality]; -1 = nothing to note (AM).
     */
    fun set(context: Context, freqHz: Int, gainTenths: Int, qualityTenths: Int = -1) {
        if (qualityTenths >= 0 && quality(context, freqHz, gainTenths) != qualityTenths) {
            context.getSharedPreferences(PREFS_QUALITY, Context.MODE_PRIVATE).edit()
                .putInt(freqHz.toString(), gainTenths * 1000 + qualityTenths.coerceAtMost(999))
                .apply()
        }
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
        context.getSharedPreferences(PREFS_QUALITY, Context.MODE_PRIVATE).edit().clear().apply()   // build 39a
    }
}
