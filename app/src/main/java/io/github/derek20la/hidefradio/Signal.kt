package io.github.derek20la.hidefradio

import java.util.Locale

/**
 * Milestone 9d: everything about the signal, for the now-playing screen.
 *
 * The native engine reports it as "key=value" lines
 * (RadioEngine.getSignalNative(), added in 9d step 1). [parse] turns that
 * text into one of these objects, so the rest of the app never deals with text.
 * Numbers that aren't known yet are -1 (e.g. ber before the first measurement).
 */
data class Signal(
    val synced: Boolean,      // what to SHOW: really synced, or lost < 1.5 s ago (grace period)
    val locked: Boolean,      // the real sync state right now
    val everSynced: Boolean,  // synced at least once since tuning
    val mode: String,         // service mode, e.g. "MP1", "MP3" ("" = not known yet)
    val am: Boolean,          // 9c: an AM station (HD only: no MER, no analog, no blend)
    val direct: Boolean,      // 14c: direct sampling (a V3-type dongle on AM): the tuner is bypassed, no tuner gain
    val offsetHz: Int,        // tuning error nrsc5 measured
    val merLower: Float,      // modulation error ratio (dB) of each sideband, higher = better
    val merUpper: Float,
    val merCount: Int,        // M10c step 2: MER readings since tuning (a new one every ~1.5 s)
    val ber: Float,           // bit error rate 0..1 (-1 = none yet): now, average, best, worst
    val berAvg: Float,
    val berMin: Float,
    val berMax: Float,
    val kbps: Float,          // audio bit rate of the program playing (0 = not measured yet)
    val crcErrors: Long,      // damaged audio packets since tuning
    val gainDb: Float,        // tuner gain (-1 = tuner AGC)
    val gainAuto: Boolean,
    val peakDbfs: Float,      // how close the dongle is to overload (0 = full scale)
    val gainRemembered: Boolean,  // M10c: auto started at the gain remembered for this station
    val gainSearchMs: Long,       // M10c: how long auto-gain took (-1 = manual gain)
    val syncMs: Long,         // ms from tuning to first sync / first audio (-1 = not yet)
    val audioMs: Long,
    val tunedMs: Long,        // ms since tuning
    val analog: Boolean,      // M11: what you HEAR is the analog FM (Analog only, or Auto while it plays the analog)
    val auto: Boolean,        // 11d: the "HD Radio" setting is Auto (the blend)
    val demodOn: Boolean,     // 11d: the analog demodulator runs (Analog only / Auto; off in Digital only)
    val fmCarrierDb: Float,   // M11: level of the FM channel after filtering, dB (0 = full scale, -99 = not measured)
    val fmQuietDb: Float,     // 9k: FM quieting - noise where the station sends nothing (60-100 kHz),
                              //     dB re 75 kHz deviation; lower = cleaner. ~-8 no station, ~-30 weak, ~-50 strong. -99 = not measured
    val fmOffsetHz: Int,      // M11: carrier offset the FM demodulator sees
    val fmLoadPct: Int,       // M11: FM demodulator CPU time, % of real time
    // 11b: stereo (fmdemod.hpp). The setting: "auto" (stereo, blended to mono when noisy),
    // "mono" (always), "stereo" (never blended). fmPilot = the station sends a 19 kHz pilot;
    // fmPilotPct = its level in % of 75 kHz (stations send 8-10); fmStereoPct = how much
    // stereo is playing right now (100 = full, 0 = mono).
    val fmStereoMode: String,
    val fmPilot: Boolean,
    val fmPilotPct: Float,
    val fmStereoPct: Int,
    val hdLoadPct: Int,       // 11d: nrsc5's decode time, % of real time
    // Build 12: where the engine's thread ran in the last second. The two loads above are
    // processor TIME, and the same work takes several times longer on a slow core clocked
    // down than on a fast one flat out - so they only mean something next to these.
    // -1 = the phone doesn't say.
    val cpuCore: Int,         // which core (0, 1, 2 ...)
    val cpuMhz: Int,          // that core's clock speed, MHz
    val cpuMaxMhz: Int,       // the top speed of the phone's fastest core, MHz
    // 11d: the blend (blend.hpp), Auto only.
    val blendState: String,   // "analog", "toHd", "hd", "toAnalog"; "sub" = a sub-channel plays live; "off" = not Auto
    val blendAligned: Boolean,    // the HD (when it plays) is time-aligned with the analog
    val blendMismatch: Boolean,   // the aligner found no match: the HD1 is not this analog station's programme
    val blendToHd: Int,       // fades to HD / to analog since tuning
    val blendToAnalog: Int,
    val blendHdSecs: Int,     // seconds of HD played since tuning
    val blendSinceSecs: Int,  // seconds since the current HD stretch began (0 while on analog)
    val blendMatchDb: Float,  // level correction applied to the analog, dB (negative = turned down)
    val blendReason: String,  // why the last fade happened, e.g. "HD is good", "dropout ahead"
    // Build 10: blendState "alone" = the analog on this frequency is ANOTHER station's, so the
    // HD stands alone like a sub-channel: it plays, or - in a dropout - there is silence.
    val blendSilent: Boolean,     // ... the silence is waiting for the HD right now
    val blendSilentSecs: Int,     // ... for this long (after 10 s the other station's analog is given back)
    val blendSilences: Int,       // fades to silence since tuning
    // 11c: analog <-> HD1 alignment (aligner.hpp). Measured from ~8 s after tuning, then every 5 s.
    val alignState: String,   // "waiting" (no HD audio yet), "measuring", "ok" (two results agree), "nomatch"
    val alignMs: Float,       // how far the analog lags HD1, real time, ms (> 0 = HD is ahead)
    val alignFrames: Long,    // the same as analog frame index - HD1 frame index (what 11d's delay line needs)
    val alignCorr: Float,     // correlation of the two streams at the match (1 = identical; ~0.95 is typical)
    val alignGainDb: Float,   // analog loudness relative to HD1, dB (> 0 = analog louder)
    val alignCount: Int,      // accepted measurements on this station
    val alignTries: Int,      // measurements attempted
    val alignJumps: Int,      // times the alignment changed (after a sync loss nrsc5 can shift by one 93 ms piece)
    // Build 6: the clock correction (drift.hpp). The analog audio is paced by the dongle's
    // crystal, the HD audio by the station's clock; the app measures the difference and
    // corrects the analog, so the two stay lined up with any dongle.
    val clockState: String,   // "off" (nothing known / demodulator off), "guess" (from the tuning offset), "measured" (from the audio)
    val clockPpm: Float,      // the dongle's clock error, parts per million (> 0 = fast). TCXO dongles: ~1; plain crystals: 30-100
    // Build 8: the app has two witnesses to the dongle's clock - the tuning offset (there at
    // once, good to a ppm or two) and the audio (much better, but only after half a minute of
    // measurements that agree). When they contradict each other, one of them is not used:
    val clockUnused: String,  // "" (they agree), "audio" (too far from the tuning offset, no strong case), "tuning" (the audio proved it wrong)
    val clockUnusedPpm: Float,    // what the one that is not used says
    val station: String,      // e.g. "KRTH" ("" = not received yet)
    val slogan: String,
    // M12: RDS / RBDS - the data an ANALOG FM station sends (rds.hpp). All empty while the
    // demodulator is off (Digital only, AM) or the station sends none.
    val rdsSync: Boolean,     // the decoder is locked to the data
    val rdsBler: Int,         // % of data blocks with errors in the last ~1.5 s (0 = perfect, 100 = nothing)
    val rdsPi: String,        // the station's PI code, 4 hex digits ("" = not confirmed yet)
    val rdsCall: String,      // call letters fit to be shown as the station's name (North America; "" = can't tell):
                              // licensed on this frequency, or named in the station's own text
    val rdsCallMaybe: String, // build 10: what the US formula makes of the code when nobody vouches for it
                              // ("WHTZ" for XHITZ-FM's 692B) - a note for the details, never a name
    val rdsPs: String,        // the 8-character name - or scrolling text, 8 characters at a time
    val rdsPsSecs: Int,       // how long the PS has stayed the same (a real name stands still)
    val rdsRt: String,        // RadioText (up to 64 characters)
    val rdsTitle: String,     // RT+ tags: song title / artist, when the station marks them
    val rdsArtist: String,
    val rdsPty: Int,          // program type number 0-31 (-1 = not known) - names: Rds.ptyName()
    // M12 step 2: is the HD the same station as the analog? (the RDS PI against the HD's
    // call sign; kept until the next tune, also while the demodulator is off)
    val hdSame: Int,          // HD_SAME_UNKNOWN / HD_SAME_YES / HD_SAME_NO
    val hdSameCall: String,   // the ANALOG station's call letters ("" = can't tell)
    val hdOtherName: String,  // the HD's own name as sent ("KSOF-FM"), kept until the next tune
    // Build 10 (stationid.hpp): two stations on one frequency - KDSC's HD and XETRA-FM's
    // analog on 91.1, 2026-10-09.
    val hdOwnPi: String,      // the HD station's OWN RDS code, 4 hex digits ("" = not known) - saved logos are filed under it
    val hdContest: Int,       // is another station's analog on this frequency right now? HD_CONTEST_NONE / SUSPECTED / CONFIRMED
    // 12a step 3: the HD decoder runs (off in Analog only - nrsc5 gets no samples then)
    val hdOn: Boolean,
    // Build 9: the HD search on a station without HD (hdsearch.hpp, Auto only). After 30 s
    // without HD the search rests and only looks again for 3 s in every 30:
    //   "on"   = the decoder gets every sample (searching, or decoding the HD)
    //   "rest" = it gets none          "look" = one of the short looks between two rests
    val hdSearch: String,
    val hdLookIn: Int,        // while resting: seconds until the next look
) {
    /** Build 9: the HD search has gone to rest on this station (resting, or taking a look). */
    val hdResting: Boolean
        get() = hdSearch == "rest" || hdSearch == "look"

    /**
     * M12: a station NAME from RDS, for stations without HD: the call letters, or the PS
     * once it has stood still for a while (LA stations love to scroll song titles through
     * the PS - that is text, not a name).
     */
    val rdsName: String
        get() = rdsCall.ifEmpty { if (rdsPsSecs >= RDS_NAME_AFTER_S) rdsPs else "" }

    /** M12: "what's on" from RDS: the tagged song title, else the whole RadioText. */
    val rdsLine1: String
        get() = if (rdsTitle.isNotBlank()) rdsTitle else rdsRt
    val rdsLine2: String
        get() = if (rdsTitle.isNotBlank()) rdsArtist else ""

    /**
     * M12 step 2: the HD on this frequency is ANOTHER station's (RDS says KHHT, the HD
     * says KSOF) - but only once the AUDIO has said so too: the aligner gave up ("nomatch").
     * A name alone isn't proof: KTWV's HD calls itself "WAVE" (looks like a W call sign!),
     * and PIs are often out of date after a change of call letters. (12a step 4: was
     * "not ok", which showed the notice on KTWV while the aligner was still measuring.)
     * (12a step 3: also while the HD is off.)
     */
    val otherHd: Boolean
        get() = hdSame == HD_SAME_NO && (alignState == "nomatch" || hdContest == HD_CONTEST_CONFIRMED)

    /**
     * Build 13: two stations share this frequency, and the analog coming in RIGHT NOW is the HD
     * station's own again (its RDS code = [hdOwnPi]) - but the contest still holds (decision 59
     * keeps it for 60 s after the other station's code was last seen). Seen on the air 10-10
     * (91.1, KDSC + XETRA): without this, the lines said "the analog here is another station"
     * while the RDS row said KDSC. Each was true of its own moment; read together they
     * contradicted. Only the WORDING changes with this - what plays doesn't.
     */
    val ownAnalogBack: Boolean
        get() = hdContest == HD_CONTEST_CONFIRMED && hdOwnPi.isNotEmpty() &&
                rdsPi.equals(hdOwnPi, ignoreCase = true)

    /** Build 10: the HD stands alone (the analog here is another station's): it plays, or the silence waits. */
    val blendAlone: Boolean
        get() = blendState == "alone"

    /** Synced a moment ago, lost it, and still inside the grace period. */
    val holding: Boolean
        get() = synced && !locked

    companion object {
        /** Before anything is known (radio off). */
        val NONE = parse("")

        /** M12: a PS that hasn't changed for this many seconds counts as the station's name. */
        const val RDS_NAME_AFTER_S = 15

        // M12 step 2: hdSame values (= blend::SameStation in blend.hpp)
        const val HD_SAME_UNKNOWN = 0
        const val HD_SAME_YES = 1
        const val HD_SAME_NO = 2

        // Build 10: hdContest values (= ident::CONTEST_* in stationid.hpp). "Suspected" = a
        // code that is not the HD station's is coming in, and the audio is being checked;
        // only "confirmed" changes what is shown and heard.
        const val HD_CONTEST_NONE = 0
        const val HD_CONTEST_SUSPECTED = 1
        const val HD_CONTEST_CONFIRMED = 2

        /** Turns RadioEngine.getSignalNative()'s text into a Signal. */
        fun parse(text: String): Signal {
            // "merLower=12.3" -> map["merLower"] = "12.3". Split at the FIRST "="
            // only, so a station name containing "=" stays whole.
            val map = text.lines()
                .filter { '=' in it }
                .associate { it.substringBefore('=') to it.substringAfter('=') }

            fun str(key: String) = map[key] ?: ""
            fun bool(key: String) = map[key] == "1"
            fun float(key: String, default: Float = -1f) = map[key]?.toFloatOrNull() ?: default
            fun long(key: String, default: Long = -1) = map[key]?.toLongOrNull() ?: default

            return Signal(
                synced = bool("synced"),
                locked = bool("locked"),
                everSynced = bool("everSynced"),
                mode = str("mode"),
                am = bool("am"),                                       // 9c
                direct = bool("direct"),                               // 14c
                offsetHz = float("offsetHz", 0f).toInt(),
                merLower = float("merLower", 0f),
                merUpper = float("merUpper", 0f),
                merCount = long("merCount", 0).toInt(),
                ber = float("ber"),
                berAvg = float("berAvg"),
                berMin = float("berMin"),
                berMax = float("berMax"),
                kbps = float("kbps", 0f),
                crcErrors = long("crcErrors", 0),
                gainDb = float("gainDb"),
                gainAuto = bool("gainAuto"),
                // M10b step 2: "no value" = -99 (quiet), like the native side.
                // It was 0 = "full scale", so Signal.NONE (shown while retuning
                // since 10b) looked like an overload -> red warning for a moment.
                peakDbfs = float("peakDbfs", -99f),
                gainRemembered = bool("gainRemembered"),
                gainSearchMs = long("gainSearchMs"),
                syncMs = long("syncMs"),
                audioMs = long("audioMs"),
                tunedMs = long("tunedMs", 0),
                // M11: what you hear. 11d: in Auto that depends on the blend's state.
                analog = str("audioSource") == "analog" ||
                         (str("audioSource") == "auto" && str("blendState") == "analog"),
                auto = str("audioSource") == "auto",                   // 11d
                demodOn = bool("demodOn"),
                fmCarrierDb = float("fmCarrierDb", -99f),
                fmQuietDb = float("fmQuietDb", -99f),                  // 9k
                fmOffsetHz = float("fmOffsetHz", 0f).toInt(),
                fmLoadPct = long("fmLoadPct", 0).toInt(),
                fmStereoMode = str("fmStereoMode").ifEmpty { "auto" },   // 11b
                fmPilot = bool("fmPilot"),
                fmPilotPct = float("fmPilotPct", 0f),
                fmStereoPct = long("fmStereoPct", 0).toInt(),
                hdLoadPct = long("hdLoadPct", 0).toInt(),
                cpuCore = long("cpuCore", -1).toInt(),                 // build 12
                cpuMhz = long("cpuMhz", -1).toInt(),
                cpuMaxMhz = long("cpuMaxMhz", -1).toInt(),
                blendState = str("blendState").ifEmpty { "off" },     // 11d
                blendAligned = bool("blendAligned"),
                blendMismatch = bool("blendMismatch"),
                blendToHd = long("blendToHd", 0).toInt(),
                blendToAnalog = long("blendToAnalog", 0).toInt(),
                blendHdSecs = long("blendHdSecs", 0).toInt(),
                blendSinceSecs = long("blendSinceSecs", 0).toInt(),
                blendMatchDb = float("blendMatchDb", 0f),
                blendReason = str("blendReason"),
                blendSilent = bool("blendSilent"),                     // build 10
                blendSilentSecs = long("blendSilentSecs", 0).toInt(),
                blendSilences = long("blendSilences", 0).toInt(),
                alignState = str("alignState").ifEmpty { "waiting" },   // 11c
                alignMs = float("alignMs", 0f),
                alignFrames = long("alignFrames", 0),
                alignCorr = float("alignCorr", 0f),
                alignGainDb = float("alignGainDb", 0f),
                alignCount = long("alignCount", 0).toInt(),
                alignTries = long("alignTries", 0).toInt(),
                alignJumps = long("alignJumps", 0).toInt(),
                clockState = str("clockState").ifEmpty { "off" },       // build 6
                clockPpm = float("clockPpm", 0f),
                clockUnused = str("clockUnused"),                       // build 8
                clockUnusedPpm = float("clockUnusedPpm", 0f),
                station = str("station").trim(),
                slogan = str("slogan").trim(),
                rdsSync = bool("rdsSync"),                             // M12
                rdsBler = long("rdsBler", 100).toInt(),
                rdsPi = str("rdsPi").trim(),
                rdsCall = str("rdsCall").trim(),
                rdsCallMaybe = str("rdsCallMaybe").trim(),             // build 10
                rdsPs = str("rdsPs").trim(),
                rdsPsSecs = long("rdsPsSecs", 0).toInt(),
                rdsRt = str("rdsRt").trim(),
                rdsTitle = str("rdsTitle").trim(),
                rdsArtist = str("rdsArtist").trim(),
                rdsPty = long("rdsPty", -1).toInt(),
                hdSame = long("hdSame", 0).toInt(),                    // M12 step 2
                hdSameCall = str("hdSameCall").trim(),
                hdOtherName = str("hdOtherName").trim(),
                hdOwnPi = str("hdOwnPi").trim(),                       // build 10
                hdContest = long("hdContest", 0).toInt(),
                hdOn = bool("hdOn"),                                   // 12a step 3
                hdSearch = str("hdSearch").ifEmpty { "on" },           // build 9
                hdLookIn = long("hdLookIn", 0).toInt(),
            )
        }

        /** 0.000131 -> "0.01%", -1 -> "-" (Locale.US: always a dot). */
        fun percent(ber: Float, decimals: Int = 2): String =
            if (ber < 0) "-" else "%.${decimals}f%%".format(Locale.US, ber * 100)

        /** 2160 -> "2.16 s", -1 -> "-" */
        fun seconds(ms: Long): String =
            if (ms < 0) "-" else "%.2f s".format(Locale.US, ms / 1000.0)
    }
}
