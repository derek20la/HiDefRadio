package io.github.derek20la.hidefradio

import android.app.Activity
import android.content.Context
import android.content.SharedPreferences
import android.content.res.Configuration
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.view.WindowCompat
import androidx.preference.PreferenceManager

/**
 * Milestone 9e step 2: easy access to the settings from anywhere in the app.
 *
 * The settings screen (SettingsActivity + res/xml/settings.xml) saves your
 * choices by itself in the app's "default" SharedPreferences, under the keys
 * below. These functions read them back, with the default when nothing has
 * been chosen yet.
 */
object Settings {
    const val KEY_GAIN = "gain"          // "auto", or tenths of a dB as text: "197" = 19.7 dB
    const val KEY_FOCUS = "focus_mode"   // FOCUS_MUTE / FOCUS_STOP / FOCUS_MIX
    const val KEY_BAND = "fm_band"       // FmBand.key
    const val KEY_PAUSE_STOP = "pause_stop"   // M10a: minutes as text ("5"); "0" = never
    const val KEY_HD_MODE = "hd_mode"    // M11: HD_DIGITAL / HD_ANALOG / HD_AUTO (11d)
    const val KEY_HD_MISMATCH = "hd_mismatch"   // 11d: MISMATCH_HD / MISMATCH_ANALOG
    const val KEY_THEME = "theme"        // 9h: THEME_SYSTEM / THEME_LIGHT / THEME_DARK / THEME_BLACK
    const val KEY_MEDIA_BUTTONS = "media_buttons"   // 9j: BUTTONS_ALL / BUTTONS_PLAY_PAUSE / BUTTONS_NONE
    const val KEY_FM_STEREO = "fm_stereo"   // 11b step 2: STEREO_AUTO / STEREO_MONO / STEREO_FULL

    // 11b step 2: analog FM stereo. The numbers are what RadioEngine.setStereoModeNative() takes.
    const val STEREO_AUTO = "auto"       // stereo, blended to mono as the signal gets noisy (default, like a car radio)
    const val STEREO_MONO = "mono"       // always mono - no hiss from weak (DX) stations
    const val STEREO_FULL = "stereo"     // stereo whenever the station sends it, hiss and all

    // 9j: what physical media buttons (headset, Bluetooth, car, volume-key
    // shortcuts) may do. The app's and the notification's buttons always work.
    const val BUTTONS_ALL = "all"               // pause/play and change station (default)
    const val BUTTONS_PLAY_PAUSE = "playpause"  // pause/play only - no station changes
    const val BUTTONS_NONE = "none"             // ignore them completely

    // 9h: the look of the app. "Dark gray" is Android's normal dark mode;
    // "True black" is the same with a pure black background (OLED/POLED screens
    // switch those pixels off completely).
    const val THEME_SYSTEM = "system"    // light or dark, like the phone (default)
    const val THEME_LIGHT = "light"
    const val THEME_DARK = "dark"
    const val THEME_BLACK = "black"

    // M11: what to listen to. The values RadioEngine.setAudioSourceNative() takes:
    const val HD_DIGITAL = "digital"     // the HD Radio audio only
    const val HD_ANALOG = "analog"       // the ordinary FM audio only (mono for now)
    const val HD_AUTO = "auto"           // 11d: analog at once, HD when it's good (the default, like a car radio)
    const val SOURCE_HD = 0
    const val SOURCE_ANALOG = 1
    const val SOURCE_AUTO = 2

    // 11d: in Auto, when the HD signal turns out NOT to be the station you hear on
    // analog (a distant station's HD on the same frequency - rare, but it happens):
    const val MISMATCH_HD = "hd"         // play the HD anyway (what car radios do; default)
    const val MISMATCH_ANALOG = "analog" // stay on the analog

    // What to do when another app starts playing sound:
    const val FOCUS_MUTE = "mute"        // mute, keep the tuner running, un-mute after (default)
    const val FOCUS_STOP = "stop"        // stop the radio (like pressing Stop)
    const val FOCUS_MIX = "mix"          // ignore other apps: keep playing along with them

    /**
     * The gain steps of the R820T / R828D tuner in RTL-SDR Blog V3/V4 and
     * Nooelec dongles, in tenths of a dB (librtlsdr's own list). Other tuners
     * have other steps; librtlsdr then simply uses the nearest one.
     */
    val GAIN_STEPS = intArrayOf(
        0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
        280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496)

    fun prefs(context: Context): SharedPreferences =
        PreferenceManager.getDefaultSharedPreferences(context)

    /**
     * 14b step 3: saves the DEFAULT of every setting that has never been saved.
     *
     * Why: the settings screen does exactly that the first time it is opened - it writes
     * "gain = auto", "hd_mode = auto" ... into the preferences. To RadioService that looked
     * like the listener changing all the settings at once, and "gain -> auto" restarts the
     * stream: the sound stopped for a moment on the very first visit to the settings after
     * a fresh install (found by Derek on the first release build, 2026-10-02).
     * Called before the service starts listening, so those first writes disturb nobody.
     * It never changes a setting that already has a value (true = also check settings that
     * a newer version of the app added).
     */
    fun writeDefaults(context: Context) {
        PreferenceManager.setDefaultValues(context, R.xml.settings, true)
    }

    /** Manual gain in tenths of a dB, or -1 = automatic (the default). */
    fun gainTenths(context: Context): Int =
        prefs(context).getString(KEY_GAIN, "auto")?.toIntOrNull() ?: -1

    fun focusMode(context: Context): String =
        prefs(context).getString(KEY_FOCUS, FOCUS_MUTE) ?: FOCUS_MUTE

    fun band(context: Context): FmBand =
        FmBand.fromKey(prefs(context).getString(KEY_BAND, null))

    /**
     * M10a: stop the radio after it has been paused (or taken over by another
     * app) for this many minutes. 0 = never. Default 5 (must match settings.xml).
     */
    fun pauseStopMinutes(context: Context): Int =
        prefs(context).getString(KEY_PAUSE_STOP, "5")?.toIntOrNull() ?: 5

    /** M11: the audio source for RadioEngine.setAudioSourceNative(). 11d: Auto is the default. */
    fun audioSource(context: Context): Int =
        when (prefs(context).getString(KEY_HD_MODE, HD_AUTO)) {
            HD_ANALOG -> SOURCE_ANALOG
            HD_DIGITAL -> SOURCE_HD
            else -> SOURCE_AUTO
        }

    /**
     * 9c: the audio source for this frequency: AM is always HD only (there is no
     * analog AM demodulator - Derek, 9h: blending stays FM-only), FM follows the setting.
     */
    fun audioSourceFor(context: Context, freqHz: Int): Int =
        if (FmBand.isAm(freqHz)) SOURCE_HD else audioSource(context)

    /** 11d: play an HD signal that doesn't match the analog? (for RadioEngine.setBlendUnmatchedNative) */
    fun unmatchedPlaysHd(context: Context): Boolean =
        prefs(context).getString(KEY_HD_MISMATCH, MISMATCH_HD) != MISMATCH_ANALOG

    /** 11b step 2: 0 = auto (default), 1 = always mono, 2 = always stereo - for RadioEngine.setStereoModeNative(). */
    fun stereoMode(context: Context): Int =
        when (prefs(context).getString(KEY_FM_STEREO, STEREO_AUTO)) {
            STEREO_MONO -> 1
            STEREO_FULL -> 2
            else -> 0
        }

    /** 11b step 2: the FM de-emphasis of the region, in microseconds (75 Americas, 50 elsewhere). */
    fun deemphasisUs(context: Context): Int = band(context).deemphasisUs

    /** 9j: BUTTONS_ALL (default) / BUTTONS_PLAY_PAUSE / BUTTONS_NONE. */
    fun mediaButtons(context: Context): String =
        prefs(context).getString(KEY_MEDIA_BUTTONS, BUTTONS_ALL) ?: BUTTONS_ALL

    fun theme(context: Context): String =
        prefs(context).getString(KEY_THEME, THEME_SYSTEM) ?: THEME_SYSTEM

    /**
     * 9h: tell Android whether the app is light or dark. Call it BEFORE
     * super.onCreate() in every screen. Returns true if the mode changed - then
     * AppCompat redraws the open screens by itself.
     */
    fun applyNightMode(context: Context): Boolean {
        val mode = when (theme(context)) {
            THEME_LIGHT -> AppCompatDelegate.MODE_NIGHT_NO
            THEME_DARK, THEME_BLACK -> AppCompatDelegate.MODE_NIGHT_YES
            else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
        }
        if (AppCompatDelegate.getDefaultNightMode() == mode) return false
        AppCompatDelegate.setDefaultNightMode(mode)
        return true
    }

    /**
     * 9h: "True black" - lay the black background over the dark theme. Call it
     * AFTER super.onCreate() and BEFORE setContentView() (the screen is built
     * from the theme in setContentView).
     */
    fun applyBlack(activity: Activity) {
        if (theme(activity) == THEME_BLACK) {
            activity.theme.applyStyle(R.style.ThemeOverlay_HiDefRadio_Black, true)
        }
    }

    /**
     * 9h: dark status/navigation bar icons on the light theme (so they don't
     * vanish on the white background), light icons on the dark ones. Call it
     * AFTER setContentView().
     */
    fun styleSystemBars(activity: Activity) {
        val night = (activity.resources.configuration.uiMode and
            Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
        val bars = WindowCompat.getInsetsController(activity.window, activity.window.decorView)
        bars.isAppearanceLightStatusBars = !night
        bars.isAppearanceLightNavigationBars = !night
    }
}
