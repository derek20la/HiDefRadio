package io.github.derek20la.hidefradio

/**
 * Build 39b: "a favorite asked for program P on the station that is tuned - what now?"
 * Plain Kotlin (no Android), so the harness can test it alone (tools/harness/programwishtest.kt).
 * RadioService's wantedChecker asks 5 times a second until the answer is final.
 *
 * THE BUG THIS FIXES (seen on 105.1 in San Juan Capistrano, no HD reception): tap an HD4
 * favorite, then the HD1 favorite of the same frequency - HD1 stayed silent at "Searching for
 * HD...", the Blend row still said "sub-channel plays live". The old rule was "switch once the
 * program is on the air" for EVERY program. But without HD nothing is ever "on the air", HD1
 * included - so the engine was never told to leave HD4, and after 30 s the wait gave up.
 *
 * The rules now:
 *  - HD1 needs no waiting. It is what a fresh tune plays: the analog signal now, the HD when
 *    it comes (the blend). Select it at once.
 *  - HD2+ that IS on the air: select it (as before).
 *  - HD2+ while NO program is known yet (no HD decoded so far): select it at once too and
 *    keep waiting - silence while searching, exactly as when the favorite is tuned from
 *    another frequency (restartStream() selects it right after the tune). So the same tap
 *    does the same thing whatever was tuned before. If it never appears, wantedChecker
 *    falls back to HD1 as it always did.
 *  - HD2+ that is NOT among the programs the station is known to send: leave things as they
 *    are and keep waiting (as before - the list may still be growing; HD4 shows up later
 *    than HD1-3).
 */
object ProgramWish {
    /** What to do now. [select]: tell the engine to play the wanted program; [done]: stop asking. */
    data class Answer(val select: Boolean, val done: Boolean)

    /**
     * [wanted] = the program asked for (0 = HD1); [playing] = the one the engine plays now;
     * [onAir] = the programs decoded from the station so far (empty = no HD yet).
     */
    fun decide(wanted: Int, playing: Int, onAir: List<Int>): Answer {
        val there = wanted == 0 || wanted in onAir
        val select = wanted != playing && (there || onAir.isEmpty())
        return Answer(select, there)
    }
}
