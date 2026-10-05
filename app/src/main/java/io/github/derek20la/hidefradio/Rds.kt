package io.github.derek20la.hidefradio

/**
 * M12: names for RDS program types (PTY). The number a station sends means
 * different things in North America (RBDS) and in the rest of the world (RDS):
 * 5 is "Rock" here and "Education" in Europe.
 */
object Rds {
    // North America - RBDS (NRSC-4-B, annex F)
    private val RBDS = arrayOf(
        "", "News", "Information", "Sports", "Talk", "Rock", "Classic rock", "Adult hits",
        "Soft rock", "Top 40", "Country", "Oldies", "Soft", "Nostalgia", "Jazz", "Classical",
        "Rhythm and blues", "Soft R&B", "Foreign language", "Religious music", "Religious talk",
        "Personality", "Public", "College", "Spanish talk", "Spanish music", "Hip hop", "", "",
        "Weather", "Emergency test", "Emergency",
    )

    // Everywhere else - RDS (IEC 62106)
    private val RDS = arrayOf(
        "", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture",
        "Science", "Varied", "Pop music", "Rock music", "Easy listening", "Light classical",
        "Serious classical", "Other music", "Weather", "Finance", "Children's programmes",
        "Social affairs", "Religion", "Phone-in", "Travel", "Leisure", "Jazz music",
        "Country music", "National music", "Oldies music", "Folk music", "Documentary",
        "Alarm test", "Alarm",
    )

    /** 20, RBDS -> "Religious talk"; 0, unknown or unassigned -> "". */
    fun ptyName(pty: Int, rbds: Boolean): String {
        val names = if (rbds) RBDS else RDS
        return if (pty in names.indices) names[pty] else ""
    }
}
