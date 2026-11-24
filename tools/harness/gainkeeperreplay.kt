// Harness only (build 41): the gain watchdog (GainKeeper) replayed on REAL recordings - the current
// GainKeeper next to build 40's (1.4), and optionally a third one. Run it with gainkeeperreplay.sh
// (see there for making the traces).
//   args: <traces dir> <station> <start gain, tenths> <sweep 0/1> <seconds> [v4]
//   "v4" = the V4's 29 steps; a gain between two recordings then uses the nearest recording's
//   MER, sync and quieting, and a peak shifted up dB for dB from the recording at or below it.
// Each run is 8 replays (starting 0, 3, 6 ... s into the recordings); per build it prints the
// share of seconds in sync, with good HD (BER under 5 %), the average BER, clipping, the analog's
// quieting (what Auto plays while the HD isn't there), the gain changes, and where it ended.
//
// Each second: the GainKeeper gets what RadioService would give it - the second's loudest peak,
// sync, the MER reading count and values, the analog quieting - taken from the recording made at
// the gain the watchdog has set right now (traces/<station>_<gain>.txt, from apptest FMTRACE=1).
// A gain change = the next second comes from another recording. Sync after being out of sync
// takes ACQ seconds (the HD search has to find it again); a gain change while in sync keeps sync
// if the new recording is in sync there. The radio is in Auto (the HD decoder runs, and so does
// the analog demodulator).
package io.github.derek20la.hidefradio

import java.io.File

class Row(val peak: Float, val synced: Boolean, val merCount: Int, val lo: Float, val up: Float, val ber: Float, val quiet: Float)

fun load(dir: String, station: String): Map<Int, List<Row>> {
    val re = Regex("""TRACE \S+ peak (\S+) synced (\d) merCount (\d+) merLower (\S+) merUpper (\S+) ber (\S+) berAvg (\S+)(?: quiet (\S+))?""")
    val out = sortedMapOf<Int, List<Row>>()
    File(dir).listFiles()!!.filter { it.name.startsWith(station + "_") && it.name.removePrefix(station + "_").removeSuffix(".txt").all { c -> c.isDigit() } }.forEach { f ->
        val g = f.name.removePrefix(station + "_").removeSuffix(".txt").toInt()
        out[g] = f.readLines().mapNotNull { re.find(it)?.groupValues }.map {
            Row(it[1].toFloat(), it[2] == "1", it[3].toInt(), it[4].toFloat(), it[5].toFloat(), it[6].toFloat(),
                if (it[8].isEmpty()) -99f else it[8].toFloat())
        }
    }
    return out
}

const val ACQ = 3          // seconds to get back into sync after being out of it

class Result(val gains: List<Int>, val synced: Int, val good: Int, val clip: Int, val changes: Int, val berSum: Double, val berN: Int,
             val quietSum: Double, val quietN: Int, val secs: Int)

val V4 = intArrayOf(0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496)
var v4mode = false
fun nearest(traces: Map<Int, List<Row>>, g: Int) = traces.keys.minByOrNull { Math.abs(it - g) }!!

/** One keeper under test: its name and "one second in -> the answer". */
class Keeper(val name: String, val tick: (Long, Int, Row, Boolean, Int, Boolean) -> Int)

/** The builds to compare: "40" = 1.4 (GainKeeper40), "41a" = an optional third (GainKeeper41a), "now" = the current GainKeeper. */
fun keepers(steps: IntArray): List<Keeper> {
    val out = ArrayList<Keeper>()
    for ((cls, name) in listOf("GainKeeper40" to "1.4  ", "GainKeeper41a" to "1.4.1")) {
        val c = try { Class.forName("io.github.derek20la.hidefradio.$cls") } catch (e: ClassNotFoundException) { continue }
        val k = c.getConstructor(IntArray::class.java).newInstance(steps)
        c.getMethod("restart", Long::class.java).invoke(k, 0L)
        val m = c.methods.first { it.name == "tick" }
        out.add(Keeper(name) { now, g, r, locked, mc, sweep ->
            val args: Array<Any> = when (m.parameterCount) {
                8 -> arrayOf(now, g, r.peak, locked, mc, r.lo, r.up, sweep)                  // build 40
                9 -> arrayOf(now, g, r.peak, locked, mc, r.lo, r.up, sweep, true)            // build 41 as first written (fm)
                else -> arrayOf(now, g, r.peak, locked, mc, r.lo, r.up, sweep, true, true, r.quiet)
            }
            m.invoke(k, *args) as Int
        })
    }
    val k = GainKeeper(steps); k.restart(0)
    out.add(Keeper("final") { now, g, r, locked, mc, sweep -> k.tick(now, g, r.peak, locked, mc, r.lo, r.up, sweep, true, true, r.quiet) })
    return out
}

fun run(which: Int, traces: Map<Int, List<Row>>, start: Int, sweep: Boolean, offset: Int, secs: Int): Pair<String, Result> {
    val steps = if (v4mode) V4 else traces.keys.sorted().toIntArray()
    val keeper = keepers(steps)[which]
    var g = start; var locked = false; var inSyncFor = 0; var merCount = 0; var lastGain = -1
    var synced = 0; var good = 0; var clip = 0; var changes = 0; var berSum = 0.0; var berN = 0; var qSum = 0.0; var qN = 0
    val gains = ArrayList<Int>()
    for (t in 1..secs) {
        val gt = nearest(traces, g); val tr = traces[gt]!!; val pos = (offset + t) % tr.size; val r0 = tr[pos]
        // the peak in between: from the recording at or below this gain, shifted UP dB for dB (a clipped
        // 0.0 can't be shifted down honestly - how far over it was is unknown); below the lowest: shifted down
        val gb = traces.keys.filter { it <= g }.maxOrNull() ?: traces.keys.min()
        val rb = traces[gb]!![(offset + t) % traces[gb]!!.size]
        val r = if (gt == g) r0 else Row(minOf(0f, rb.peak + (g - gb) / 10f), r0.synced, r0.merCount, r0.lo, r0.up, r0.ber, r0.quiet)
        if (r.synced) { inSyncFor++; if (!locked && inSyncFor >= ACQ) locked = true } else { inSyncFor = 0; locked = false }
        val prev = tr[(pos - 1 + tr.size) % tr.size]
        if (locked && g == lastGain && r0.merCount != prev.merCount && r0.merCount > 0) merCount++
        lastGain = g
        if (locked) { synced++; if (r.ber in 0f..0.05f) good++; if (r.ber >= 0f) { berSum += r.ber; berN++ } }
        if (r.quiet > -90f) { qSum += r.quiet; qN++ }
        if (r.peak >= -0.5f) clip++
        val to = keeper.tick(t * 1000L, g, r, locked, merCount, sweep)
        gains.add(g)
        if (to != GainKeeper.NO_CHANGE) {
            if (System.getenv("TRAJ") != null && offset == 0) println("    ${keeper.name} ${t}s ${g / 10.0} -> ${to / 10.0}  (peak ${r.peak}, MER ${r.lo}/${r.up}, synced ${locked})")
            changes++; g = to
            val nt = traces[nearest(traces, g)]!!
            if (nt[(offset + t + 1) % nt.size].synced.not()) { locked = false; inSyncFor = 0 }
        }
    }
    return keeper.name to Result(gains, synced, good, clip, changes, berSum, berN, qSum, qN, secs)
}

fun main(args: Array<String>) {
    val traces = load(args[0], args[1]); val start = args[2].toInt(); val sweep = args[3] == "1"; val secs = args[4].toInt()
    v4mode = args.size > 5 && args[5] == "v4"
    val n = keepers(if (v4mode) V4 else traces.keys.sorted().toIntArray()).size
    for (b in 0 until n) {
        val rs = (0 until 8).map { run(b, traces, start, sweep, it * 3, secs) }
        val name = rs[0].first; val res = rs.map { it.second }
        val tot = res.sumOf { it.secs }.toDouble()
        val sy = res.sumOf { it.synced } / tot * 100; val gd = res.sumOf { it.good } / tot * 100; val cl = res.sumOf { it.clip } / tot * 100
        val ber = res.sumOf { it.berSum } / maxOf(1, res.sumOf { it.berN }) * 100
        val q = res.sumOf { it.quietSum } / maxOf(1, res.sumOf { it.quietN })
        val ends = res.map { it.gains.last() / 10.0 }.groupingBy { it }.eachCount().entries.sortedByDescending { it.value }.joinToString(" ") { "${it.key}(${it.value})" }
        val first60 = res.flatMap { it.gains.take(60) }.groupingBy { it / 10.0 }.eachCount().maxByOrNull { it.value }!!.key
        val ch = res.map { it.changes }.average()
        println(String.format("  %s: synced %5.1f %%  good HD (BER<5%%) %5.1f %%  avg BER %5.1f %%  clipping %5.1f %%  quieting %6.1f dB  changes %4.1f  ends on %s  (1st minute mostly %.1f)",
            name, sy, gd, ber, cl, q, ch, ends, first60))
    }
}
