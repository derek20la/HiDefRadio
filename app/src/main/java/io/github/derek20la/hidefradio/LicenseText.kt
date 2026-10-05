package io.github.derek20la.hidefradio

/**
 * Milestone 9f: helps show license texts on the About screen.
 *
 * License files (GPL, Apache...) are written for 80-column terminals: every
 * line ends after ~72 characters. On a phone that makes ragged half-lines.
 * [unwrap] joins the lines of each paragraph back together, so Android can
 * wrap the text to the width of the screen.
 *
 * Plain Kotlin (no Android), so it can be tested on its own.
 */
object LicenseText {

    /**
     * Joins the lines of each paragraph into one line.
     * - A blank line ends a paragraph (and is kept).
     * - A line made only of - or = (a "rule" under a heading) stays on its own.
     * - Leading spaces are dropped (centered titles would otherwise start
     *   halfway across a phone screen).
     */
    fun unwrap(text: String): String {
        val out = StringBuilder()
        var inParagraph = false   // are we in the middle of a paragraph?
        for (rawLine in text.replace("\r\n", "\n").split("\n")) {
            val line = rawLine.trimEnd()
            when {
                line.isEmpty() -> {
                    // Blank line: end the paragraph and keep the blank line.
                    out.append('\n')
                    if (inParagraph) out.append('\n')
                    inParagraph = false
                }
                isRule(line) -> {
                    // A line of ---- or ====: always on its own line.
                    if (inParagraph) out.append('\n')
                    out.append(line.trim()).append('\n')
                    inParagraph = false
                }
                inParagraph -> out.append(' ').append(line.trim())   // join
                else -> {
                    out.append(line.trim())   // first line of a paragraph
                    inParagraph = true
                }
            }
        }
        if (inParagraph) out.append('\n')
        // Tidy up: never more than one blank line in a row, none at the ends.
        return out.toString().replace(Regex("\n{3,}"), "\n\n").trim('\n')
    }

    private fun isRule(line: String): Boolean {
        val t = line.trim()
        return t.length >= 3 && (t.all { it == '-' } || t.all { it == '=' })
    }
}
