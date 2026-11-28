// Harness only (build 39): the gain watchdog (GainKeeper.kt, plain Kotlin) on made-up signals.
//
//   kotlinc ../../app/src/main/java/io/github/derek20la/hidefradio/GainKeeper.kt gainkeepertest.kt -include-runtime -d gk.jar
//   java -jar gk.jar
//
// A tiny model of a station stands in for the radio: the peak rises dB for dB with the gain,
// and the HD's MER depends on the gain the way it did on 103.5 KOST in Long Beach (best at one
// gain, a little worse next to it, no HD at all two steps up). One tick = one second, as in
// RadioService.watchGain(). Every check prints PASS or FAIL; the last line counts the FAILs.
// To prove that nothing changed WITHOUT a quality sweep (AM, stations without HD), build 38's
// GainKeeper.kt can be compiled in as "GainKeeper38" (see gainkeepertest_38.sh) - then test 7
// feeds both the same 300,000 random seconds and compares every answer.
// Build 41: tests 8-10 are the FM-only rules (a real clip; the early try down), and with build 40's
// GainKeeper also compiled in ("GainKeeper40") test 12 checks that on FM nothing else changed:
// no clip and no MER below zero = every answer the same as build 40's, sweeps included.
// Build 41, later: test 8 follows rule 1's final path (6 dB only on a converter full two seconds
// running), test 13 is a station just under full scale (99.1 KGGI at 28.0 dB), 14-19 are rule 4
// (no HD sync since the tune -> walk down), and 20 checks over random seconds that rule 4 acts
// only where it may - every other tune answers exactly as build 40. 21 is rule 1's patience on FM with
// a peak that is loud (-1.5 ... -0.5 dBFS) but not clipping; the random models of 11, 12 and 20 keep
// out of that band (test 21's) - under -1.5 dBFS, and now and then a clip that is not full scale
// (-0.5 ... -0.1: one step, as always) - and their answer-for-answer comparisons then still hold.
// Build 42: 22-25 are the AM rules (tick's am: rule 1 only for a real clip, rule 2 never above the tune's
// gain) on made-up XEKAM 950 / KBRT 740 signals; 26 checks over random seconds that without am every
// answer is build 41's (GainKeeper41 compiled in, see gainkeepertest_38.sh), and 27 the AM rules' two
// promises over random seconds.
package io.github.derek20la.hidefradio

import java.util.Random

val STEPS = intArrayOf(0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496)
var fails = 0
fun check(what: String, ok: Boolean) { println((if (ok) "PASS  " else "FAIL  ") + what); if (!ok) fails++ }

/**
 * The made-up station. [mer] gives the better sideband's MER at a gain, or null = no HD sync there.
 * [quiet] (build 41) = the analog FM's quieting at a gain (Signal.fmQuietDb; -99 = not measured,
 * as in Digital only).
 */
class Station(var gain: Int, val peakAt144: Float, val mer: (Int) -> Float?, val quiet: (Int) -> Float = { -99f },
              val peakOf: ((Int) -> Float)? = null) {
    val keeper = GainKeeper(STEPS)
    var now = 0L
    var merCount = 0
    var maxGain = gain
    var changes = ArrayList<String>()
    private var sinceMer = 0.0
    fun peak() = peakOf?.invoke(gain) ?: minOf(0f, peakAt144 + (gain - 144) / 10f)
    var clipSeconds = 0                                   // build 41: seconds with a real clip (peak >= -0.5)
    var walkSteps = 0                                     // build 41: changes made by rule 4's walk
    /**
     * Runs [seconds] ticks. [sweep] = what RadioService passes as Signal.gainSwept, [fm] = !Signal.am,
     * [hdDecoding] = not Analog only (build 41).
     */
    fun run(seconds: Int, sweep: Boolean, hdThere: Boolean = true, fm: Boolean = true, hdDecoding: Boolean = true) {
        repeat(seconds) {
            now += 1000
            val m = if (hdThere) mer(gain) else null
            if (m != null) { sinceMer += 1.0; if (sinceMer >= 1.5) { sinceMer -= 1.5; merCount++ } }
            if (peak() >= GainKeeper.CLIP_DBFS) clipSeconds++
            val to = keeper.tick(now, gain, peak(), m != null, merCount, m ?: 0f, (m ?: 0f) - 3f, sweep, fm, hdDecoding, quiet(gain))
            if (to != GainKeeper.NO_CHANGE) {
                if (keeper.walking) walkSteps++
                changes.add("${now / 1000}s ${gain / 10.0}->${to / 10.0}${if (keeper.walking) " (walk)" else if (keeper.tryingTo >= 0) " (try)" else ""}")
                gain = to
                maxGain = maxOf(maxGain, gain)
            }
        }
    }
}

// KOST-like: HD best at 14.4 dB, a little worse at 12.5, and GONE from 15.7 dB up (the tuner overloads).
val kost: (Int) -> Float? = { g -> when { g >= 157 -> null; g == 144 -> 10.6f; g == 125 -> 10.0f; else -> 8.5f } }

fun main() {
    println("1. KOST after a quality sweep chose 14.4 dB (peak -9 dBFS, 'quiet'): 10 minutes")
    var s = Station(144, -9f, kost); s.keeper.restart(0); s.run(600, sweep = true)
    println("   " + s.changes.joinToString(", "))
    check("ends on 14.4 dB", s.gain == 144)
    check("rule 2 never climbed: no change that stayed (ups ${s.keeper.ups}, downs ${s.keeper.downs})", s.keeper.ups == 0 && s.keeper.downs == 0)
    check("only ONE MER try up in 10 minutes (it lost the HD, so 15.7 dB became a ceiling)", s.changes.count { it.contains("->15.7") } == 1)
    check("the gain is 'trusted' (may be remembered)", s.keeper.gainTrusted)

    println("2. The same station WITHOUT a sweep (what build 38 did): rule 2 climbs on the peak alone")
    s = Station(144, -9f, kost); s.keeper.restart(0); s.run(600, sweep = false)
    println("   " + s.changes.joinToString(", "))
    check("it climbs to 15.7 dB and beyond and the HD is gone (ends on ${s.gain / 10.0} dB)", s.maxGain > 157 && kost(s.gain) == null)
    check("... and that gain is NOT trusted (not remembered)", !s.keeper.gainTrusted)

    println("3. Overload guard after a sweep: the peak jumps to -1 dBFS")
    val loud = Station(144, -1f, kost); loud.keeper.restart(0); loud.run(3, sweep = true, fm = false)
    println("   AM: " + loud.changes.joinToString(", "))
    check("AM: steps down at once (to ${loud.gain / 10.0} dB)", loud.gain < 144 && loud.changes.isNotEmpty() && loud.changes[0].startsWith("1s"))
    check("a gain the overload guard moved to is not trusted", !loud.keeper.gainTrusted)
    val loudFm = Station(144, -1f, kost); loudFm.keeper.restart(0); loudFm.run(5, sweep = true)
    println("   FM: " + loudFm.changes.joinToString(", "))
    check("FM (build 41): -1 dBFS is loud, not a clip - down on the 3rd such second (${loudFm.changes.firstOrNull()})", loudFm.changes.isNotEmpty() && loudFm.changes[0].startsWith("3s") && !loudFm.keeper.gainTrusted)

    println("4. A sweep chose 14.4 dB; after 20 s the HD goes away for good (drove off): rule 2 gets its freedom back ${GainKeeper.SWEEP_LIFT_MS / 1000} s later")
    s = Station(144, -20f, { 8f }); s.keeper.restart(0); s.run(20, sweep = true)
    s.run(55, sweep = true, hdThere = false)
    val at55 = s.gain
    s.run(120, sweep = true, hdThere = false)
    println("   " + s.changes.joinToString(", "))
    check("no climb in the first 55 s without HD (still ${at55 / 10.0} dB)", at55 == 144)
    check("climbs after that (now ${s.gain / 10.0} dB)", s.gain > 144)
    check("... not trusted", !s.keeper.gainTrusted)

    println("5. A station that is better one step up (MER 8 -> 9 from 15.7 dB): the MER try is kept and becomes the new limit")
    s = Station(144, -12f, { g -> if (g >= 157) 9f else 8f }); s.keeper.restart(0); s.run(50, sweep = true)
    println("   " + s.changes.joinToString(", "))
    check("on 15.7 dB after 50 s (kept ${s.keeper.kept} of ${s.keeper.tries} tries)", s.gain == 157 && s.keeper.kept == 1)
    check("trusted", s.keeper.gainTrusted)

    println("6. The overload guard stepped down from the sweep's gain; rule 2 may come BACK up to it, not beyond")
    s = Station(144, -9f, { 10f }); s.keeper.restart(0); s.run(5, sweep = true)
    val strong = s.keeper.tick(s.now + 1000, 144, -1.0f, true, s.merCount, 10f, 7f, true)      // one loud second
    s.now += 1000; s.gain = strong
    s.run(200, sweep = true)                                                                 // quiet again
    println("   stepped down to ${strong / 10.0} dB; then " + s.changes.joinToString(", "))
    check("back on 14.4 dB at the most (now ${s.gain / 10.0} dB)", strong == 125 && s.gain <= 157 && s.changes.any { it.contains("12.5->14.4") })

    println("8. Build 41, FM: a REAL clip (0 dBFS) at 40.2 dB, no HD (94.7 KTWV on 11-20) - one step, then 6 dB at a time while still full")
    s = Station(402, -10.2f, { null }); s.keeper.restart(0); s.run(60, sweep = false)
    println("   FM: " + s.changes.joinToString(", ") + "  (${s.clipSeconds} s clipping)")
    val am8 = Station(402, -10.2f, { null }); am8.keeper.restart(0); am8.run(60, sweep = false, fm = false)
    println("   AM (as build 40): " + am8.changes.joinToString(", ") + "  (${am8.clipSeconds} s clipping)")
    check("FM: out of the clip in 3 moves (40.2 -> 38.6: the first full second; 38.6 still full -> 29.7; still full -> 22.9), ${s.clipSeconds} s clipping",
          s.changes.size == 3 && s.changes[0].endsWith("40.2->38.6") && s.changes[1].endsWith("38.6->29.7") && s.changes[2].endsWith("29.7->22.9") && s.clipSeconds <= 5)
    check("FM: ends on the highest step under the overload limit, where AM's one step at a time ends too (${s.gain / 10.0} / ${am8.gain / 10.0} dB, peak ${s.peak()})",
          s.gain == 229 && am8.gain == 229 && s.peak() < GainKeeper.OVERLOAD_DBFS)
    check("AM: one step at a time, as before (${am8.changes.size} moves, ${am8.clipSeconds} s clipping)", am8.changes.size >= 8 && am8.changes.all { !it.contains("(try)") } && am8.clipSeconds > s.clipSeconds)

    println("9. Build 41, FM: synced, MER below zero, quiet peak (95.5 KLOS on 11-20: overload the peak can't see) - early tries down")
    // MER best near 20 dB; above it the (unseen) overload costs 1 dB of MER per 2 dB of gain, below it the noise 1 per 4.
    val klos: (Int) -> Float? = { g -> if (g >= 200) 6f - (g - 200) / 20f else 6f - (200 - g) / 40f }
    s = Station(402, -37.8f, klos); s.keeper.restart(0); s.run(60, sweep = false)
    val at60 = s.gain
    println("   first 60 s: " + s.changes.joinToString(", "))
    s.run(240, sweep = false)
    check("an early try before the usual first one (${GainKeeper.FIRST_TRY_MS / 1000} s): ${s.changes.firstOrNull()}",
          s.changes.isNotEmpty() && s.changes[0].endsWith("40.2->37.2 (try)") && s.changes[0].substringBefore("s").toInt() < 20)
    check("MER above zero within 60 s (on ${at60 / 10.0} dB, MER ${klos(at60)})", (klos(at60) ?: -9f) >= 0f)
    check("never above 40.2 dB, and after 5 minutes still MER >= 0 (on ${s.gain / 10.0} dB, MER ${klos(s.gain)})", s.maxGain == 402 && (klos(s.gain) ?: -9f) >= 0f)
    check("trusted (a tested gain)", s.keeper.gainTrusted)
    val am9 = Station(402, -37.8f, klos); am9.keeper.restart(0); am9.run(60, sweep = false, fm = false)
    println("   the same as AM / build 40, first 60 s: " + am9.changes.joinToString(", "))
    check("(build 40 climbed on the quiet peak instead: max ${am9.maxGain / 10.0} dB)", am9.maxGain > 402)

    println("10. Build 41, FM: a WEAK station, MER -1 and worse with less gain - one early try, then the usual tries")
    s = Station(402, -30.8f, { g -> -1f + (g - 402) / 30f }); s.keeper.restart(0); s.run(120, sweep = false)
    println("   " + s.changes.joinToString(", "))
    check("exactly one try down to 37.2 dB, and it went back", s.changes.count { it.contains("->37.2") } == 1 && s.changes.any { it.contains("37.2->40.2") })
    check("the usual tries then go UP and keep what is better (ends on ${s.gain / 10.0} dB)", s.gain > 402)

    println("13. Build 41, FM: just UNDER full scale (99.1 KGGI at 28.0 dB: -0.0 ... -0.6 dBFS, a full 0.0 one second in 5) - one step, not 6 dB")
    run {
        val k = GainKeeper(STEPS); k.restart(0)
        var g = 280; var mc = 0; val moves = ArrayList<String>()
        for (t in 1..120) {
            // peak: the recording's pattern at 28.0, 2.6 dB lower at 25.4 (KGGI 9 PM: -0.2 / -2.9 median)
            val p = when (g) { 280 -> if (t % 5 == 0) 0f else -0.3f - (t % 3) * 0.1f; else -> -0.2f - (280 - g) / 10f }
            if (t % 3 != 0) mc++
            val to = k.tick(t * 1000L, g, p, true, mc, 4.6f, 6.2f, true, true, true, -33f)
            if (to != GainKeeper.NO_CHANGE) { moves.add("${t}s ${g / 10.0}->${to / 10.0}${if (k.tryingTo >= 0) " (try)" else ""}"); g = to }
        }
        println("   " + moves.joinToString(", "))
        check("the first move is ONE step, 28.0 -> 25.4 (build 41 at first went 6 dB, to 22.9)", moves.isNotEmpty() && moves[0].endsWith("28.0->25.4"))
        check("no 6 dB drop at all, and it stays near the top (now ${g / 10.0} dB)", moves.none { it.endsWith("->22.9") && !it.contains("(try)") } && g >= 229)
    }
    run {
        // ... while a gain that is full EVERY second (KGGI at 29.7) still gets the 6 dB from the next gain on
        val k = GainKeeper(STEPS); k.restart(0)
        var g = 297; val moves = ArrayList<String>()
        for (t in 1..20) {
            val p = if (g >= 280) 0f else -4.5f - (229 - g) / 10f
            val to = k.tick(t * 1000L, g, p, true, t, 4f, 5f, true, true, true, -33f)
            if (to != GainKeeper.NO_CHANGE) { moves.add("${t}s ${g / 10.0}->${to / 10.0}"); g = to }
        }
        println("   full every second: " + moves.joinToString(", "))
        check("a real clip: one step (29.7 -> 28.0), then still full -> 6 dB (-> 20.7)", moves.size >= 2 && moves[0].endsWith("29.7->28.0") && moves[1].endsWith("28.0->20.7"))
    }

    // Rule 4 (build 41): an overload the peak can't see. LA KOST-like: HD in sync only at 20.7 dB
    // and below; above it no sync - but the burst looks like HD, so the search took 40.2. The peak
    // stays calm and hardly moves with the gain up there (-12 dBFS from 28.0 up: the overloaded tuner
    // compresses, as on 11-20), then falls dB for dB. The analog's quieting shows the overload:
    // -15 dB above 14.4, -40 below.
    val blind: (Int) -> Float? = { g -> if (g <= 207) 7f + (207 - g) / 40f else null }
    val blindQuiet: (Int) -> Float = { g -> if (g > 144) -15f else -40f }
    val blindPeak: (Int) -> Float = { g -> if (g >= 280) -12f else -12f - (280 - g) / 10f }
    println("14. Build 41, rule 4: the search took 40.2 dB for its HD, no sync there (overload the peak can't see) - walk down")
    s = Station(402, 0f, blind, blindQuiet, blindPeak); s.keeper.restart(0); s.run(29, sweep = true)
    val at30 = s.gain
    println("   first 29 s: " + s.changes.joinToString(", "))
    s.run(571, sweep = true)
    println("   then: " + s.changes.drop(3).joinToString(", "))
    check("walks 40.2 -> 33.8 -> 25.4 -> 16.6 (6 dB a step, from 8 s, 6 s a step) and syncs there (${at30 / 10.0} dB at 29 s)",
          s.changes.size >= 3 && s.changes[0] == "9s 40.2->33.8 (walk)" && s.changes[1] == "15s 33.8->25.4 (walk)" && s.changes[2] == "21s 25.4->16.6 (walk)" && at30 == 166)
    check("the found gain counts as a change that stayed (↓${s.keeper.downs}) and is trusted (may be remembered)", s.keeper.downs >= 1 && s.keeper.gainTrusted)
    check("10 minutes later still in sync, never back above 20.7 (now ${s.gain / 10.0} dB, max after the walk ${s.changes.drop(3).maxOfOrNull { it.substringAfter("->").substringBefore(" ").toDouble() } ?: 16.6})",
          blind(s.gain) != null && s.changes.drop(3).none { it.substringAfter("->").substringBefore(" ").toDouble() > 20.7 })
    val off14 = Station(402, 0f, blind, blindQuiet, blindPeak); off14.keeper.restart(0); off14.run(600, sweep = true, hdDecoding = false)
    check("Analog only (HD decoder off): no walk at all (${off14.changes.size} changes in 10 min; the analog is left as build 41 had it)",
          off14.walkSteps == 0 && off14.changes.none { it.contains("(walk)") })

    println("15. Build 41, rule 4: no sync at ANY gain (HD too weak to decode) - the walk ends by 26 s, back where it started")
    s = Station(402, 0f, { null }, { -30f }, blindPeak); s.keeper.restart(0); s.run(40, sweep = true)
    println("   " + s.changes.joinToString(", "))
    check("3 steps, then at 27 s back to 40.2 dB", s.changes.size == 4 && s.changes[3] == "27s 16.6->40.2" && s.gain == 402)
    val counted15 = s.keeper.downs + s.keeper.ups
    s.run(560, sweep = true)
    check("once per tune: no second walk in 10 minutes (${s.changes.count { it.contains("(walk)") }} walk steps in all)", s.changes.count { it.contains("(walk)") } == 3)
    check("... and a walk that found nothing is not counted as a change on the screen ($counted15 in the first 40 s)", counted15 == 0)

    println("16. Build 41, rule 4: the analog gets WORSE a step down (no overload: a station whose HD just won't sync) - back at once")
    // quieting 6 dB worse per 6 dB less gain (worse than any of the 11-23 sets: 0-3 dB, once 8)
    s = Station(280, -21.6f, { null }, { g -> -30f + (280 - g) / 10f }); s.keeper.restart(0); s.run(60, sweep = true)
    println("   " + s.changes.joinToString(", "))
    check("one step (28.0 -> 20.7), judged on 3 quieting readings after the settle second, back 4 s later: ${s.changes}",
          s.changes.size == 2 && s.changes[0] == "9s 28.0->20.7 (walk)" && s.changes[1] == "13s 20.7->28.0" && s.gain == 280)
    s = Station(280, -21.6f, { null }, { g -> -30f + (280 - g) / 60f }); s.keeper.restart(0); s.run(40, sweep = true)
    check("... while 1 dB worse is NOT reason to stop (${s.changes.count { it.contains("(walk)") }} steps)", s.changes.count { it.contains("(walk)") } >= 2)

    println("17. Build 41, rule 4 in Digital only (no analog: no quieting): the same station walks on, nothing to lose")
    s = Station(280, -21.6f, { null }); s.keeper.restart(0); s.run(40, sweep = true)
    println("   " + s.changes.joinToString(", "))
    check("steps down until the peak is quiet (20.7: -15.3 dBFS, one more; 14.4: -21.6, no more), then back (${s.changes})",
          s.changes == listOf("9s 28.0->20.7 (walk)", "15s 20.7->14.4 (walk)", "21s 14.4->28.0") && s.gain == 280)

    println("18. Build 41, rule 4: NOT for HD that synced and then faded (weak signal, not an overload) - and not without a sweep")
    s = Station(402, 0f, { null }, blindQuiet, blindPeak); s.keeper.restart(0)
    run { val st = s; val k = st.keeper; var mc = 0
        for (t in 1..5) { st.now += 1000; mc++; k.tick(st.now, st.gain, st.peak(), true, mc, 7f, 4f, true, true, true, -40f) } }   // in sync for 5 s
    s.run(120, sweep = true)
    check("synced at first, then 2 minutes without sync: no walk (${s.changes.size} changes: ${s.changes.take(3)})", s.changes.none { it.contains("(walk)") })
    s = Station(402, 0f, blind, blindQuiet, blindPeak); s.keeper.restart(0); s.run(60, sweep = false)
    check("no sweep (the search found no HD): no walk", s.changes.none { it.contains("(walk)") })

    println("19. Build 41, rule 4 + rule 1: a clipping second during the walk - rule 1 comes first, as always, and the walk is over")
    s = Station(402, 0f, blind, blindQuiet, blindPeak); s.keeper.restart(0); s.run(11, sweep = true)
    run { val st = s; st.now += 1000
        val to = st.keeper.tick(st.now, st.gain, 0.0f, false, st.merCount, 0f, 0f, true, true, true, -15f)
        st.changes.add("${st.now / 1000}s ${st.gain / 10.0}->${to / 10.0} (loud)"); if (to != GainKeeper.NO_CHANGE) st.gain = to }
    s.run(60, sweep = true)
    println("   " + s.changes.joinToString(", "))
    check("rule 1's one step at the clipping second (33.8 -> 32.8), and no walk step after it",
          s.changes.size >= 2 && s.changes[1] == "12s 33.8->32.8 (loud)" && s.changes.drop(2).none { it.contains("(walk)") })

    println("7. No sweep (AM, FM without HD): every answer the same as build 38's GainKeeper")
    val old = try { Class.forName("io.github.derek20la.hidefradio.GainKeeper38") } catch (e: ClassNotFoundException) { null }
    if (old == null) println("   (skipped: GainKeeper38 is not compiled in - see gainkeepertest_38.sh)")
    else {
        val k38 = old.getConstructor(IntArray::class.java).newInstance(STEPS)
        val tick38 = old.getMethod("tick", Long::class.java, Int::class.java, Float::class.java, Boolean::class.java, Int::class.java, Float::class.java, Float::class.java)
        val restart38 = old.getMethod("restart", Long::class.java)
        val k39 = GainKeeper(STEPS)
        val rnd = Random(39)
        var now = 0L; var gain = 207; var merCount = 0; var differ = 0; var moves = 0
        var base = -8f; var locked = true
        k39.restart(0); restart38.invoke(k38, 0L)
        repeat(300_000) { n ->
            now += 1000
            if (rnd.nextInt(400) == 0) { base = -30f + rnd.nextFloat() * 32f }           // the signal changes
            if (rnd.nextInt(120) == 0) locked = !locked
            if (rnd.nextInt(5000) == 0) { k39.restart(now); restart38.invoke(k38, now); gain = STEPS[rnd.nextInt(STEPS.size)] }   // a retune
            if (locked && n % 3 != 0) merCount++
            val peak = if (rnd.nextInt(200) == 0) -99f else minOf(0f, base + (gain - 207) / 10f + rnd.nextFloat() * 2f)
            val lo = 4f + (rnd.nextFloat() * 6f) - Math.abs(gain - 207) / 40f; val up = lo - 2f + rnd.nextFloat() * 4f
            val a = k39.tick(now, gain, peak, locked, merCount, lo, up)
            val b = tick38.invoke(k38, now, gain, peak, locked, merCount, lo, up) as Int
            if (a != b) differ++
            if (a != GainKeeper.NO_CHANGE) { gain = a; moves++ }
        }
        check("300,000 random seconds, $moves gain changes: $differ answers differ", differ == 0 && moves > 1000)
    }
    println("11. Build 41, FM without HD (never in sync), peak under -1.5 dBFS or clipping (never in between): every answer the same as build 38's")
    val old38 = try { Class.forName("io.github.derek20la.hidefradio.GainKeeper38") } catch (e: ClassNotFoundException) { null }
    if (old38 == null) println("   (skipped: GainKeeper38 is not compiled in - see gainkeepertest_38.sh)")
    else {
        val k38 = old38.getConstructor(IntArray::class.java).newInstance(STEPS)
        val tick38 = old38.getMethod("tick", Long::class.java, Int::class.java, Float::class.java, Boolean::class.java, Int::class.java, Float::class.java, Float::class.java)
        val restart38 = old38.getMethod("restart", Long::class.java)
        val k41 = GainKeeper(STEPS)
        val rnd = Random(41)
        var now = 0L; var gain = 207; var differ = 0; var moves = 0; var base = -8f
        k41.restart(0); restart38.invoke(k38, 0L)
        repeat(300_000) {
            now += 1000
            if (rnd.nextInt(400) == 0) { base = -30f + rnd.nextFloat() * 32f }
            if (rnd.nextInt(5000) == 0) { k41.restart(now); restart38.invoke(k38, now); gain = STEPS[rnd.nextInt(STEPS.size)] }
            val peak = if (rnd.nextInt(200) == 0) -99f else if (rnd.nextInt(40) == 0) -0.5f + rnd.nextFloat() * 0.4f else minOf(-1.6f, base + (gain - 207) / 10f + rnd.nextFloat() * 2f)
            val a = k41.tick(now, gain, peak, false, 0, 0f, 0f, false, true, true, -30f)   // (no sweep: rule 4 never)
            val b = tick38.invoke(k38, now, gain, peak, false, 0, 0f, 0f) as Int
            if (a != b) differ++
            if (a != GainKeeper.NO_CHANGE) { gain = a; moves++ }
        }
        check("300,000 random seconds, $moves gain changes: $differ answers differ", differ == 0 && moves > 1000)
    }

    println("12. Build 41, FM with HD (sweeps, MER tries), MER never below zero, HD in sync at the tune, peak under -1.5 or clipping: every answer the same as build 40's")
    val old40 = try { Class.forName("io.github.derek20la.hidefradio.GainKeeper40") } catch (e: ClassNotFoundException) { null }
    if (old40 == null) println("   (skipped: GainKeeper40 is not compiled in - see gainkeepertest_38.sh)")
    else {
        val k40 = old40.getConstructor(IntArray::class.java).newInstance(STEPS)
        val tick40 = old40.getMethod("tick", Long::class.java, Int::class.java, Float::class.java, Boolean::class.java, Int::class.java, Float::class.java, Float::class.java, Boolean::class.java)
        val restart40 = old40.getMethod("restart", Long::class.java)
        val k41 = GainKeeper(STEPS)
        val rnd = Random(40)
        var now = 0L; var gain = 207; var merCount = 0; var differ = 0; var moves = 0; var tries = 0
        var base = -8f; var locked = true; var sweep = true
        k41.restart(0); restart40.invoke(k40, 0L)
        repeat(300_000) { n ->
            now += 1000
            if (rnd.nextInt(400) == 0) { base = -30f + rnd.nextFloat() * 32f }
            if (rnd.nextInt(120) == 0) locked = !locked
            // (build 41: the HD is in sync when the tune starts, so rule 4 - "no sync since the tune" - never runs;
            //  test 20 is the same model with tunes that don't sync)
            if (rnd.nextInt(3000) == 0) { k41.restart(now); restart40.invoke(k40, now); gain = STEPS[rnd.nextInt(STEPS.size)]; sweep = rnd.nextBoolean(); locked = true }
            if (locked && n % 3 != 0) merCount++
            val peak = if (rnd.nextInt(200) == 0) -99f else if (rnd.nextInt(40) == 0) -0.5f + rnd.nextFloat() * 0.4f else minOf(-1.6f, base + (gain - 207) / 10f + rnd.nextFloat() * 2f)
            val lo = maxOf(0.1f, 4f + (rnd.nextFloat() * 6f) - Math.abs(gain - 207) / 40f); val up = lo - 2f + rnd.nextFloat() * 4f
            val a = k41.tick(now, gain, peak, locked, merCount, lo, up, sweep, true, true, -30f - rnd.nextFloat() * 10f)
            val b = tick40.invoke(k40, now, gain, peak, locked, merCount, lo, up, sweep) as Int
            if (a != b) differ++
            if (a != GainKeeper.NO_CHANGE) { gain = a; moves++; if (k41.tryingTo >= 0) tries++ }
        }
        check("300,000 random seconds, $moves gain changes ($tries MER tries): $differ answers differ", differ == 0 && moves > 1000 && tries > 100)
    }

    println("20. Build 41, rule 4 over random seconds: the model of 12, but tunes often start out of sync - rule 4 acts only where it may")
    if (old40 == null) println("   (skipped: GainKeeper40 is not compiled in - see gainkeepertest_38.sh)")
    else {
        val k40 = old40.getConstructor(IntArray::class.java).newInstance(STEPS)
        val tick40 = old40.getMethod("tick", Long::class.java, Int::class.java, Float::class.java, Boolean::class.java, Int::class.java, Float::class.java, Float::class.java, Boolean::class.java)
        val restart40 = old40.getMethod("restart", Long::class.java)
        val k41 = GainKeeper(STEPS)
        val rnd = Random(4141)
        var now = 0L; var gain = 207; var merCount = 0; var base = -8f; var locked = false; var sweep = true
        var tuneAt = 0L; var syncedSince = false; var walkedThisTune = false
        var differOutside = 0; var walks = 0; var badWalks = 0; var tunes = 1; var walkTunes = 0
        k41.restart(0); restart40.invoke(k40, 0L)
        repeat(300_000) { n ->
            now += 1000
            if (rnd.nextInt(400) == 0) { base = -30f + rnd.nextFloat() * 32f }
            if (rnd.nextInt(60) == 0) locked = !locked
            if (rnd.nextInt(600) == 0) {
                k41.restart(now); restart40.invoke(k40, now); gain = STEPS[rnd.nextInt(STEPS.size)]; sweep = rnd.nextBoolean()
                locked = rnd.nextBoolean(); tuneAt = now; syncedSince = false; walkedThisTune = false; tunes++
            }
            if (locked) syncedSince = true
            if (locked && n % 3 != 0) merCount++
            val peak = if (rnd.nextInt(200) == 0) -99f else if (rnd.nextInt(40) == 0) -0.5f + rnd.nextFloat() * 0.4f else minOf(-1.6f, base + (gain - 207) / 10f + rnd.nextFloat() * 2f)
            val lo = maxOf(0.1f, 4f + (rnd.nextFloat() * 6f) - Math.abs(gain - 207) / 40f); val up = lo - 2f + rnd.nextFloat() * 4f
            val a = k41.tick(now, gain, peak, locked, merCount, lo, up, sweep, true, true, -30f - rnd.nextFloat() * 4f)
            val b = tick40.invoke(k40, now, gain, peak, locked, merCount, lo, up, sweep) as Int
            if (a != GainKeeper.NO_CHANGE && k41.walking) {
                walks++
                if (!walkedThisTune) walkTunes++
                walkedThisTune = true
                // a walk step may only come: after a sweep, before any sync since the tune, 8-26 s into it
                val t = now - tuneAt
                if (!sweep || syncedSince || t < GainKeeper.NOSYNC_MS || t >= GainKeeper.SYNC_WALK_END_MS + 1000) badWalks++
            }
            if (a != b && !walkedThisTune) differOutside++          // a tune the walk never touched: build 40, answer for answer
            if (a != GainKeeper.NO_CHANGE) gain = a
        }
        check("300,000 random seconds, $tunes tunes, $walks walk steps in $walkTunes of them: $badWalks out of place, " +
              "$differOutside answers differ from build 40 in tunes without a walk", walks > 50 && badWalks == 0 && differOutside == 0)
    }
    println("21. Build 41, FM: LOUD but not clipping (-1.5 ... -0.5 dBFS) - one step down only if it keeps happening")
    // [pattern] = the peak each second at the starting gain; one step lower is 2.5 dB quieter
    fun loud(pattern: (Int) -> Float, fm: Boolean, start: Int = 197): List<String> {
        val k = GainKeeper(STEPS); k.restart(0)
        var g = start; var mc = 0; val moves = ArrayList<String>()
        for (t in 1..120) {
            val p = if (g == start) pattern(t) else pattern(t) - 2.5f
            if (t % 3 != 0) mc++
            val to = k.tick(t * 1000L, g, p, true, mc, 5.6f, 4.2f, true, fm, true, -36f)
            if (to != GainKeeper.NO_CHANGE) { moves.add("${t}s ${g / 10.0}->${to / 10.0}${if (k.tryingTo >= 0) " (try)" else ""}"); g = to }
        }
        return moves
    }
    // 99.1 KGGI at 7 PM, 19.7 dB: -2.6 dBFS, and -0.9 one second in 29
    val kggi7 = { t: Int -> if (t % 29 == 11) -0.9f else -2.6f - (t % 3) * 0.1f }
    var m = loud(kggi7, fm = true)
    println("   FM: " + m.joinToString(", "))
    check("one loud second now and then: it stays on 19.7 dB (rule 1 never moves it; MER tries may look and come back)", m.none { it.matches(Regex("\\d+s 19\\.7->16\\.6")) })
    m = loud(kggi7, fm = false)
    check("AM, the same seconds: one step down at the loud second, as before (${m.firstOrNull()})", m.isNotEmpty() && m[0] == "11s 19.7->16.6")
    m = loud({ t -> if (t >= 20 && t % 5 < 3) -1.0f else -2.6f }, fm = true)
    println("   loud 3 seconds in 5 from 20 s: " + m.joinToString(", "))
    check("loud 3 seconds in 5: down one step on the 3rd (${m.firstOrNull()})", m.isNotEmpty() && m[0] == "22s 19.7->16.6")
    m = loud({ t -> if (t == 15) 0f else -2.6f }, fm = true)
    check("a REAL clip (0 dBFS) still acts at once (${m.firstOrNull()})", m.isNotEmpty() && m[0] == "15s 19.7->16.6")
    m = loud({ -1.3f }, fm = true, start = 144)
    check("loud every second (KOST at 14.4 in LA, -1.3): down on the 3rd second (${m.firstOrNull()})", m.isNotEmpty() && m[0] == "3s 14.4->12.5")


    // ---------------- Build 42: AM (tick's am) ----------------
    // [peakOf] = the peak at a gain (each second, before [extra]); [locked] = HD in sync each second.
    fun amRun(start: Int, seconds: Int, am: Boolean, sweepChose: Boolean = true, locked: (Int) -> Boolean = { true },
              peakOf: (Int, Int) -> Float): Pair<Int, List<String>> {
        val k = GainKeeper(STEPS); k.restart(0)
        var g = start; val moves = ArrayList<String>()
        for (t in 1..seconds) {
            val to = k.tick(t * 1000L, g, peakOf(g, t), locked(t), 0, 0f, 0f, sweepChose, false, true, -99f, am)
            if (to != GainKeeper.NO_CHANGE) { moves.add("${t}s ${g / 10.0}->${to / 10.0}"); g = to }
        }
        return g to moves
    }
    // XEKAM 950 on a V4: the peak hardly follows the gain past ~30 dB (the chain compresses) - about -8 dBFS everywhere up there.
    val xekam = { g: Int, _: Int -> if (g >= 297) -8f else -8f - (297 - g) / 10f }
    println("22. Build 42, AM: the tune's search chose 33.8 dB (XEKAM 950: peak ~ -8 dBFS at every gain from 30 dB up) - 10 minutes")
    var (endG, mv) = amRun(338, 600, am = true, peakOf = xekam)
    println("   build 42: " + (if (mv.isEmpty()) "no change" else mv.joinToString(", ")))
    check("rule 2 never climbs above the tune's gain on AM: stays on 33.8 dB (ends ${endG / 10.0})", endG == 338 && mv.isEmpty())
    // (build 41's rule 2 on AM: one step up, then - while in sync - it waits for a MER try that never comes on AM;
    //  out of sync it climbs on. So the contrast is shown out of sync: Digital only before the first sync, a fade.)
    var (endOld, mvOld) = amRun(338, 600, am = false, sweepChose = false, locked = { false }, peakOf = xekam)
    println("   as build 41 (AM, no sweep, out of sync): ends on ${endOld / 10.0} dB after ${mvOld.size} steps up")
    check("... where build 41's AM rules climbed to the top step", endOld == 496)
    val (endNoSync, mvNoSync) = amRun(338, 600, am = true, locked = { false }, peakOf = xekam)
    check("no HD sync for 10 minutes (an analog-only station, or a fade): still never above 33.8 - no lift on AM (ends ${endNoSync / 10.0}, ${mvNoSync.size} changes)",
          endNoSync == 338 && mvNoSync.isEmpty())

    // KBRT 740 on a V4: the dongle's own oscillator leaks in at -740 kHz, -1 ... -2.5 dBFS at 28-38 dB.
    val kbrt = { g: Int, t: Int -> if (g >= 280) -1.2f - (t % 5) * 0.3f else -1.2f - (280 - g) / 10f }
    println("23. Build 42, AM: KBRT 740 at 33.8 dB with the V4's leak at -1.2 ... -2.4 dBFS (not a clip) - 2 minutes")
    val (end23, mv23) = amRun(338, 120, am = true, peakOf = kbrt)
    println("   build 42: " + (if (mv23.isEmpty()) "no change" else mv23.joinToString(", ")))
    check("rule 1 leaves it alone: stays on 33.8 dB (ends ${end23 / 10.0})", end23 == 338 && mv23.isEmpty())
    val (end23o, mv23o) = amRun(338, 120, am = false, sweepChose = false, peakOf = kbrt)
    println("   as build 41: " + mv23o.joinToString(", ") + "  (ends ${end23o / 10.0})")
    check("... where build 41's AM rule 1 stepped down at the first second over -1.5 (${mv23o.firstOrNull()})", mv23o.isNotEmpty() && mv23o[0] == "5s 33.8->32.8")

    println("24. Build 42, AM: a REAL clip - one step at once; a converter full two seconds running - 6 dB, as on FM")
    // -0.3 dBFS at 33.8 for 20 s (a strong station close by), 3 dB quieter per step down
    val (end24a, mv24a) = amRun(338, 20, am = true) { g, t -> if (t in 5..20) minOf(0f, -0.3f - (338 - g) / 10f) else -3f - (338 - g) / 10f }
    println("   -0.3 dBFS from 5 s: " + mv24a.joinToString(", "))
    check("a clip that is not full scale: one step down at once (${mv24a.firstOrNull()}), and on down while it clips",
          mv24a.isNotEmpty() && mv24a[0] == "5s 33.8->32.8" && end24a < 338)
    val (_, mv24b) = amRun(338, 20, am = true) { g, t -> if (t >= 5) minOf(0f, 3f - (338 - g) / 10f) else -3f }
    println("   full scale (0.0) from 5 s: " + mv24b.joinToString(", "))
    check("full scale: one step on the first full second, 6 dB once still full the next (${mv24b.take(2).joinToString(", ")})",
          mv24b.size >= 2 && mv24b[0] == "5s 33.8->32.8" && mv24b[1] == "7s 32.8->25.4")

    println("25. Build 42, AM: after rule 1 stepped down, rule 2 comes back up to the tune's gain when it is quiet again - never beyond")
    // loud (-0.2 dBFS, a clip) for 10 s at 33.8, then quiet (-12 dBFS, the peak still following the gain below 33.8)
    val (end25, mv25) = amRun(338, 600, am = true) { g, t -> if (t <= 10) minOf(0f, -0.2f - (338 - g) / 10f) else -12f - (338 - g) / 10f }
    println("   " + mv25.joinToString(", ") + "  (ends ${end25 / 10.0})")
    check("down on the clip, back up to 33.8 when quiet, not above it (ends ${end25 / 10.0}, top ${mv25.maxOfOrNull { it.substringAfter("->").toDouble() }})",
          end25 == 338 && mv25.none { it.substringAfter("->").toDouble() > 33.8 })

    println("26. Build 42: tick without am (= false) - every answer the same as build 41's GainKeeper (FM and AM, sweeps, MER tries, rule 4, any peak)")
    val old41 = try { Class.forName("io.github.derek20la.hidefradio.GainKeeper41") } catch (e: ClassNotFoundException) { null }
    if (old41 == null) println("   (skipped: GainKeeper41 is not compiled in - see gainkeepertest_38.sh)")
    else {
        val k41 = old41.getConstructor(IntArray::class.java).newInstance(STEPS)
        val tick41 = old41.getMethod("tick", Long::class.java, Int::class.java, Float::class.java, Boolean::class.java, Int::class.java,
                                     Float::class.java, Float::class.java, Boolean::class.java, Boolean::class.java, Boolean::class.java, Float::class.java)
        val restart41 = old41.getMethod("restart", Long::class.java)
        val k42 = GainKeeper(STEPS)
        val rnd = Random(42)
        var now = 0L; var gain = 207; var merCount = 0; var differ = 0; var moves = 0; var tries = 0
        var base = -8f; var locked = true; var sweep = true; var fm = true; var hdDec = true
        k42.restart(0); restart41.invoke(k41, 0L)
        repeat(300_000) { n ->
            now += 1000
            if (rnd.nextInt(400) == 0) { base = -30f + rnd.nextFloat() * 32f }
            if (rnd.nextInt(120) == 0) locked = !locked
            if (rnd.nextInt(3000) == 0) {
                k42.restart(now); restart41.invoke(k41, now); gain = STEPS[rnd.nextInt(STEPS.size)]
                sweep = rnd.nextBoolean(); fm = rnd.nextInt(4) != 0; hdDec = rnd.nextInt(5) != 0; locked = rnd.nextBoolean()
            }
            if (locked && n % 3 != 0) merCount++
            val peak = if (rnd.nextInt(200) == 0) -99f else minOf(0f, base + (gain - 207) / 10f + rnd.nextFloat() * 3f)
            val lo = 4f + (rnd.nextFloat() * 6f) - Math.abs(gain - 207) / 40f - (if (rnd.nextInt(6) == 0) 6f else 0f); val up = lo - 2f + rnd.nextFloat() * 4f
            val quiet = if (hdDec && rnd.nextBoolean()) -99f else -30f - rnd.nextFloat() * 10f
            val a = k42.tick(now, gain, peak, locked, merCount, lo, up, sweep, fm, hdDec, quiet)
            val b = tick41.invoke(k41, now, gain, peak, locked, merCount, lo, up, sweep, fm, hdDec, quiet) as Int
            if (a != b) differ++
            if (a != GainKeeper.NO_CHANGE) { gain = a; moves++; if (k42.tryingTo >= 0) tries++ }
        }
        check("300,000 random seconds, $moves gain changes ($tries tries): $differ answers differ", differ == 0 && moves > 1000 && tries > 100)
    }

    println("27. Build 42, AM over random seconds (am = true, a tune's gain from the search each time): never above that gain, rule 1 only for a clip")
    run {
        val k = GainKeeper(STEPS)
        val rnd = Random(4242)
        var now = 0L; var gain = 338; var tuned = 338; var above = 0; var falseOverload = 0; var moves = 0; var base = -8f
        k.restart(0)
        repeat(300_000) {
            now += 1000
            if (rnd.nextInt(400) == 0) { base = -30f + rnd.nextFloat() * 31f }
            if (rnd.nextInt(3000) == 0) { k.restart(now); gain = STEPS[10 + rnd.nextInt(STEPS.size - 10)]; tuned = gain }
            val peak = if (rnd.nextInt(200) == 0) -99f else minOf(0f, base + (minOf(gain, 297) - 297) / 10f + rnd.nextFloat() * 2f)
            val a = k.tick(now, gain, peak, rnd.nextInt(3) != 0, 0, 0f, 0f, true, false, true, -99f, true)
            if (a != GainKeeper.NO_CHANGE) {
                moves++
                if (a > tuned) above++
                if (a < gain && peak < GainKeeper.CLIP_DBFS && peak > -90f) falseOverload++   // down without a clip this second
                gain = a
            }
        }
        check("300,000 random seconds, $moves gain changes: $above above the tune's gain, $falseOverload down without a clip", above == 0 && falseOverload == 0 && moves > 200)
    }

    println(if (fails == 0) "ALL PASS" else "$fails FAILED")
}
