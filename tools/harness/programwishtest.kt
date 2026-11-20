// Harness only (build 39b): ProgramWish.kt alone - the rule RadioService's wantedChecker follows when
// a favorite asks for a program. Needs kotlinc:
//   kotlinc ../../app/src/main/java/io/github/derek20la/hidefradio/ProgramWish.kt programwishtest.kt -include-runtime -d build/pw.jar && java -jar build/pw.jar
// "Engine" below does what the native engine does with setProgramNative(); each test plays the
// checker's ticks (decide -> select -> done?) and looks at where the engine ends up.
import io.github.derek20la.hidefradio.ProgramWish

var failed = 0
fun check(name: String, ok: Boolean) { println((if (ok) "ok    " else "FAIL  ") + name); if (!ok) failed++ }

/** Plays up to [ticks] checker ticks. Returns the engine's program, and whether the wish was settled. */
fun play(wanted: Int, playing: Int, ticks: Int = 3, onAir: (tick: Int) -> List<Int>): Pair<Int, Boolean> {
    var engine = playing
    for (t in 0 until ticks) {
        val a = ProgramWish.decide(wanted, engine, onAir(t))
        if (a.select) engine = wanted
        if (a.done) return engine to true
    }
    return engine to false
}

fun main() {
    val none = emptyList<Int>()
    // The bug: no HD reception, the engine is on HD4 (silent) from the HD4 favorite; now the HD1 favorite.
    check("no HD, on HD4, HD1 wanted -> HD1 at once, settled", play(0, 3) { none } == (0 to true))
    check("no HD, on HD2, HD1 wanted -> HD1 at once, settled", play(0, 1) { none } == (0 to true))
    check("already on HD1, HD1 wanted -> nothing to do, settled", ProgramWish.decide(0, 0, none) == ProgramWish.Answer(false, true))
    // And back: no HD, on HD1 (analog playing), the HD4 favorite of the same frequency.
    check("no HD, on HD1, HD4 wanted -> HD4 at once (silence), still waiting", play(3, 0) { none } == (3 to false))
    check("  ... and it is selected once only", ProgramWish.decide(3, 3, none) == ProgramWish.Answer(false, false))
    check("  ... HD arrives with HD4 -> settled on HD4", play(3, 0, 5) { if (it < 3) none else listOf(0, 1, 2, 3) } == (3 to true))
    // After a retune the engine is on the sub-channel already (restartStream selects it).
    check("fresh tune to HD4, no HD yet -> stays on HD4, waiting", play(3, 3) { none } == (3 to false))
    // With HD (the cases that worked before - unchanged).
    check("HD1-4 on the air, on HD1, HD4 wanted -> HD4, settled", play(3, 0) { listOf(0, 1, 2, 3) } == (3 to true))
    check("HD1-4 on the air, on HD4, HD1 wanted -> HD1, settled", play(0, 3) { listOf(0, 1, 2, 3) } == (0 to true))
    check("HD1-4 on the air, on HD4, HD2 wanted -> HD2, settled", play(1, 3) { listOf(0, 1, 2, 3) } == (1 to true))
    check("only HD1-2 on the air, on HD1, HD4 wanted -> stays on HD1, waiting (as before)", play(3, 0) { listOf(0, 1) } == (0 to false))
    check("  ... HD4 shows up later -> HD4, settled", play(3, 0, 5) { if (it < 3) listOf(0, 1) else listOf(0, 1, 2, 3) } == (3 to true))
    // Every combination: HD1 is always settled at once, and the engine never ends on a program nobody asked for.
    var all = true
    for (w in 0..7) for (p in 0..7) for (mask in 0..255) {
        val air = (0..7).filter { mask shr it and 1 == 1 }
        val (e, done) = play(w, p, 2) { air }
        if (w == 0 && !(e == 0 && done)) all = false
        if (e != w && e != p) all = false
        if (done && e != w) all = false
    }
    check("all 16,384 combinations: HD1 settles at once on HD1; settled = playing what was asked", all)
    println(if (failed == 0) "ProgramWish: all checks pass" else "ProgramWish: $failed FAILED")
}
