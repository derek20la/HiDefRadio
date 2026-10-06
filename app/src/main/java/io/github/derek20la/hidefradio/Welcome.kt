package io.github.derek20la.hidefradio

import android.content.Context
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * Build 7: the welcome card - four lines that tell a newcomer how this radio works.
 *
 * It is shown once, the first time the app is opened (MainActivity), and can be read
 * again any time under Settings > About > "Getting started" (SettingsActivity). A hint
 * that shows only once is easily missed, so it must have a place where it stays.
 *
 * The text is R.string.welcome_text. Whether it has been read is remembered in the "ui"
 * preferences file (the one MainActivity keeps its own little switches in), so "Clear
 * saved data" in the settings does not bring it back - clearing the app's storage does.
 */
object Welcome {
    private const val PREFS = "ui"
    private const val KEY_SEEN = "welcome_seen"

    /** True until the card has been closed once. */
    fun wanted(context: Context): Boolean =
        !context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_SEEN, false)

    /**
     * Shows the card. onClosed runs when it goes away - "Got it", the back key or a tap
     * beside it all count as read.
     */
    fun show(context: Context, onClosed: (() -> Unit)? = null): AlertDialog =
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.welcome_title)
            .setMessage(R.string.welcome_text)
            .setPositiveButton(R.string.welcome_ok, null)
            .setOnDismissListener {
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                    .edit().putBoolean(KEY_SEEN, true).apply()
                onClosed?.invoke()
            }
            .show()
}
