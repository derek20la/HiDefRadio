package io.github.derek20la.hidefradio

import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Bundle
import android.text.format.DateUtils
import android.text.format.Formatter
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.pm.PackageInfoCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.github.derek20la.hidefradio.databinding.ActivitySettingsBinding
import java.io.IOException
import java.util.Locale

/**
 * Milestone 9e step 2: the settings screen (opened with ⚙ on the main screen).
 *
 * Android's "preference" library does almost everything: it draws the list
 * from res/xml/settings.xml, shows the choices, and saves them. RadioService
 * listens for changes, so a new setting works right away, even while playing.
 *
 * 9f: an About section at the bottom (version, license, source code link,
 * open-source licenses; 14c: the privacy policy).
 * 13a step 2: a Storage section above it: "Clear saved data" (station logos,
 * remembered gains).
 */
class SettingsActivity : AppCompatActivity() {

    // 9h: Theme changed -> redraw this screen in the new look. (Light <-> dark
    // AppCompat redraws by itself; dark gray <-> true black we do here.)
    // MainActivity notices by itself when you go back (onStart).
    private val themeListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == Settings.KEY_THEME && !Settings.applyNightMode(this)) recreate()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        Settings.applyNightMode(this)                     // 9h: light / dark
        super.onCreate(savedInstanceState)
        Settings.applyBlack(this)                         // 9h: true black
        val binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        Settings.styleSystemBars(this)                    // 9h
        Settings.prefs(this).registerOnSharedPreferenceChangeListener(themeListener)

        // The toolbar at the top, with a ← back arrow.
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)

        // Edge-to-edge (Android 15+): keep clear of the status and navigation bars.
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }

        // Put the settings list into the screen (only the first time - after
        // turning the phone, Android brings the old one back by itself).
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(R.id.settings_container, SettingsFragment())
                .commit()
        }
    }

    override fun onDestroy() {
        Settings.prefs(this).unregisterOnSharedPreferenceChangeListener(themeListener)
        super.onDestroy()
    }

    /** The ← arrow: back to the radio. */
    override fun onSupportNavigateUp(): Boolean {
        finish()
        return true
    }

    /** The list of settings itself (a "fragment" = a piece of a screen). */
    class SettingsFragment : PreferenceFragmentCompat() {
        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            setPreferencesFromResource(R.xml.settings, rootKey)

            // Gain choices: "Auto (recommended)", "0.0 dB", "0.9 dB" ... "49.6 dB".
            // The saved value is "auto" or the gain in tenths of a dB ("197").
            findPreference<ListPreference>(Settings.KEY_GAIN)?.let { pref ->
                val names = listOf(getString(R.string.gain_auto)) +
                    Settings.GAIN_STEPS.map { String.format(Locale.US, "%.1f dB", it / 10.0) }
                val values = listOf("auto") + Settings.GAIN_STEPS.map { it.toString() }
                pref.entries = names.toTypedArray<CharSequence>()
                pref.entryValues = values.toTypedArray<CharSequence>()
            }

            // --- 13a step 2: Storage ---

            // "Clear saved data" → a list with check boxes: what to clear.
            findPreference<Preference>(KEY_CLEAR)?.setOnPreferenceClickListener {
                showClearDialog()
                true
            }
            updateClearSummary()

            // --- 9f: About ---

            // "HiDef Radio / Version 1.0-beta1 (build 1) / Installed Oct 2, 8:15 PM"
            findPreference<Preference>(KEY_VERSION)?.summary = versionSummary()

            // "Free software (GNU GPL v3 or later)" → show the GPL.
            findPreference<Preference>(KEY_LICENSE)?.setOnPreferenceClickListener {
                showLicense(getString(R.string.about_license), "GPL-3.0.txt")
                true
            }

            // "Source code" → open the GitHub page in the web browser.
            // (Only when tapped - the app itself never goes on the internet.)
            findPreference<Preference>(KEY_SOURCE)?.setOnPreferenceClickListener {
                openSourcePage()
                true
            }

            // "Open-source licenses" → list of licenses → the chosen text.
            findPreference<Preference>(KEY_LICENSES)?.setOnPreferenceClickListener {
                showLicenseList()
                true
            }

            // 14c: "Privacy policy" → the short version in a dialog, with a button to the
            // full page. (Google Play asks for the policy inside the app as well.)
            findPreference<Preference>(KEY_PRIVACY)?.setOnPreferenceClickListener {
                showPrivacy()
                true
            }

            // 13b: "Station list / FCC data of 2026-10-01 ..." - how old the built-in list is.
            findPreference<Preference>(KEY_STATIONS)?.summary =
                getString(R.string.about_stations_summary, Stations.info(requireContext()))
        }

        /** Back on this screen: the numbers may have changed (a new logo arrived). */
        override fun onResume() {
            super.onResume()
            updateClearSummary()
        }

        /** "14 station logos (212 kB), tuner gain remembered for 23 stations. ..." */
        private fun updateClearSummary() {
            val context = requireContext()
            findPreference<Preference>(KEY_CLEAR)?.summary = getString(
                R.string.setting_clear_summary,
                LogoCache.count(context), fileSize(LogoCache.bytes(context)), GainMemory.count(context))
        }

        /** 217_000 -> "212 kB" (Android's own wording). */
        private fun fileSize(bytes: Long): String =
            Formatter.formatShortFileSize(requireContext(), bytes)

        /**
         * 13a step 2: what should be cleared? Logos are ticked already (that's the
         * usual reason to come here: a wrong or broken logo that stuck); the gains
         * are not. Presets, your names and the settings are never touched.
         */
        private fun showClearDialog() {
            val context = requireContext()
            val items = arrayOf<CharSequence>(
                getString(R.string.clear_logos, LogoCache.count(context), fileSize(LogoCache.bytes(context))),
                getString(R.string.clear_gains, GainMemory.count(context)),
            )
            val ticked = booleanArrayOf(true, false)
            MaterialAlertDialogBuilder(context)
                .setTitle(R.string.setting_clear)
                .setMultiChoiceItems(items, ticked) { _, which, isChecked -> ticked[which] = isChecked }
                .setPositiveButton(R.string.clear_button) { _, _ ->
                    if (ticked[0]) LogoCache.clear(context)
                    if (ticked[1]) GainMemory.clear(context)
                    val message = if (ticked[0] || ticked[1]) R.string.clear_done else R.string.clear_nothing
                    Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
                    updateClearSummary()
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }

        /**
         * Which build is this? Two lines for the About row:
         *   "Version 1.0-beta1 (build 1)"  - versionName and versionCode from app/build.gradle.kts
         *                                    (the dev build's name ends in "-dev")
         *   "Installed Oct 2, 8:15 PM"     - when THIS build was put on the phone (14b: so a
         *                                    screenshot shows which build it came from)
         */
        @Suppress("DEPRECATION")   // the newer call needs Android 13; this one works everywhere
        private fun versionSummary(): String {
            val context = requireContext()
            val info = context.packageManager.getPackageInfo(context.packageName, 0)
            val build = PackageInfoCompat.getLongVersionCode(info)
            val installed = DateUtils.formatDateTime(
                context, info.lastUpdateTime,
                DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME or DateUtils.FORMAT_ABBREV_MONTH)
            return getString(R.string.about_version, info.versionName ?: "?", build) + "\n" +
                   getString(R.string.about_installed, installed)
        }

        private fun openSourcePage() = openWebPage(getString(R.string.source_url))

        /**
         * Hands [url] to the phone's web browser. The app itself still never goes on the
         * internet (it has no permission to) - the browser is another app.
         */
        private fun openWebPage(url: String) {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
            try {
                startActivity(intent)
            } catch (e: ActivityNotFoundException) {
                Toast.makeText(requireContext(), R.string.no_browser, Toast.LENGTH_SHORT).show()
            }
        }

        /**
         * 14c: the privacy policy in short - readable without a network, because the text is
         * part of the app. "Open the web page" shows the full policy in the browser.
         */
        private fun showPrivacy() {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.about_privacy)
                .setMessage(getString(R.string.about_privacy_text, getString(R.string.privacy_url_short)))
                .setPositiveButton(R.string.about_privacy_open) { _, _ ->
                    openWebPage(getString(R.string.privacy_url))
                }
                .setNegativeButton(R.string.close, null)
                .show()
        }

        /** A dialog listing the licenses (names and files come from strings.xml). */
        private fun showLicenseList() {
            val names = resources.getStringArray(R.array.license_names)
            val files = resources.getStringArray(R.array.license_files)
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.about_licenses)
                .setItems(names) { _, which -> showLicense(names[which], files[which]) }
                .setNegativeButton(R.string.close, null)
                .show()
        }

        /**
         * Shows a text file from app/src/main/assets/licenses/ in a scrolling
         * dialog. License texts get their lines joined (LicenseText.unwrap) so
         * they fit the phone's width; our own NOTICES.txt is already written
         * that way and is shown as it is.
         */
        private fun showLicense(title: String, file: String) {
            val context = requireContext()
            val raw = try {
                context.assets.open("licenses/$file").bufferedReader().use { it.readText() }
            } catch (e: IOException) {
                "($file is missing)"
            }
            val text = if (file == "NOTICES.txt") raw else LicenseText.unwrap(raw)

            val padding = (20 * resources.displayMetrics.density).toInt()   // 20 dp
            val textView = TextView(context).apply {
                this.text = text
                textSize = 13f               // sp - a bit smaller than normal
                setTextIsSelectable(true)    // long-press to copy
                setPadding(padding, padding / 2, padding, 0)
            }
            MaterialAlertDialogBuilder(context)
                .setTitle(title)
                .setView(ScrollView(context).apply { addView(textView) })
                .setPositiveButton(R.string.close, null)
                .show()
        }
    }

    companion object {
        // Keys of the Storage and About entries in res/xml/settings.xml
        const val KEY_CLEAR = "clear_data"              // 13a step 2
        const val KEY_VERSION = "about_version"
        const val KEY_LICENSE = "about_license"
        const val KEY_SOURCE = "about_source"
        const val KEY_LICENSES = "about_licenses"
        const val KEY_PRIVACY = "about_privacy"         // 14c
        const val KEY_STATIONS = "about_stations"       // 13b
    }
}
