package io.github.derek20la.hidefradio

import java.util.Locale

/**
 * Build 12: how much current the phone takes from its battery while the radio plays.
 *
 * Why: "is this build easier on the battery?" could only be answered by watching the
 * battery percentage - and a phone counts whole percents, so a 15 minute test reads
 * "3 %" whether the truth is 2.6 or 3.4. The phone also measures the current itself
 * (Android's BatteryManager, no permission needed); averaged over a few minutes that is a
 * far finer measure, and it is there after one minute instead of an hour.
 *
 * RadioService hands in one reading a second (also with the screen off - it holds a wake
 * lock while the radio plays). This class only does the arithmetic, so it has no Android
 * in it and can be tested on a PC.
 *
 * Two figures:
 *  - the average of the last minute, whatever the screen did;
 *  - the average over the LAST TIME THE SCREEN WAS OFF. That is the one to compare builds
 *    or settings with: the screen is the biggest consumer in the phone and would drown
 *    everything else. Tune, switch the screen off, wait a few minutes, switch it on, read.
 *    (The first seconds after the screen goes dark are left out - the phone is still
 *    winding down.)
 *
 * Good to know when reading the figures: the dongle is powered by the phone and takes
 * about 250-270 mA at 5 V all by itself (RTL-SDR Blog V4 datasheet) - most of what the
 * radio costs is the dongle, not the app's arithmetic.
 */
class BatteryMeter {

    /** One reading, already in mA drawn from the battery (see [milliamps]). */
    private class Sample(val atMs: Long, val ma: Float)

    private val recent = ArrayDeque<Sample>()        // the last RECENT_MS
    private var lastMa = -1f
    private var volts = 0f                           // the battery's voltage, for watts
    private var capacityMah = 0f                     // the whole battery, for "% per hour" (0 = unknown)
    private var external = false                     // on a charger / powered hub: the reading means nothing

    // The screen-off period being measured, and the last finished one.
    private var screenWasOn = true
    private var offSinceMs = 0L
    private var offSum = 0.0
    private var offCount = 0
    private var offFirstMs = 0L
    private var offLastMs = 0L
    private var doneMa = -1f                         // the last finished period: its average ...
    private var doneMs = 0L                          // ... and how long it was

    /**
     * One reading. [rawCurrent] = BatteryManager.BATTERY_PROPERTY_CURRENT_NOW as the phone
     * gives it; [millivolts] the battery voltage; [capacityMah] the whole battery (0 =
     * unknown); [onExternalPower] = a charger or powered hub is connected.
     */
    fun add(nowMs: Long, rawCurrent: Int, millivolts: Int, capacityMah: Float, screenOn: Boolean, onExternalPower: Boolean) {
        external = onExternalPower
        if (millivolts > 0) volts = millivolts / 1000f
        if (capacityMah > 0f) this.capacityMah = capacityMah
        val ma = milliamps(rawCurrent)
        lastMa = ma ?: -1f

        // The screen went off / came back on.
        if (screenWasOn && !screenOn) {
            offSinceMs = nowMs; offSum = 0.0; offCount = 0
        } else if (!screenWasOn && screenOn && offCount >= MIN_OFF_SAMPLES) {
            doneMa = (offSum / offCount).toFloat()
            doneMs = offLastMs - offFirstMs + 1000L
        }
        screenWasOn = screenOn
        if (ma == null || onExternalPower) return

        recent.addLast(Sample(nowMs, ma))
        while (recent.isNotEmpty() && nowMs - recent.first().atMs >= RECENT_MS) recent.removeFirst()
        if (!screenOn && nowMs - offSinceMs >= SETTLE_MS) {
            if (offCount == 0) offFirstMs = nowMs
            offLastMs = nowMs
            offSum += ma; offCount++
        }
    }

    /** The two lines for the debug text. */
    fun text(): String {
        if (external) return "Battery: on external power - no reading"
        if (lastMa < 0f || recent.isEmpty()) return "Battery: this phone does not report its current"
        var sum = 0.0
        for (s in recent) sum += s.ma
        val avg = (sum / recent.size).toFloat()
        val spanS = (recent.last().atMs - recent.first().atMs) / 1000 + 1
        val first = String.format(Locale.US, "Battery: %.0f mA now, %.0f mA average of the last %d s%s",
            lastMa, avg, spanS, extras(avg))
        val second = if (doneMa >= 0f)
            String.format(Locale.US, "Last time the screen was off: %.0f mA average over %s%s",
                doneMa, duration(doneMs), extras(doneMa))
        else "Screen off: no figure yet (switch the screen off for a minute or more, then look again)"
        return "$first\n$second"
    }

    /** " (1.76 W, about 9.1 % of the battery per hour)" - whatever of it is known. */
    private fun extras(ma: Float): String {
        val parts = ArrayList<String>()
        if (volts > 0f) parts.add(String.format(Locale.US, "%.2f W", ma * volts / 1000f))
        if (capacityMah > 0f) parts.add(String.format(Locale.US, "about %.1f %% of the battery per hour", ma / capacityMah * 100f))
        return if (parts.isEmpty()) "" else " (" + parts.joinToString(", ") + ")"
    }

    private fun duration(ms: Long): String {
        val s = ms / 1000
        return if (s < 60) "$s s" else String.format(Locale.US, "%d min %02d s", s / 60, s % 60)
    }

    companion object {
        private const val RECENT_MS = 60_000L
        private const val SETTLE_MS = 10_000L          // after the screen goes off: leave these out
        private const val MIN_OFF_SAMPLES = 20         // a screen-off period shorter than this is no measurement

        /**
         * The phone's current reading as mA DRAWN (a positive number), or null when it is
         * not usable. Android says "microamperes, negative while discharging", but phones
         * differ: some give the sign the other way round, a few give milliamperes, and
         * "not supported" is Int.MIN_VALUE (or 0). A phone playing radio with a dongle
         * draws somewhere between 100 mA and a few A, which tells the two units apart:
         * a number below 10,000 can only be mA, one above only uA.
         */
        fun milliamps(raw: Int): Float? {
            if (raw == Int.MIN_VALUE || raw == 0) return null
            val a = Math.abs(raw.toLong())
            val ma = if (a < 10_000L) a.toFloat() else a / 1000f
            return if (ma > 20_000f) null else ma
        }

        /**
         * The battery's whole capacity in mAh from what is left in it ([chargeMicroAh] =
         * BATTERY_PROPERTY_CHARGE_COUNTER) and the percentage shown; 0 = unknown.
         */
        fun capacityMah(chargeMicroAh: Int, levelPct: Int): Float {
            if (chargeMicroAh <= 0 || levelPct < 5 || levelPct > 100) return 0f
            val mah = chargeMicroAh / 1000f / (levelPct / 100f)
            return if (mah < 500f || mah > 20_000f) 0f else mah
        }
    }
}
