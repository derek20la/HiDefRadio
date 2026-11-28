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
 * Build 39 - FM stations with HD. The tune's auto-gain now looks at the HD
 * sidebands themselves and picks the gain where they stand out of the noise
 * best (the "quality sweep", doAutoGainFm in native-lib.cpp; [tick]'s
 * sweepChose says it did). That gain may be well below what the peak allows:
 * on 103.5 KOST in Long Beach the HD is best at a peak of -9 dBFS and gone
 * at -3. Rule 2 knows nothing of that - "quiet, so up" is exactly what put
 * KOST back on the gain that kills its HD. So after a sweep:
 *  - Rule 2 doesn't go ABOVE the sweep's gain. A MER try (rule 3) still may,
 *    and if it is kept, that gain is the new limit ("the highest tested gain").
 *    If HD hasn't been in sync for [SWEEP_LIFT_MS], the limit is dropped: the
 *    signal has changed (or gone), and the peak is all there is to go by.
 *  - A try UP that loses the MER altogether (the HD dropped out up there)
 *    counts as "worse": that step becomes a ceiling, like a try with a worse MER.
 *  - [gainTrusted] says whether the gain now set is a TESTED one: chosen by
 *    the sweep, or by a MER try that was kept - and not moved by rule 1 or 2
 *    since. RadioService remembers only such a gain (GainMemory).
 * Without a sweep (AM, FM stations without HD) nothing here differs from build 38.
 *
 * Build 41 - FM only ([tick]'s fm; AM is exactly as in build 40). Two things seen in
 * Long Beach on 11-20, where strong stations overloaded the tuner:
 *  - Rule 1, a REAL clip. At a peak of [CLIP_DBFS] or more the dongle's converter is
 *    flat out (0 dBFS = every one of its 256 levels in use) and the signal is being cut
 *    off - how far too loud it is, the peak can no longer say. One step down at a time
 *    took 11 steps (some 20 s of clipping) on 94.7 KTWV. Now a clip goes down
 *    [CLIP_DROP_TENTHS] (6 dB) at once; rule 2 brings it back up a step at a time if
 *    that was too much. (-1.5 ... -0.5 dBFS: one step, as before - until "later the same day".)
 *  - Rule 3, an EARLY try down. Synced, but the MER below zero on BOTH sidebands for
 *    [EARLY_READINGS] readings (~10 s): the HD is in trouble, and on 11-20 the cause was
 *    overload the peak couldn't see (95.5 KLOS: peak -10.9 dBFS, MER -11). Don't wait for
 *    the next scheduled try - try about 3 dB down ([EARLY_DROP_TENTHS]) now. Kept (the MER
 *    got better): again ~10 s later if it is still below zero, so it walks down while that
 *    helps. Not kept: no more early tries on this station (a WEAK station can have a MER
 *    that low too, and less gain only makes it worse) - the usual tries go on as before.
 *    And while the MER is below zero, rule 2 does not go up on the peak alone: more gain
 *    is what hurts an overloaded tuner.
 *  Stations that never sync (no HD) only see the rule 1 change.
 *
 * Build 41, later the same day (after the LA set, 99.1 KGGI at night and Auto live in Long Beach):
 *  - Rule 1's 6 dB only for a converter that is FULL ([FULL_SCALE_DBFS], 0.0 dBFS) two seconds
 *    running. A weak station next to loud ones can be at its best just under that: 99.1 KGGI at
 *    28.0 dB read -0.0 ... -0.6 dBFS every second, a full 0.0 in only 6-22 % of them, and its HD
 *    was better there than anywhere else. The 6 dB took it to 22.9 dB (BER 4.8 -> 6.5 %); one
 *    step, as before, to 25.4 (5.9 %). A real clip reads 0.0 every second and still goes 6 dB.
 *    In practice: the first full second after a calmer one -> one step; still full at that next
 *    gain -> 6 dB. ("Running" counts the half-and-half second after a change, which reads full if
 *    the gain before it clipped: a gain just below one that clipped needs one full second of its own.)
 *  - Rule 1, LOUD but not clipping (-1.5 ... -0.5 dBFS), FM: one step down only if it is so in
 *    [NEAR_CLIP_COUNT] of the last [NEAR_CLIP_SECONDS] seconds at this gain. Under -0.5 dBFS no
 *    sample was cut off, so nothing is lost by waiting; a real clip still acts at once. 99.1 KGGI
 *    at 7 PM in Long Beach is at its best at 19.7 dB (BER 7 %), but its neighbours push the peak
 *    over -1.5 now and then (one second in 29, -0.9 at the most): one such second took it to
 *    14.4 dB (BER 22 %) and kept it off 19.7 for two minutes - over and over ("Auto kept going
 *    lower"). Replayed on the 7 PM recordings: BER 20.6 % -> 7.7 %, on 19.7 at the end of every
 *    run. A gain that is loud ALL the time still goes down, as before: in downtown LA, KOST at
 *    14.4 dB (-1.1 ... -1.6 every second, and overloaded) - and also KPCC at 28.0 and KDAY at
 *    14.4, which were as good as anything there (-0.4 ... -1.4 every second): a little lost, as
 *    in every build. How loud is too loud for the HD is a question for another day.
 *  - The "no sync -> lower" walk (rule 4 below). The tune's search picked a gain for its HD (or
 *    a remembered one) but the HD never syncs. An overload the peak can't see can look like HD
 *    to the search: on 103.5 KOST in downtown LA the overloaded tuner's own mixing products fill
 *    the HD sidebands' place at 40.2 dB ("HD" 7-9 dB over the floor) while the real HD has no
 *    sync at all there. Those recordings clip, but scaled down to a calm peak - what strong
 *    stations OUTSIDE our 1.5 MHz did in Long Beach on 11-20 - the search took 40.2 dB in 6 of
 *    16 tunes, and nothing else in here could help: no MER readings, a calm peak.
 *  4. After a gain chosen for HD, no sync since the tune for [NOSYNC_MS] (8 s): try
 *     [SYNC_DROP_TENTHS] (6 dB) lower, wait up to [SYNC_WAIT_MS] for sync, again lower if there
 *     is none - until it syncs (stay there: rule 2 doesn't climb back above it, a MER try may),
 *     the peak gets quiet ([WALK_MIN_PEAK_DBFS]), or [SYNC_WALK_END_MS] (26 s) have gone by since
 *     the tune: then back to where the walk started. Once per tune. Only:
 *       - at the START of a tune, before the HD has ever synced. HD that synced and then faded
 *         is a weak signal, not an overload, and less gain would only make it weaker.
 *       - while the HD decoder runs ([tick]'s hdDecoding: Auto or Digital only - in Analog only
 *         it is off, so "never synced" means nothing there).
 *       - within the HD search's first 30 s: after that it rests (hdsearch.hpp) and would not
 *         notice the HD at a new gain anyway.
 *     The analog FM's quieting is the safety check (Auto: that is what you hear meanwhile).
 *     Measured on the 11-23 recordings at every gain: on an OVERLOADED tuner the quieting gets
 *     much worse too (LA KOST -15 ... -18 dB at 20.7-40.2, -44 at 7.7; LA KLOS -27 at 40.2, -47 at 8.7),
 *     and less gain makes it better. On a tuner that is NOT overloaded it hardly changes with the
 *     gain (KGGI -26 ... -29 from 3.7 to 28.0 dB; KPCC LA -33 ... -39 from 8.7 to 33.8). So if a
 *     step makes the quieting [QUIET_WORSE_DB] worse than before the walk, this is no overload:
 *     back at once, no more steps. (Digital only: no analog, no check - and nothing to lose.)
 *
 * Build 42 - AM ([tick]'s am; left out = AM exactly as in build 41). The tune's auto-gain on
 * AM now looks at the HD too (doAutoGainAm in native-lib.cpp), because on the V4's AM input the
 * peak says little about the gain: past ~30 dB it hardly moves (the tuner chain compresses - on
 * XEKAM 950 the peak never reached -4 dBFS, so the old search went to the top step, 49.6 dB,
 * where the HD doesn't decode), and the V4's own oscillator, leaking through at the bottom of the
 * AM band, can be the loudest thing on screen at -1 ... -3 dBFS without doing any harm (KBRT 740
 * at 33.8 dB, its best gain). Two rules had to follow:
 *  - Rule 1 on AM acts only on a REAL clip, [CLIP_DBFS] (-0.5) or louder - as the tune's search
 *    does. Then one step down, or [CLIP_DROP_TENTHS] at once for a converter that is full two
 *    seconds running (as on FM). -1.5 ... -0.5 dBFS on AM is that leak, not an overload.
 *  - Rule 2 on AM never climbs above the gain the tune chose ([tick]'s sweepChose is true for every
 *    AM tune now, with or without HD), and that limit is never lifted: "quiet, so up" is exactly
 *    how the old AM rules ended on 49.6 dB. Below it, rule 2 works as before (back up after rule 1
 *    stepped down, once it is quiet again).
 *  Rules 3 and 4 never run on AM: nrsc5 sends no MER on AM, and rule 4 is FM only.
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

    /** Build 41: the try running now is a step of rule 4's walk (no HD sync since the tune - see the top). */
    val walking: Boolean
        get() = trySync

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

    // Build 39 (see the top)
    /** The gain now set was chosen by the quality sweep or by a kept MER try (and not moved by rule 1 or 2 since). */
    var gainTrusted = false
        private set
    private var first = true                // the first reading after restart() is still to come
    private var sweepTop = -1               // after a quality sweep: the highest tested step; rule 2 stays at or below it (-1 = no sweep)
    private var lastLockedMs = 0L           // when HD was last in sync (or the restart)

    // Build 41 (see the top)
    private var earlyOn = true              // early tries down still allowed on this station (none has failed)
    private var tryEarly = false            // the try running now is an early one
    // Build 41, later (see the top)
    private var lastPeak = -99f             // rule 1: the second before this one (any gain)
    private var tunedAtMs = 0L              // rule 4: the restart (the tune)
    private var everLocked = false          // rule 4: the HD has been in sync since the tune
    private var syncWalkOn = true           // rule 4: the walk may still run on this station
    private var trySync = false             // rule 4: the "try" running now is a step of the walk
    private var walkFrom = -1               // rule 4: where the walk started (back there if no step syncs)
    private val quietHere = ArrayList<Float>()   // rule 4: analog quieting readings at the current gain
    private val loudHere = ArrayDeque<Boolean>() // rule 1, FM: the last seconds at this gain - louder than OVERLOAD_DBFS?
    private var quietRef = Float.NaN        // rule 4: the quieting before the walk (NaN = not known: no check)

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
        gainTrusted = false
        first = true
        sweepTop = -1
        lastLockedMs = nowMs
        earlyOn = true
        lastPeak = -99f
        tunedAtMs = nowMs
        everLocked = false
        syncWalkOn = true
        walkFrom = -1
        quietHere.clear()
        quietRef = Float.NaN
        loudHere.clear()
    }

    /** How long the gain has stayed the same (ms). 0 while a try is running. */
    fun stableForMs(nowMs: Long): Long = if (tryFrom >= 0) 0L else nowMs - stableSinceMs

    /**
     * One reading, once a second.
     * [gainTenths] = the gain now set, [peakDbfs] = the loudest peak since the
     * last reading (-99 = no samples), [locked] = HD sync right now,
     * [merCount] / [merLower] / [merUpper] = native's MER counter and the latest MER.
     * [sweepChose] (build 39) = this tune's gain came from the FM quality sweep
     * (Signal.gainSwept); looked at on the first reading after [restart] only.
     * [fm] (build 41) = the station is on FM: the clip rule and the early tries (see the
     * top). false = AM, every answer as in build 40.
     * [hdDecoding] (build 41) = the HD decoder runs (Auto or Digital only, not Analog only), and
     * [quietDb] = the analog FM's quieting (Signal.fmQuietDb, -99 = not measured): rule 4.
     * Left out, rule 4 never runs.
     * [am] (build 42) = the station is on AM: rule 1 only for a real clip, rule 2 never above the
     * tune's gain (see the top). false = every answer as in build 41 (fm = false was "AM" until then).
     * Returns the gain to switch to (tenths of a dB), or [NO_CHANGE].
     */
    fun tick(nowMs: Long, gainTenths: Int, peakDbfs: Float,
             locked: Boolean, merCount: Int, merLower: Float, merUpper: Float,
             sweepChose: Boolean = false, fm: Boolean = false,
             hdDecoding: Boolean = false, quietDb: Float = -99f, am: Boolean = false): Int {
        if (steps.isEmpty()) return NO_CHANGE

        // Build 39: a new station - did the quality sweep choose its gain?
        if (first) {
            first = false
            lastLockedMs = nowMs
            tunedAtMs = nowMs
            if (sweepChose) {
                sweepTop = nearestIndex(gainTenths)
                gainTrusted = true
            }
        }
        if (locked) { lastLockedMs = nowMs; everLocked = true }

        // A NEW MER reading (one every ~1.5 s)? Collect it for rule 3.
        if (merCount != lastMerCount) {
            val isNew = lastMerCount >= 0 && merCount > lastMerCount
            lastMerCount = merCount
            if (isNew && locked) {
                if (skipMer > 0) skipMer-- else merHere.add(maxOf(merLower, merUpper))
            }
        }

        if (peakDbfs < NO_DATA_DBFS) return NO_CHANGE
        val prevPeak = lastPeak             // build 41: the second before this one (any gain - see the top)
        lastPeak = peakDbfs
        if (settle > 0) {
            settle--
            return NO_CHANGE
        }
        if (quietDb > NO_DATA_DBFS) quietHere.add(quietDb)     // rule 4 (not the half-and-half second)
        val i = nearestIndex(gainTenths)
        loudHere.addLast(peakDbfs > OVERLOAD_DBFS)
        while (loudHere.size > NEAR_CLIP_SECONDS) loudHere.removeFirst()
        // Build 41, FM: loud but not clipping - only if it keeps happening (see the top).
        val waitAndSee = fm && peakDbfs < CLIP_DBFS && loudHere.count { it } < NEAR_CLIP_COUNT

        // Rule 1: overload -> down, now. (Also ends a try.)
        // Build 41, FM: a real clip -> CLIP_DROP_TENTHS down in one go, else one step.
        // Build 42, AM: only a real clip counts as an overload (see the top).
        val overload = if (am) peakDbfs >= CLIP_DBFS else (peakDbfs > OVERLOAD_DBFS && !waitAndSee)
        if (overload) {
            endTry()
            if (i == 0) return NO_CHANGE            // already at the lowest gain
            blockedIndex = i
            blockedUntilMs = nowMs + UP_BLOCK_MS
            gainTrusted = false                     // build 39: moved by the peak, not tested
            // ... and only for a converter that is FULL two seconds running: just under that, one step
            // (99.1 KGGI is at its best at 28.0 dB, -0.0 ... -0.6 dBFS - see the top).
            val fullScale = peakDbfs >= FULL_SCALE_DBFS && prevPeak >= FULL_SCALE_DBFS
            val to = if ((fm || am) && fullScale) stepBelow(i, CLIP_DROP_TENTHS) else i - 1
            return change(nowMs, i, to, stays = true)
        }

        recent.addLast(peakDbfs)
        while (recent.size > QUIET_TICKS) recent.removeFirst()

        // Rule 3, part 2: a try is running -> better, or back?
        if (tryFrom >= 0) return judgeTry(nowMs, i)

        // Rule 4 (build 41), FM: a gain chosen for its HD, and no sync at all since the tune ->
        // walk down until it syncs (see the top). Out of sync there is no HD audio to lose.
        if (fm && hdDecoding && !everLocked && syncWalkOn && sweepTop >= 0 && i > 0 &&
                nowMs - tunedAtMs >= NOSYNC_MS && nowMs - tunedAtMs < SYNC_WALK_END_MS &&
                (recent.maxOrNull() ?: -99f) > WALK_MIN_PEAK_DBFS) {
            syncWalkOn = false
            walkFrom = i
            quietRef = if (quietHere.size >= QUIET_READINGS) quietHere.takeLast(QUIET_REF_READINGS).average().toFloat() else Float.NaN
            tryFrom = i
            tryTo = stepBelow(i, SYNC_DROP_TENTHS)
            tryStartMs = nowMs
            trySync = true
            return change(nowMs, i, tryTo, stays = false)
        }

        // Build 41, FM: the HD's MER is below zero on both sidebands (merHere keeps the better
        // one). [merBad] = the latest reading says so; [early] = the last EARLY_READINGS
        // readings at this gain all did - time for an early try down (see the top).
        val merBad = fm && locked && merHere.isNotEmpty() && merHere.last() < EARLY_MER_DB
        if (merBad && earlyOn && i > 0 && merHere.size >= EARLY_READINGS &&
                merHere.takeLast(EARLY_READINGS).all { it < EARLY_MER_DB }) {
            tries++
            checkPending = false
            tryBase = merHere.takeLast(BASE_READINGS).average().toFloat()
            tryFrom = i
            tryTo = stepBelow(i, EARLY_DROP_TENTHS)
            tryStartMs = nowMs
            tryEarly = true
            return change(nowMs, i, tryTo, stays = false)
        }

        // Rule 2: quiet for a while -> up, if the next step still stays under the target
        // (and isn't blocked by an overload or a worse MER a moment ago). While synced,
        // one step at a time: the MER try that checks the last step comes first.
        // Build 39: and not above the gain the quality sweep chose (see the top).
        // Build 41: and not while the MER is below zero (FM).
        if (recent.size == QUIET_TICKS && i < steps.lastIndex && mayGoUpTo(i + 1, nowMs) &&
                !(locked && checkPending) && sweepAllows(i + 1, nowMs, am) && !merBad) {
            val loudest = recent.maxOrNull() ?: return NO_CHANGE
            if (loudest + rise(i) < TARGET_DBFS) {
                // Went up by the peak alone -> check soon with a MER try DOWN
                // that it really is better up here (see "Which MER?" above).
                preferUp = false
                nextTryMs = minOf(nextTryMs, nowMs + TRY_AGAIN_MS)
                checkPending = true
                gainTrusted = false                 // build 39: moved by the peak, not tested
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
        if (trySync) return judgeSyncWalk(nowMs, i)         // rule 4 (build 41)
        val from = tryFrom
        val to = tryTo
        val up = to > from
        if (merHere.size >= TRY_READINGS) {
            val mer = merHere.average().toFloat()
            if (mer >= tryBase + KEEP_MARGIN_DB) {
                // Better: stay here. Soon try one more step the same way.
                kept++
                record(nowMs, from, to)
                gainTrusted = true                  // build 39: a tested gain ...
                if (sweepTop >= 0) sweepTop = to    // ... and rule 2's new limit
                // The higher gain was worse: stay below it. (Build 41: to + 1 - the same step as
                // [from] for a one-step try; for an early try, every step between was above the
                // gain that turned out better, so it counts as "worse" too.)
                if (!up) setCeiling(to + 1, nowMs)
                preferUp = up
                failWaitMs = TRY_FAIL_WAIT_MS
                nextTryMs = nowMs + TRY_AGAIN_MS
                endTry()
                return NO_CHANGE                    // (merHere = this gain's readings: the next base)
            }
            // Not better: back to where we were, and wait longer before the next try.
            if (tryEarly) return earlyFailed(nowMs, i, from)    // build 41
            if (up) setCeiling(to, nowMs)
            preferUp = !up
            nextTryMs = nowMs + failWaitMs
            failWaitMs = minOf(failWaitMs * 2, TRY_FAIL_WAIT_MAX_MS)
            endTry()
            return change(nowMs, i, from, stays = false)
        }
        if (nowMs - tryStartMs > TRY_TIMEOUT_MS) {  // no MER readings (sync lost?) -> back
            if (tryEarly) return earlyFailed(nowMs, i, from)    // build 41
            // Build 39: after a quality sweep, "the HD dropped out up there" is as good a
            // "worse" as a lower MER: stay below that step for a while.
            if (up && sweepTop >= 0) setCeiling(to, nowMs)
            nextTryMs = nowMs + failWaitMs
            endTry()
            return change(nowMs, i, from, stays = false)
        }
        return NO_CHANGE
    }

    /**
     * Rule 4 (build 41): one step of the "no sync -> lower" walk is running (see the top).
     *  - In sync (the first time since the tune): stay. A tested gain now - the HD syncs here and
     *    not where the walk began - and rule 2's limit: it doesn't climb back blind, a MER try may.
     *  - The analog got clearly worse than before the walk: no overload - back, and the walk ends.
     *  - No sync after [SYNC_WAIT_MS]: one more step down while there is room, time and level;
     *    else back to where the walk began.
     */
    private fun judgeSyncWalk(nowMs: Long, i: Int): Int {
        if (everLocked) {
            val from = walkFrom
            endTry()
            record(nowMs, from, i)
            gainTrusted = true
            sweepTop = i
            return NO_CHANGE
        }
        if (!quietRef.isNaN() && quietHere.size >= QUIET_READINGS &&
                quietHere.takeLast(QUIET_READINGS).average() > quietRef + QUIET_WORSE_DB) return endWalk(nowMs, i)
        if (nowMs - tryStartMs < SYNC_WAIT_MS) return NO_CHANGE
        if (i > 0 && nowMs - tunedAtMs < SYNC_WALK_END_MS && (recent.maxOrNull() ?: -99f) > WALK_MIN_PEAK_DBFS) {
            tryTo = stepBelow(i, SYNC_DROP_TENTHS)
            tryStartMs = nowMs
            return change(nowMs, i, tryTo, stays = false)
        }
        return endWalk(nowMs, i)
    }

    /** Rule 4: the walk found nothing - back to where it began (the tune's gain, untouched). */
    private fun endWalk(nowMs: Long, i: Int): Int {
        val back = walkFrom
        endTry()
        return change(nowMs, i, back, stays = false)
    }

    /**
     * Build 41: an early try down (see the top) did not help - back to step [from]. No more
     * early tries on this station; the usual tries go on as if this one had not happened
     * (the same direction and waits), only not sooner than [TRY_AGAIN_MS] from now.
     */
    private fun earlyFailed(nowMs: Long, i: Int, from: Int): Int {
        earlyOn = false
        nextTryMs = maxOf(nextTryMs, nowMs + TRY_AGAIN_MS)
        endTry()
        return change(nowMs, i, from, stays = false)
    }

    /** Not blocked by an overload (rule 1) or a worse MER (rule 3) right now? */
    private fun mayGoUpTo(index: Int, nowMs: Long): Boolean {
        val blocked = blockedIndex in 0..index && nowMs < blockedUntilMs
        val capped = ceilingIndex in 0..index && nowMs < ceilingUntilMs
        return !blocked && !capped
    }

    /**
     * Build 39, rule 2 only: after a quality sweep, not above the highest tested step -
     * unless HD has been out of sync for [SWEEP_LIFT_MS] (then the peak is all we have).
     * Build 42: on AM ([am]) never - there the peak is no guide to more gain at all (see the top).
     */
    private fun sweepAllows(index: Int, nowMs: Long, am: Boolean): Boolean =
        sweepTop < 0 || index <= sweepTop || (!am && nowMs - lastLockedMs >= SWEEP_LIFT_MS)

    private fun setCeiling(index: Int, nowMs: Long) {
        ceilingIndex = index
        ceilingUntilMs = nowMs + CEILING_MS
    }

    private fun endTry() {
        tryFrom = -1
        tryTo = -1
        tryEarly = false
        trySync = false
    }

    /** dB from step [i] to the next one up (e.g. 3.7 -> 7.7 dB = 4.0). */
    private fun rise(i: Int): Float = (steps[i + 1] - steps[i]) / 10f

    /**
     * Build 41: the highest step at least [dropTenths] below step [i] (e.g. 40.2 dB, 6 dB ->
     * 33.8 on a V4), or step 0 if there is none that far down. Always at least one step down.
     */
    private fun stepBelow(i: Int, dropTenths: Int): Int {
        var k = i - 1
        while (k > 0 && steps[i] - steps[k] < dropTenths) k--
        return k
    }

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
        quietHere.clear()                   // (and quieting readings - rule 4)
        loudHere.clear()                    // (and rule 1's loud seconds)
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

        // Build 39 (after a quality sweep)
        const val SWEEP_LIFT_MS = 60_000L         // HD out of sync this long -> rule 2 may go above the sweep's gain again

        // Build 41 (FM only, see the top)
        const val CLIP_DBFS = -0.5f               // rule 1: a peak this loud is a real clip (as native GAIN_NO_EFFECT_CLIP_DBFS;
                                                  //   build 42: AM's only overload, = native AM_CLIP_DBFS)
        const val CLIP_DROP_TENTHS = 60           // ... -> this far down at once (6 dB; on Long Beach's 11-23 recordings
                                                  //   28.0, 33.8 and 40.2 dB all read 0.0 dBFS on KLOS, KOST and KDAY)
        const val EARLY_MER_DB = 0f               // rule 3: "the HD is in trouble" = the better MER is below this ...
        const val EARLY_READINGS = 6              // ... for this many readings in a row (one every ~1.5 s: ~10 s)
        // Build 41, later (see the top)
        const val FULL_SCALE_DBFS = -0.05f        // rule 1: "the converter is full" = the peak reads 0.0 dBFS
        const val NEAR_CLIP_SECONDS = 5           // rule 1, FM, loud but not clipping: step down only if it was so
        const val NEAR_CLIP_COUNT = 3             //   in this many of the last NEAR_CLIP_SECONDS seconds at this gain
        const val NOSYNC_MS = 8_000L              // rule 4: no sync this long after the tune -> walk down (nrsc5
                                                  //   syncs within half a second where it can: hdsearch.hpp)
        const val SYNC_WAIT_MS = 6_000L           // rule 4: wait this long for sync at each step
        const val SYNC_WALK_END_MS = 26_000L      // rule 4: no step after this long since the tune (the HD search
                                                  //   rests at 30 s and wouldn't see HD at the new gain)
        const val SYNC_DROP_TENTHS = 60           // rule 4: each step this far down (6 dB; LA KOST: none at 20.7, fine at 14.4)
        const val WALK_MIN_PEAK_DBFS = -16f       // rule 4: not below a peak this quiet (as native SWEEP_MIN_PEAK_DBFS)
        const val QUIET_READINGS = 3              // rule 4: quieting readings (~1 s each) at a step before it is judged
        const val QUIET_REF_READINGS = 5          // ... and before the walk, at most (the reference)
        const val QUIET_WORSE_DB = 3f             // ... this much worse than the reference = no overload: back
                                                  //   (one reading scatters +-2-3 dB; 6 dB less gain on a tuner that
                                                  //   is NOT overloaded: 0-3 dB worse on the 11-23 sets, 8 on one)
        const val EARLY_DROP_TENTHS = 30          // the early try goes at least this far down (3 dB: on an overloaded tuner
                                                  //   one 1-2 dB step moves the MER less than its scatter, +-1-2 dB)
        private const val NO_DATA_DBFS = -90f
    }
}
