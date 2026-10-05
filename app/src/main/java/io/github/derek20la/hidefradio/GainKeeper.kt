package io.github.derek20la.hidefradio

/**
 * Milestone 10c: the gain watchdog for the AUTO gain.
 *
 * Auto-gain picks a gain once, when you tune. But signals change while you
 * listen (tropo, moving the phone, a car going by), so RadioService calls
 * [tick] once a second with the gain now set, the LOUDEST peak of that
 * second and the latest MER. [tick] answers with the gain to switch to, or
 * [NO_CHANGE]. RadioService then sets it on the engine thread (live, no gap).
 *
 * The rules (0 dBFS = the dongle uses its whole 0..255 range = clipping):
 *  1. Overload guard: peak above [OVERLOAD_DBFS] -> one step DOWN, right away.
 *     The step that overloaded is then off-limits for [UP_BLOCK_MS], so we
 *     don't climb straight back into the overload (no ping-pong).
 *  2. Too quiet: if for [QUIET_TICKS] seconds even the loudest peak was so low
 *     that one step up would STILL stay below [TARGET_DBFS] -> one step UP.
 *     (Same target as the auto-gain search in native-lib.cpp.)
 *  3. (Step 2) MER tries: now and then, while synced, TRY one step up or down
 *     for a few seconds. Is the MER at least [KEEP_MARGIN_DB] better there?
 *     Keep it (and soon try one more step the same way). Otherwise go back,
 *     and wait longer before the next try. The peak can't see everything:
 *     a very strong station just OUTSIDE our 1.5 MHz window can overload the
 *     tuner while our Peak looks fine - only the MER shows that.
 *     A gain that turned out worse becomes a "ceiling" for [CEILING_MS], so
 *     rule 2 doesn't climb right back up to it.
 *  After every change, the next [SETTLE_TICKS] peak reading and the next MER
 *  reading are ignored: they're half old gain, half new.
 *
 * Which MER? The BETTER of the two sidebands. The worse one is often limited
 * by a station next door (e.g. KKGO's upper sideband by 105.3 KIOZ), which no
 * gain setting can fix, and it jumps around - it would only add noise.
 *
 * Plain Kotlin (no Android), so it can be tested on its own.
 * Used on the main thread only.
 */
class GainKeeper(steps: IntArray) {

    /** The tuner's gain steps in tenths of a dB, low to high (e.g. 0, 9, 14 ... 496). */
    private val steps = steps.sortedArray()

    // What the details screen shows, for the station you're on.
    var downs = 0                  // changes that stayed (rules 1-2, and MER tries that were kept)
        private set
    var ups = 0
        private set
    var lastFrom = -1              // the last change that stayed: from ... to ... (tenths of a dB), -1 = none
        private set
    var lastTo = -1
        private set
    var lastAtMs = 0L              // ... and when (the same clock as tick()'s nowMs)
        private set
    var tries = 0                  // rule 3: MER tries on this station ...
        private set
    var kept = 0                   // ... and how many of them were better
        private set

    /** The gain being tried right now (tenths of a dB), or -1. */
    val tryingTo: Int
        get() = if (tryTo >= 0) steps[tryTo] else -1

    private var stableSinceMs = 0L          // last change that stayed, or restart
    private var settle = 0                  // peak readings still to ignore
    private val recent = ArrayDeque<Float>()   // the last few peaks (rules 2 and 3)
    private var blockedIndex = -1           // rule 1: no going up to this step or higher ...
    private var blockedUntilMs = 0L         // ... until then

    // Rule 3
    private var lastMerCount = -1           // native's MER counter at the last tick (-1 = not seen yet)
    private var skipMer = 0                 // MER readings still to ignore
    private val merHere = ArrayList<Float>()   // MER readings at the current gain
    private var tryFrom = -1                // trying: the step we came from (-1 = not trying) ...
    private var tryTo = -1                  // ... and the step we're trying
    private var tryBase = 0f                // the MER before the try
    private var tryStartMs = 0L
    private var nextTryMs = 0L
    private var failWaitMs = TRY_FAIL_WAIT_MS
    private var preferUp = true             // which way to try next
    private var ceilingIndex = -1           // a step that was worse: stay below it ...
    private var ceilingUntilMs = 0L         // ... until then
    private var checkPending = false        // rule 2 went up; a MER try down should check it first

    /** Start over (a new station, gain set by hand, radio off...). */
    fun restart(nowMs: Long) {
        downs = 0
        ups = 0
        lastFrom = -1
        lastTo = -1
        lastAtMs = 0L
        tries = 0
        kept = 0
        stableSinceMs = nowMs
        settle = 0
        recent.clear()
        blockedIndex = -1
        blockedUntilMs = 0L
        lastMerCount = -1
        skipMer = 0
        merHere.clear()
        endTry()
        nextTryMs = nowMs + FIRST_TRY_MS
        failWaitMs = TRY_FAIL_WAIT_MS
        preferUp = true
        ceilingIndex = -1
        ceilingUntilMs = 0L
        checkPending = false
    }

    /** How long the gain has stayed the same (ms). 0 while a try is running. */
    fun stableForMs(nowMs: Long): Long = if (tryFrom >= 0) 0L else nowMs - stableSinceMs

    /**
     * One reading, once a second.
     * [gainTenths] = the gain now set, [peakDbfs] = the loudest peak since the
     * last reading (-99 = no samples), [locked] = HD sync right now,
     * [merCount] / [merLower] / [merUpper] = native's MER counter and the latest MER.
     * Returns the gain to switch to (tenths of a dB), or [NO_CHANGE].
     */
    fun tick(nowMs: Long, gainTenths: Int, peakDbfs: Float,
             locked: Boolean, merCount: Int, merLower: Float, merUpper: Float): Int {
        if (steps.isEmpty()) return NO_CHANGE

        // A NEW MER reading (one every ~1.5 s)? Collect it for rule 3.
        if (merCount != lastMerCount) {
            val isNew = lastMerCount >= 0 && merCount > lastMerCount
            lastMerCount = merCount
            if (isNew && locked) {
                if (skipMer > 0) skipMer-- else merHere.add(maxOf(merLower, merUpper))
            }
        }

        if (peakDbfs < NO_DATA_DBFS) return NO_CHANGE
        if (settle > 0) {
            settle--
            return NO_CHANGE
        }
        val i = nearestIndex(gainTenths)

        // Rule 1: overload -> down, now. (Also ends a try.)
        if (peakDbfs > OVERLOAD_DBFS) {
            endTry()
            if (i == 0) return NO_CHANGE            // already at the lowest gain
            blockedIndex = i
            blockedUntilMs = nowMs + UP_BLOCK_MS
            return change(nowMs, i, i - 1, stays = true)
        }

        recent.addLast(peakDbfs)
        while (recent.size > QUIET_TICKS) recent.removeFirst()

        // Rule 3, part 2: a try is running -> better, or back?
        if (tryFrom >= 0) return judgeTry(nowMs, i)

        // Rule 2: quiet for a while -> up, if the next step still stays under the target
        // (and isn't blocked by an overload or a worse MER a moment ago). While synced,
        // one step at a time: the MER try that checks the last step comes first.
        if (recent.size == QUIET_TICKS && i < steps.lastIndex && mayGoUpTo(i + 1, nowMs) &&
                !(locked && checkPending)) {
            val loudest = recent.maxOrNull() ?: return NO_CHANGE
            if (loudest + rise(i) < TARGET_DBFS) {
                // Went up by the peak alone -> check soon with a MER try DOWN
                // that it really is better up here (see "Which MER?" above).
                preferUp = false
                nextTryMs = minOf(nextTryMs, nowMs + TRY_AGAIN_MS)
                checkPending = true
                return change(nowMs, i, i + 1, stays = true)
            }
        }

        // Rule 3, part 1: time for a try?
        if (locked && nowMs >= nextTryMs && merHere.size >= BASE_READINGS) {
            val loudest = recent.maxOrNull()
            val canUp = i < steps.lastIndex && loudest != null &&
                        loudest + rise(i) < TRY_PEAK_MAX_DBFS && mayGoUpTo(i + 1, nowMs)
            val canDown = i > 0
            val up = if (preferUp) canUp else (canUp && !canDown)
            val target = when {
                up -> i + 1
                canDown -> i - 1
                else -> -1
            }
            if (target < 0) {
                nextTryMs = nowMs + failWaitMs      // nowhere to go; look again later
                return NO_CHANGE
            }
            tries++
            checkPending = false
            tryBase = merHere.takeLast(BASE_READINGS).average().toFloat()
            tryFrom = i
            tryTo = target
            tryStartMs = nowMs
            return change(nowMs, i, target, stays = false)
        }
        return NO_CHANGE
    }

    /** Rule 3: enough MER readings at the tried gain? Keep it or go back. */
    private fun judgeTry(nowMs: Long, i: Int): Int {
        val from = tryFrom
        val to = tryTo
        val up = to > from
        if (merHere.size >= TRY_READINGS) {
            val mer = merHere.average().toFloat()
            if (mer >= tryBase + KEEP_MARGIN_DB) {
                // Better: stay here. Soon try one more step the same way.
                kept++
                record(nowMs, from, to)
                if (!up) setCeiling(from, nowMs)    // the higher gain was worse
                preferUp = up
                failWaitMs = TRY_FAIL_WAIT_MS
                nextTryMs = nowMs + TRY_AGAIN_MS
                endTry()
                return NO_CHANGE                    // (merHere = this gain's readings: the next base)
            }
            // Not better: back to where we were, and wait longer before the next try.
            if (up) setCeiling(to, nowMs)
            preferUp = !up
            nextTryMs = nowMs + failWaitMs
            failWaitMs = minOf(failWaitMs * 2, TRY_FAIL_WAIT_MAX_MS)
            endTry()
            return change(nowMs, i, from, stays = false)
        }
        if (nowMs - tryStartMs > TRY_TIMEOUT_MS) {  // no MER readings (sync lost?) -> back
            nextTryMs = nowMs + failWaitMs
            endTry()
            return change(nowMs, i, from, stays = false)
        }
        return NO_CHANGE
    }

    /** Not blocked by an overload (rule 1) or a worse MER (rule 3) right now? */
    private fun mayGoUpTo(index: Int, nowMs: Long): Boolean {
        val blocked = blockedIndex in 0..index && nowMs < blockedUntilMs
        val capped = ceilingIndex in 0..index && nowMs < ceilingUntilMs
        return !blocked && !capped
    }

    private fun setCeiling(index: Int, nowMs: Long) {
        ceilingIndex = index
        ceilingUntilMs = nowMs + CEILING_MS
    }

    private fun endTry() {
        tryFrom = -1
        tryTo = -1
    }

    /** dB from step [i] to the next one up (e.g. 3.7 -> 7.7 dB = 4.0). */
    private fun rise(i: Int): Float = (steps[i + 1] - steps[i]) / 10f

    /** Counts a change that stayed, for the details screen. */
    private fun record(nowMs: Long, from: Int, to: Int) {
        if (to < from) downs++ else ups++
        lastFrom = steps[from]
        lastTo = steps[to]
        lastAtMs = nowMs
        stableSinceMs = nowMs
    }

    /**
     * Switches from step [from] to step [to] and returns the new gain.
     * [stays] = a real change (counted), false = the start or end of a try.
     */
    private fun change(nowMs: Long, from: Int, to: Int, stays: Boolean): Int {
        if (stays) record(nowMs, from, to)
        settle = SETTLE_TICKS
        recent.clear()
        merHere.clear()                     // MER readings belong to one gain
        skipMer = 1                         // the reading in progress is half old, half new
        return steps[to]
    }

    /** The index of the step closest to [gainTenths]. */
    private fun nearestIndex(gainTenths: Int): Int {
        var best = 0
        for (k in steps.indices) {
            if (Math.abs(steps[k] - gainTenths) < Math.abs(steps[best] - gainTenths)) best = k
        }
        return best
    }

    companion object {
        const val NO_CHANGE = -1
        const val OVERLOAD_DBFS = -1.5f     // rule 1 (the screen warns from -1.0)
        const val TARGET_DBFS = -4.0f       // rule 2 (= native AUTO_TARGET_DBFS)
        const val QUIET_TICKS = 10          // rule 2: seconds of "quiet" before going up
        const val UP_BLOCK_MS = 120_000L    // rule 1: 2 minutes
        const val SETTLE_TICKS = 1

        // Rule 3 (MER tries). MER comes every ~1.5 s, so a try takes ~5 s.
        const val FIRST_TRY_MS = 30_000L          // first try this long after tuning
        const val TRY_AGAIN_MS = 20_000L          // after a try that was better
        const val TRY_FAIL_WAIT_MS = 60_000L      // after a try that wasn't: 1, 2, 4, 8 minutes
        const val TRY_FAIL_WAIT_MAX_MS = 480_000L
        const val BASE_READINGS = 3               // MER readings before a try (the "before")
        const val TRY_READINGS = 2                // MER readings at the tried gain (the "after")
        const val KEEP_MARGIN_DB = 0.5f           // keep only if at least this much better
        const val TRY_TIMEOUT_MS = 10_000L        // no MER at the tried gain -> go back
        const val TRY_PEAK_MAX_DBFS = -2.5f       // try up only if the peak would stay below this
        const val CEILING_MS = 600_000L           // a worse gain stays off-limits for 10 minutes
        private const val NO_DATA_DBFS = -90f
    }
}
