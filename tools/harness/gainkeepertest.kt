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
// GainKeeper.kt can be compiled in as "GainKeeper38" (see gainkeepertest_38.sh) - then test 6
// feeds both the same 300,000 random seconds and compares every answer.
package io.github.derek20la.hidefradio

import java.util.Random

val STEPS = intArrayOf(0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496)
var fails = 0
fun check(what: String, ok: Boolean) { println((if (ok) "PASS  " else "FAIL  ") + what); if (!ok) fails++ }

/** The made-up station. [mer] gives the better sideband's MER at a gain, or null = no HD sync there. */
class Station(var gain: Int, val peakAt144: Float, val mer: (Int) -> Float?) {
    val keeper = GainKeeper(STEPS)
    var now = 0L
    var merCount = 0
    var maxGain = gain
    var changes = ArrayList<String>()
    private var sinceMer = 0.0
    fun peak() = minOf(0f, peakAt144 + (gain - 144) / 10f)
    /** Runs [seconds] ticks. [sweep] = what RadioService passes as Signal.gainSwept. */
    fun run(seconds: Int, sweep: Boolean, hdThere: Boolean = true) {
        repeat(seconds) {
            now += 1000
            val m = if (hdThere) mer(gain) else null
            if (m != null) { sinceMer += 1.0; if (sinceMer >= 1.5) { sinceMer -= 1.5; merCount++ } }
            val to = keeper.tick(now, gain, peak(), m != null, merCount, m ?: 0f, (m ?: 0f) - 3f, sweep)
            if (to != GainKeeper.NO_CHANGE) {
                changes.add("${now / 1000}s ${gain / 10.0}->${to / 10.0}${if (keeper.tryingTo >= 0) " (try)" else ""}")
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
    val loud = Station(144, -1f, kost); loud.keeper.restart(0); loud.run(3, sweep = true)
    println("   " + loud.changes.joinToString(", "))
    check("steps down at once (to ${loud.gain / 10.0} dB)", loud.gain < 144 && loud.changes.isNotEmpty() && loud.changes[0].startsWith("1s"))
    check("a gain the overload guard moved to is not trusted", !loud.keeper.gainTrusted)

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
    println(if (fails == 0) "ALL PASS" else "$fails FAILED")
}
