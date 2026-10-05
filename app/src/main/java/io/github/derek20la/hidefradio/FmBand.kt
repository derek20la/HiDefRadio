package io.github.derek20la.hidefradio

import java.util.Locale

/**
 * Milestone 9e step 2: the FM band of your region (a setting).
 *
 *   minHz..maxHz            = what you may TYPE (tap the display)
 *   firstHz..lastHz, stepHz = the channels ◀ ▶ step through
 *   extraHz                 = one extra channel below firstHz, or 0 (9i)
 *
 * 9i (Derek's idea): the Americas band starts at 87.75 MHz - the sound of
 * analog TV channel 6. Some channel-6 TV stations (ATSC 3.0) still send an
 * FM audio carrier there ("Franken FM"), in LA, Chicago and other cities.
 * ❮ ❯ step 87.75, 87.9, 88.1 ... 107.9, so you can find one even if you
 * never knew your town had it. Typed frequencies round to 0.05 MHz.
 *
 * The US uses channels 0.2 MHz apart on odd tenths (87.9, 88.1 ... 107.9),
 * most of the world 0.1 MHz. HD Radio is mostly an Americas thing, so
 * AMERICAS is the default; the others are there for completeness.
 *
 * 9c: each region also has its AM band (HD Radio on AM = MA1/MA3, the US has a
 * few hundred such stations; e.g. KMZT 1260 and KBRT 740 in LA). Anything below
 * 30 MHz is AM ([isAm]); the same ❮ ❯ step through the AM channels then:
 * the Americas 530-1700 kHz in 10 kHz steps, the rest of the world 531-1602
 * in 9 kHz steps. (The name "FmBand" stayed; the setting is the region.)
 *
 * An `enum class` = a fixed list of choices, each with its own values.
 * Plain Kotlin (no Android), so it's tested on a computer.
 */
enum class FmBand(
    val key: String,            // saved in the settings
    val minHz: Int, val maxHz: Int,
    val firstHz: Int, val lastHz: Int, val stepHz: Int,
    val extraHz: Int = 0,
    // 9c: the AM band of the region (typing and ❮ ❯ use the same grid)
    val amFirstHz: Int = 531_000, val amLastHz: Int = 1_602_000, val amStepHz: Int = 9_000,
) {
    // 9i: Americas = 87.75 (TV channel 6 audio) + 87.9 ... 107.9 (was typed 87.5-108.0)
    AMERICAS("americas", 87_750_000, 107_900_000, 87_900_000, 107_900_000, 200_000, 87_750_000,
             530_000, 1_700_000, 10_000),
    WORLD("world", 87_500_000, 108_000_000, 87_500_000, 108_000_000, 100_000),
    // Japan: 76-90 MHz, "wide FM" to 95 MHz (2014), and 95-99 MHz opened on 2025-05-19
    // (Derek found it: AFN expanding on FM in Japan). 99-108 MHz may follow later.
    JAPAN("japan", 76_000_000, 99_000_000, 76_000_000, 99_000_000, 100_000),
    BRAZIL("brazil", 76_000_000, 108_000_000, 76_100_000, 107_900_000, 200_000, 0,
           530_000, 1_700_000, 10_000);

    /**
     * 11b step 2: FM de-emphasis in microseconds. Stations boost the treble by this
     * much and the receiver turns it back down (that also turns the hiss down).
     * 75 us in the Americas (incl. Brazil) and South Korea, 50 us everywhere else.
     */
    val deemphasisUs: Int
        get() = if (this == AMERICAS || this == BRAZIL) 75 else 50

    /** 9i: every channel ❮ ❯ can land on, lowest first (extraHz, then the grid). */
    val channels: IntArray by lazy {
        val grid = (0..(lastHz - firstHz) / stepHz).map { firstHz + it * stepHz }
        (if (extraHz > 0) listOf(extraHz) + grid else grid).toIntArray()
    }

    /** 9c: every AM channel, lowest first. */
    val amChannels: IntArray by lazy {
        IntArray((amLastHz - amFirstHz) / amStepHz + 1) { amFirstHz + it * amStepHz }
    }

    /** Can you tune here by typing it? 9c: FM range, or an AM channel. */
    fun contains(hz: Int): Boolean =
        if (isAm(hz)) hz in amFirstHz..amLastHz && (hz - amFirstHz) % amStepHz == 0
        else hz in minHz..maxHz

    /**
     * ◀ (up = false) or ▶ (up = true): the next channel. Always lands ON a
     * channel (101.2 ▶ -> 101.3 in the Americas), and wraps around at the
     * ends of the band (107.9 ▶ -> 87.9).
     */
    fun step(hz: Int, up: Boolean): Int {
        val list = if (isAm(hz)) amChannels else channels                 // 9c: AM steps stay on AM
        return if (up) list.firstOrNull { it > hz } ?: list.first()     // wrap to the bottom
               else list.lastOrNull { it < hz } ?: list.last()          // wrap to the top
    }

    /** "87.75–107.9" for messages. */
    fun rangeText(): String = "${mhzText(minHz)}–${mhzText(maxHz)}"

    /** 9c: "530–1700" (kHz) for messages. */
    fun amRangeText(): String = "${amFirstHz / 1000}–${amLastHz / 1000}"

    /** 9c: the step in kHz, for messages ("10"). */
    fun amStepKhz(): Int = amStepHz / 1000

    /**
     * 9c: what you typed -> Hz, or -1 if it's no frequency of this band.
     * Numbers from 100 up are kHz (AM): "1260" -> 1_260_000. Anything else is MHz
     * (FM), rounded to 0.05 MHz: "105.1" -> 105_100_000, "87.75" -> 87_750_000.
     * A comma works as a decimal point too ("105,1").
     */
    fun parseTyped(text: String): Int {
        val v = text.trim().replace(',', '.').toDoubleOrNull() ?: return -1
        val hz = when {
            v >= 100.0 && v < 10_000.0 && v >= amFirstHz / 1000 - 50 -> Math.round(v).toInt() * 1000   // kHz
            v >= 1.0 && v < 1000.0 -> Math.round(v * 20).toInt() * 50_000                                 // MHz
            else -> -1
        }
        return if (hz > 0 && contains(hz)) hz else -1
    }

    companion object {
        val DEFAULT = AMERICAS

        /** 9c: below this it's AM (the native side uses the same limit). */
        const val AM_BELOW_HZ = 30_000_000

        /** 9c: is this an AM frequency? */
        fun isAm(hz: Int): Boolean = hz in 1 until AM_BELOW_HZ

        /** 9c: the number as shown: "101.1" / "87.75" (MHz) or "1260" (kHz). */
        fun freqText(hz: Int): String = if (isAm(hz)) "${hz / 1000}" else mhzText(hz)

        /** 9c: with the band: "101.1 FM" / "1260 AM". */
        fun bandText(hz: Int): String = freqText(hz) + if (isAm(hz)) " AM" else " FM"

        /**
         * 9i: a frequency as text, in MHz: 101_100_000 -> "101.1", and two
         * decimals only when needed: 87_750_000 -> "87.75".
         */
        fun mhzText(hz: Int): String =
            if (hz % 100_000 == 0) String.format(Locale.US, "%.1f", hz / 1_000_000.0)
            else String.format(Locale.US, "%.2f", hz / 1_000_000.0)

        /** The saved setting -> the band (unknown/missing -> AMERICAS). */
        fun fromKey(key: String?): FmBand = entries.firstOrNull { it.key == key } ?: DEFAULT
    }
}
