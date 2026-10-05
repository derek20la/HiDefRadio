package io.github.derek20la.hidefradio

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.DialogInterface
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.InputFilter
import android.text.InputType
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TableRow
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import io.github.derek20la.hidefradio.databinding.ActivityMainBinding
import io.github.derek20la.hidefradio.databinding.ItemPresetBinding
import org.xmlpull.v1.XmlPullParser
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.roundToInt

/**
 * The screen. Since Milestone 8 the radio itself runs in RadioService, so this
 * activity only:
 *   1. finds the dongle and gets USB permission,
 *   2. starts RadioService (which opens the dongle and plays),
 *   3. (9a) lets you tune: ◀ ▶, or (9d) tap the display and type a frequency,
 *   4. (9b) shows one button per program (HD1, HD2, ...);
 *      long-press a button to give the program your own name,
 *   5. (9d) shows what's playing: a VFD-style frequency display, station,
 *      program, song, and a signal line you can tap for all the details.
 *      The old status text is still there behind "Show debug info".
 *   6. (9d step 3) shows album art, or the station logo (saved in LogoCache).
 *   7. (9e step 1) presets: a strip of tiles (tap = tune, hold = rename /
 *      delete, 13a: hold and drag = move) and a ☆ next to the station name
 *      to save one.
 *   8. (9e step 2) the ⚙ button opens the settings (SettingsActivity).
 *   9. (9e step 3) a "▶ Start radio" button while the radio is off.
 *  10. (M10a) a ⏸/▶ button. The display follows station changes made from
 *      the notification, headphones or car (⏮ ⏭).
 *      9h: ⏸/▶ and ⏹ moved to the VFD row (always on screen), like the
 *      notification's buttons; ▶ also starts the radio when it's off. A line
 *      under the station name says why it's silent (paused, headphones
 *      disconnected, another app...) or asks you to plug in the dongle.
 *      The ◀ ▶ tuning arrows became big chevrons ❮ ❯. Settings -> Theme.
 *  11. (9g) a sticky header: the VFD row and the program buttons stay put
 *      while the rest scrolls, and once the station name has scrolled away a
 *      compact "now playing" bar (logo, song, station) fades in over the top
 *      of the scrolling part. Tap it to get back to the top.
 * Everything is refreshed once a second while the screen is visible.
 * Closing this screen does NOT stop the radio - use "Stop" in the notification.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var usbManager: UsbManager

    // Dongle IDs we support, read from res/xml/device_filter.xml
    // (the same list the manifest uses). Each entry is Pair(vendorId, productId).
    private lateinit var supportedIds: Set<Pair<Int, Int>>

    private val uiHandler = Handler(Looper.getMainLooper())

    /** Once a second while the screen is visible: refresh everything. */
    private var wasRunning = false
    private val statusUpdater = object : Runnable {
        override fun run() {
            val service = RadioService.instance
            if (service != null) {
                showDebug(service.statusText())
                message = ""
                wasRunning = true
            } else if (wasRunning) {
                showMessage("Radio stopped.")     // 9e step 3: + the Start button
                wasRunning = false
            }
            refreshScreen()
            uiHandler.postDelayed(this, 1000)
        }
    }

    // ---- Milestone 9a: tuning ----
    // The frequency shown on the display (Hz). ◀ ▶ change it right away, but the
    // actual retune waits until you stop tapping for a moment (TUNE_DELAY_MS),
    // so tapping ▶ five times retunes once, not five times.
    private var shownFreqHz = RadioService.DEFAULT_FREQ_HZ
    private val delayedTune = Runnable {
        tunePending = false
        tuneTo(shownFreqHz)
    }
    // M10a: true while a ◀ ▶ tap is waiting to retune - then the display shows
    // YOUR new frequency, not the service's (which may change by ⏮ ⏭ in the
    // notification, headphones or the car).
    private var tunePending = false

    // ---- Milestone 9b: program buttons ----
    // A short "fingerprint" of the buttons on screen (which programs, their
    // names, which one is selected). The buttons are only rebuilt when it
    // changes - not every second, which would make them flicker.
    private var shownProgramsKey = ""

    // ---- Milestone 9d: now playing ----
    // A message for when the radio isn't running ("No supported dongle found...").
    private var message = ""

    // 9h: the theme this screen was drawn with (Settings -> Theme). If it
    // changed while we were away, onStart() redraws the screen.
    private var themeShown = ""
    // 9h: the status line's normal text colors (it turns amber for "plug in").
    private var hintColors: ColorStateList? = null
    // Are the signal details / debug text open? Remembered between app starts.
    private var detailsOpen = false

    // M10c step 3: "synced but decoding badly" (amber dot + "weak").
    private var lastCrcErrors = 0L          // the program's CRC error count last second
    private var crcErrorAtMs = -1L          // when it last went up (-1 = not on this station)
    private var debugOpen = false
    // 9d step 4: the station's call sign (e.g. "KRTH"), so a program name that
    // just repeats it can be replaced by the program type (Program.nameFor).
    private var station = ""

    // M12 step 2: the last Signal shown (for the ☆ and the pictures).
    private var shownSignal = Signal.NONE

    // ---- Milestone 9e step 1: presets ----
    // Like shownProgramsKey: the tiles are only rebuilt when this changes.
    private var shownPresetsKey = ""
    private var scrolledToIndex = -1     // the tile we last scrolled into view

    // ---- 13a: drag & drop of the preset tiles ----
    // Hold a tile (long-press) and it "lifts"; move your finger and it follows, the
    // other tiles slide out of the way; let go and it stays there. Let go WITHOUT
    // moving and you get the Rename / Delete menu, as before.
    // While a tile is held, the views stay in their old order - everything you see
    // is done with translationX (a view drawn shifted) - and the saved list changes
    // only at the end. So nothing is half-moved if the drag is interrupted.
    private var dragTile: View? = null   // the tile being held, null = no drag going on
    private var dragPreset: Preset? = null
    private var dragFrom = -1            // its place in the list when you picked it up
    private var dragTarget = -1          // the place it would land in right now
    private var dragPitch = 0            // one tile + the gap to the next, in pixels
    private var dragMoved = false        // the finger really moved (else: show the menu)
    private var dragSettling = false     // let go, the tile is gliding into its place
    private var dragFingerX = 0f         // where the finger is / was at the pick-up
    private var dragStartX = 0f          //   (screen coordinates, pixels)
    private var dragStartScroll = 0      // how far the strip was scrolled at the pick-up
    private var keepPresetScroll = false // after a drop: don't jump to the playing tile
    private var dragFrameMs = 0L         // when the edge-scroller last ran
    // Near the left / right edge of the strip while holding a tile -> the strip scrolls.
    private val dragScroller = object : Runnable {
        override fun run() {
            val tile = dragTile ?: return
            if (dragSettling) return
            val scroll = binding.presetScroll
            val xy = IntArray(2)
            scroll.getLocationOnScreen(xy)
            val x = dragFingerX - xy[0]                     // finger, from the strip's left edge
            val edge = dpToPx(DRAG_EDGE_DP).toFloat()
            val now = SystemClock.uptimeMillis()
            val seconds = (now - dragFrameMs).coerceIn(0L, 50L) / 1000f   // since the last frame
            dragFrameMs = now
            // The closer to the edge, the faster (up to DRAG_SCROLL_DP_PER_S).
            val push = when {
                x < edge -> -((edge - x) / edge).coerceAtMost(1f)
                x > scroll.width - edge -> ((x - (scroll.width - edge)) / edge).coerceAtMost(1f)
                else -> 0f
            }
            if (push != 0f) {
                scroll.scrollBy((push * dpToPx(DRAG_SCROLL_DP_PER_S) * seconds).roundToInt(), 0)
                moveDraggedTile()                           // the tile stays under the finger
            }
            tile.postOnAnimation(this)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        Settings.applyNightMode(this)                    // 9h: light / dark
        super.onCreate(savedInstanceState)
        Settings.applyBlack(this)                        // 9h: true black
        themeShown = Settings.theme(this)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        Settings.styleSystemBars(this)                   // 9h
        keepClearOfSystemBars()
        hintColors = binding.statusHint.textColors      // 9h

        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        supportedIds = loadSupportedIds()
        setUpTuning()
        setUpToggles()
        setUpStickyBar()                                             // 9g
        binding.presetStar.setOnClickListener { togglePreset() }   // 9e
        binding.settingsButton.setOnClickListener {                 // 9e step 2
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        binding.startButton.setOnClickListener { findAndStartRadio() }   // 9e step 3
        // M10a ⏸/▶; 9h: in the VFD row, and ▶ starts the radio when it's off.
        binding.pauseButton.setOnClickListener {
            val service = RadioService.instance
            if (service == null) findAndStartRadio() else service.togglePause()
            refreshScreen()
        }
        binding.stopButton.setOnClickListener {                          // 9h: ⏹
            RadioService.instance?.stopFromScreen()
            refreshScreen()
        }

        // Listen for our USB permission answer.
        ContextCompat.registerReceiver(
            this, permissionReceiver, IntentFilter(ACTION_USB_PERMISSION),
            ContextCompat.RECEIVER_NOT_EXPORTED)

        // Android 13+: ask once for permission to show notifications (the
        // "now playing" notification). The radio works even if you say no.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(
                this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }

        // 9h: only on a fresh start - not when the screen is redrawn (new theme,
        // phone turned), or a stopped radio would start again by itself.
        if (savedInstanceState == null) findAndStartRadio()
    }

    // Called instead of onCreate when the app is already open and Android
    // sends it a new "dongle plugged in" event (because of launchMode="singleTop").
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            findAndStartRadio()
        }
    }

    // Update the screen only while it's visible (saves battery).
    override fun onStart() {
        super.onStart()
        // 9h: back from Settings with another theme -> draw the screen again.
        if (themeShown != Settings.theme(this)) {
            recreate()
            return
        }
        uiHandler.post(statusUpdater)
    }

    override fun onStop() {
        cancelDrag()                                     // 13a: a held tile goes back
        uiHandler.removeCallbacks(statusUpdater)
        super.onStop()
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(delayedTune)
        unregisterReceiver(permissionReceiver)
        // NOTE: we do NOT stop the radio here - RadioService keeps playing.
        super.onDestroy()
    }

    /**
     * Android 15+ draws the app edge-to-edge: our screen continues underneath
     * the status bar, the camera cutout and the navigation bar. Android tells us
     * how big those are ("insets"); we add that much padding around our layout,
     * on top of the 12dp padding it already has.
     */
    private fun keepClearOfSystemBars() {
        val root = binding.root
        val left = root.paddingLeft      // the 12dp from activity_main.xml
        val top = root.paddingTop
        val right = root.paddingRight
        val bottom = root.paddingBottom
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(left + bars.left, top + bars.top, right + bars.right, bottom + bars.bottom)
            insets
        }
    }

    // ------------------------------------------------------------------ tuning (9a, 9d)

    /** Connects ◀ ▶ and the display (tap = type a frequency). */
    private fun setUpTuning() {
        shownFreqHz = RadioService.loadFrequency(this)   // last station
        showFrequency(shownFreqHz)

        binding.tuneDown.setOnClickListener { step(up = false) }
        binding.tuneUp.setOnClickListener { step(up = true) }
        binding.vfdPanel.setOnClickListener { showFrequencyDialog() }
    }

    /**
     * ◀ or ▶: the next channel of your FM band (9e: a setting - Americas =
     * 0.2 MHz steps, 87.9 ... 107.9, wrapping around at the ends; see FmBand).
     * 9i: Americas also has 87.75 (TV channel 6 audio) below 87.9.
     */
    private fun step(up: Boolean) {
        val hz = Settings.band(this).step(shownFreqHz, up)
        shownFreqHz = hz
        showFrequency(hz)
        uiHandler.removeCallbacks(delayedTune)          // restart the wait
        uiHandler.postDelayed(delayedTune, TUNE_DELAY_MS)
        tunePending = true
    }

    /**
     * 9d: tapping the display opens a small window to type a frequency
     * (this replaces 9a's text box + Tune button). The keyboard's "Go" key
     * works as well as the Tune button.
     */
    private fun showFrequencyDialog() {
        val input = EditText(this)
        input.setSingleLine()
        input.inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        input.imeOptions = EditorInfo.IME_ACTION_GO
        input.filters = arrayOf<InputFilter>(InputFilter.LengthFilter(6))   // "101.1", 9i: "87.75", 9c: "1260"
        input.setText(RadioService.formatMhz(shownFreqHz))
        input.setSelectAllOnFocus(true)
        input.hint = "101.1"

        // 9c (Derek's choice): FM and AM buttons under the box - each goes back to
        // the last frequency you had on that band. The band you're on is the filled one.
        val lastFm = RadioService.lastFrequency(this, am = false)
        val lastAm = RadioService.lastFrequency(this, am = true)
        val onAm = FmBand.isAm(shownFreqHz)
        fun bandButton(hz: Int, current: Boolean): MaterialButton {
            val b = if (current) MaterialButton(this)
                    else MaterialButton(this, null, com.google.android.material.R.attr.materialButtonOutlinedStyle)
            b.text = FmBand.bandText(hz).split(' ').reversed().joinToString(" ")   // "FM 101.1" / "AM 1260"
            b.isAllCaps = false
            return b
        }
        val fmButton = bandButton(if (onAm) lastFm else shownFreqHz, current = !onAm)
        val amButton = bandButton(if (onAm) shownFreqHz else lastAm, current = onAm)
        val buttons = LinearLayout(this)
        buttons.orientation = LinearLayout.HORIZONTAL
        val gap = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        gap.marginEnd = dpToPx(8)
        buttons.addView(fmButton, gap)
        buttons.addView(amButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        // Same side margins as the dialog's text.
        val box = LinearLayout(this)
        box.orientation = LinearLayout.VERTICAL
        box.setPadding(dpToPx(24), dpToPx(8), dpToPx(24), 0)
        box.addView(input)
        box.addView(buttons)

        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.enter_frequency)
            .setMessage(R.string.enter_frequency_message)
            .setView(box)
            .setPositiveButton(R.string.tune, null)    // set below, so a bad value keeps the window open
            .setNegativeButton(android.R.string.cancel, null)
            .create()

        // Try to tune; close the window only if the frequency was OK.
        val tryTune = {
            if (tuneToTypedFrequency(input.text.toString())) dialog.dismiss()
        }
        dialog.setOnShowListener {
            dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener { tryTune() }
        }
        // 9c: a band button tunes straight away (to its frequency) and closes the window.
        fun jump(hz: Int) {
            if (tuneToTypedFrequency(FmBand.freqText(hz))) dialog.dismiss()
        }
        fmButton.setOnClickListener { jump(if (onAm) lastFm else shownFreqHz) }
        amButton.setOnClickListener { jump(if (onAm) shownFreqHz else lastAm) }
        input.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_GO) { tryTune(); true } else false
        }
        // Open the keyboard together with the window.
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
        dialog.show()
        input.requestFocus()
    }

    /** Checks a typed frequency and tunes to it. Returns false if it's not valid. */
    private fun tuneToTypedFrequency(text: String): Boolean {
        // "105.1" -> 105.1 MHz, 9c: "1260" -> 1260 kHz (AM). A comma works too ("105,1").
        // 9e: the allowed range depends on your band (region) setting; see FmBand.parseTyped.
        val band = Settings.band(this)
        val hz = band.parseTyped(text)
        if (hz < 0) {
            Toast.makeText(this, getString(R.string.bad_frequency, band.rangeText(),
                                           band.amRangeText(), band.amStepKhz()),
                           Toast.LENGTH_LONG).show()
            return false
        }
        uiHandler.removeCallbacks(delayedTune)          // a typed value beats a pending ◀ ▶
        tunePending = false
        shownFreqHz = hz
        showFrequency(hz)
        tuneTo(hz)
        return true
    }

    /**
     * Remember the frequency and, if the radio is playing, retune it.
     * 9e: [program] = the program to play (a preset can be HD2); ◀ ▶ and typed
     * frequencies always start on HD1.
     */
    private fun tuneTo(hz: Int, program: Int = 0) {
        val service = RadioService.instance
        if (service != null) {
            service.tune(hz, program)                   // also saves both
        } else {
            RadioService.saveFrequency(this, hz)        // used when the radio starts
            RadioService.saveProgram(this, program)
        }
        refreshScreen()
    }

    /**
     * 9d: shows a frequency on the VFD. 101_100_000 -> "101.1", 99_100_000 ->
     * "!99.1" ("!" = a blank digit in the DSEG7 font, so the digits stay lined
     * up with the dim "888.8" behind them).
     * 9c: AM shows kHz, four digits and no point: "1260", " 740" ("!740").
     */
    private fun showFrequency(hz: Int) {
        val text = RadioService.formatMhz(hz)
        if (FmBand.isAm(hz)) {
            binding.vfdFreq.text = text.padStart(4, '!')
            binding.vfdGhost.text = "8888"
            binding.vfdUnit.setText(R.string.khz)
            return
        }
        binding.vfdFreq.text = text.padStart(5, '!')
        // 9i: "87.75" has its point one digit further left -> so must the dim
        // unlit segments behind it ("88.88" instead of "888.8").
        binding.vfdGhost.text = if (text.length - text.indexOf('.') == 3) "88.88" else "888.8"
        binding.vfdUnit.setText(R.string.mhz)
    }

    // ------------------------------------------------------------------ now playing (9d)

    /** Updates everything on the screen from the radio service. */
    private fun refreshScreen() {
        val service = RadioService.instance
        val programs = service?.programs() ?: emptyList()      // radio off -> none
        val selected = service?.currentProgram() ?: -1
        val freqHz = service?.freqHz ?: shownFreqHz
        val signal = service?.signal() ?: Signal.NONE
        station = signal.station                               // 9d step 4: for program names
        shownSignal = signal                                   // M12 step 2

        // M10a: the station may have been changed from outside the app (⏮ ⏭ in
        // the notification, headphones, car) -> show it on the display too.
        if (service != null && !tunePending) {
            val hz = service.displayFreqHz()
            if (hz != shownFreqHz) {
                shownFreqHz = hz
                showFrequency(hz)
            }
        }

        updateProgramButtons(programs, selected, freqHz)
        updatePresets(programs, if (selected >= 0) selected else RadioService.loadProgram(this), freqHz)
        updateNowPlaying(service != null, programs, selected, freqHz, signal)
        updateSignal(service != null, signal)
        updatePictures(service, selected, freqHz)
        // 9e step 3: radio off -> offer to start it again.
        binding.startButton.visibility = if (service == null) View.VISIBLE else View.GONE
        updatePauseButton(service)
        updateSticky(binding.scroll.scrollY)                    // 9g
    }

    /**
     * M10a: ⏸ while sound is playing, ▶ while paused (or another app has the sound).
     * 9h: radio off -> ▶ (= Start radio) and a dimmed ⏹. Also the status line.
     */
    private fun updatePauseButton(service: RadioService?) {
        val playing = service?.isPlaying() == true
        binding.pauseButton.setImageResource(if (playing) R.drawable.ic_btn_pause else R.drawable.ic_btn_play)   // 9i: cropped icons
        binding.pauseButton.contentDescription = getString(when {
            service == null -> R.string.start_radio_icon
            playing -> R.string.pause
            else -> R.string.play
        })
        binding.stopButton.isEnabled = service != null
        binding.stopButton.alpha = if (service != null) 1f else 0.3f
        updateStatusHint(service)
    }

    /**
     * 9h: the line under the station name.
     *   radio off, no dongle plugged in -> "⚠ Plug in the RTL-SDR" (amber)
     *   paused / muted -> why, and when the auto-stop will turn the radio off
     *   otherwise hidden
     */
    private fun updateStatusHint(service: RadioService?) {
        val hint = binding.statusHint
        if (service == null) {
            val dongle = usbManager.deviceList.values.any { d ->
                Pair(d.vendorId, d.productId) in supportedIds
            }
            if (dongle) {
                hint.visibility = View.GONE
            } else {
                hint.text = getString(R.string.hint_plug_in)
                hint.setTextColor(color(R.color.signal_holding))
                hint.visibility = View.VISIBLE
            }
            return
        }
        val reason = when (service.silentReason()) {
            RadioService.SILENT_PAUSED -> R.string.hint_paused
            RadioService.SILENT_UNPLUGGED -> R.string.hint_paused_unplugged
            RadioService.SILENT_OTHER_APP -> R.string.hint_muted_other_app
            RadioService.SILENT_INTERRUPTED -> R.string.hint_muted_interrupted
            else -> 0
        }
        if (reason == 0) {
            hint.visibility = View.GONE
            return
        }
        var text = getString(reason)
        val stopsInMs = service.stopsInMs()
        if (stopsInMs >= 0) {
            val minutes = maxOf(1, ceil(stopsInMs / 60_000.0).toInt())
            text += "\n" + getString(R.string.hint_stops_in, minutes)
        }
        hint.text = text
        hintColors?.let { hint.setTextColor(it) }
        hint.visibility = View.VISIBLE
    }

    // 9d step 3: what the pictures on screen were made from, so we only decode
    // them again when something changed (new picture, other program, retune).
    private var shownPicturesKey = ""

    /**
     * 9d step 3: the big picture = album art, else the program's logo, else the
     * logo saved from last time (LogoCache), else nothing. The small logo next
     * to the station name only shows while album art has the big spot.
     */
    private fun updatePictures(service: RadioService?, selected: Int, freqHz: Int) {
        // Radio off -> the saved logo of the program you listened to last (9e).
        val program = if (selected >= 0) selected else RadioService.loadProgram(this)
        // M12 step 2: hearing the analog while the HD here is ANOTHER station's -> that
        // station's logo and album art would be the wrong pictures: none.
        val otherAnalog = service != null && shownSignal.otherHd && shownSignal.analog
        val key = "${service?.pictureGen()}/$program/$freqHz/$otherAnalog"
        if (key == shownPicturesKey) return
        shownPicturesKey = key

        val art = if (otherAnalog) null else service?.art(program)?.let { decode(it) }
        val logo = if (otherAnalog) null else
            (service?.logo(program) ?: LogoCache.load(this, freqHz, program))?.let { decode(it) }

        val big = art ?: logo
        binding.nowImage.setImageBitmap(big)
        binding.nowImage.visibility = if (big != null) View.VISIBLE else View.GONE

        val small = if (art != null) logo else null
        binding.stationLogo.setImageBitmap(small)
        binding.stationLogo.visibility = if (small != null) View.VISIBLE else View.GONE

        // 9g: the sticky bar always shows the logo (never the album art - the
        // bar is about WHAT you're listening to, and art changes every song).
        binding.stickyLogo.setImageBitmap(logo)
        binding.stickyLogo.visibility = if (logo != null) View.VISIBLE else View.GONE
    }

    /** PNG/JPEG bytes -> a picture Android can show (null if the file is damaged). */
    private fun decode(bytes: ByteArray): Bitmap? =
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)

    /** Station, program and song. */
    private fun updateNowPlaying(
        running: Boolean, programs: List<Program>, selected: Int, freqHz: Int, signal: Signal,
    ) {
        // M12 step 2: show what RDS says when there is no HD at all (an analog-only station,
        // or the HD hasn't arrived yet) - and also while you hear the analog of a frequency
        // whose HD is ANOTHER station's (the HD's name, program and song aren't this one's).
        val otherAnalog = running && signal.otherHd && signal.analog
        val rdsMode = running && ((signal.station.isEmpty() && programs.isEmpty()) || otherAnalog)

        // Station: the call sign from the station, or the frequency until it arrives.
        // M12: no HD name -> the name RDS gives.
        binding.stationName.text = if (otherAnalog) {
            signal.rdsName.ifEmpty { signal.hdSameCall }.ifEmpty { FmBand.bandText(freqHz) }
        } else signal.station.ifEmpty {
            signal.rdsName.ifEmpty { FmBand.bandText(freqHz) }                           // 9c: "1260 AM"
        }

        // Program: "HD1 · Adult Hits" (your name for it first, see 9b step 3b).
        // M12 step 2: an RDS station -> "FM · Religious talk" (its program type), if it sends one.
        val program = if (rdsMode) null else programs.firstOrNull { it.number == selected }
        if (rdsMode) {
            setTextOrHide(binding.programName, rdsTypeLine(signal))
        } else if (program != null) {
            val label = ProgramNames.label(this, freqHz, program, station)
            setTextOrHide(binding.programName,
                if (label.isEmpty()) program.hdName else "${program.hdName} · $label")
        } else {
            setTextOrHide(binding.programName, "")
        }

        // Song. No song yet -> the slogan (if it isn't just the call sign again).
        // Radio off -> the message, e.g. "No supported dongle found".
        when {
            !running -> {
                setTextOrHide(binding.songTitle, message)
                setTextOrHide(binding.songArtist, "")
            }
            program != null && (program.title.isNotBlank() || program.artist.isNotBlank()) -> {
                setTextOrHide(binding.songTitle, program.title)
                setTextOrHide(binding.songArtist, program.artist)
            }
            else -> {
                val slogan = if (signal.slogan != signal.station) signal.slogan else ""
                // M12: an RDS station -> what it says over RDS: title + artist if it tags
                // them (RT+), else its RadioText. (Never the other station's HD slogan.)
                setTextOrHide(binding.songTitle, if (rdsMode) signal.rdsLine1 else slogan)
                setTextOrHide(binding.songArtist, if (rdsMode) signal.rdsLine2 else "")
            }
        }

        // 9g: the sticky bar repeats all that in two short lines:
        //   "If You Leave – OMD"          (song, or the slogan while there's no song)
        //   "KRTH · HD1 · Adult Hits"     (station · program)
        // No song and no slogan -> the station line moves up, second line hidden.
        val song = if (program != null) {
            listOf(program.title, program.artist).filter { it.isNotBlank() }.joinToString(" – ")
        } else ""
        val slogan = if (!rdsMode && signal.slogan != signal.station) signal.slogan else ""
        val where = listOfNotNull(
            binding.stationName.text.toString(),
            program?.hdName,
            program?.let { ProgramNames.label(this, freqHz, it, station) },
            if (rdsMode) Rds.ptyName(signal.rdsPty, rbds()) else null                   // M12 step 2
        ).filter { it.isNotBlank() }.joinToString(" · ")
        val rdsSong = if (rdsMode)                                                       // M12
            listOf(signal.rdsLine1, signal.rdsLine2).filter { it.isNotBlank() }.joinToString(" – ") else ""
        val line1 = song.ifBlank { slogan }.ifBlank { rdsSong }
        if (line1.isNotBlank()) {
            setTextOrHide(binding.stickySong, line1)
            setTextOrHide(binding.stickyStation, where)
        } else {
            setTextOrHide(binding.stickySong, where)
            setTextOrHide(binding.stickyStation, "")
        }
    }

    /**
     * M10c step 3: why a synced station is "weak", or null if it's fine.
     * Synced only means nrsc5 found the HD signal; the sound can still be
     * missing or break up (KLLI at BER 18 %: green dot, no audio). So:
     *  - damaged audio packets (CRC errors) in the last [WEAK_HOLD_MS], or
     *  - synced for [WEAK_NO_AUDIO_MS] and still no audio at all.
     * (Not BER alone: KYSR plays perfectly at BER 14 % with one sideband gone.)
     */
    private fun weakReason(s: Signal): String? {
        // CRC errors only count up; a smaller number = another station or program
        // (while retuning the count is 0) -> start over.
        val now = SystemClock.elapsedRealtime()
        if (s.crcErrors > lastCrcErrors) crcErrorAtMs = now
        else if (s.crcErrors < lastCrcErrors) crcErrorAtMs = -1L
        lastCrcErrors = s.crcErrors
        if (!s.synced || s.holding) return null
        if (crcErrorAtMs >= 0 && now - crcErrorAtMs < WEAK_HOLD_MS) return "audio errors"
        // 9c: AM's first audio comes ~6-7 s after sync (longer interleaver) -> wait longer there.
        val noAudioMs = if (s.am) WEAK_NO_AUDIO_AM_MS else WEAK_NO_AUDIO_MS
        if (s.audioMs < 0 && s.syncMs >= 0 && s.tunedMs - s.syncMs > noAudioMs) return "no audio yet"
        return null
    }

    /** The signal line (always) and the details (when opened). */
    private fun updateSignal(running: Boolean, s: Signal) {
        val weak = if (running) weakReason(s) else null          // M10c step 3
        // The VFD's little "HD" lights up while synced.
        binding.vfdHd.setTextColor(color(if (s.synced) R.color.vfd_on else R.color.vfd_ghost))

        val (dotColor, text) = when {
            !running -> R.color.signal_off to getString(R.string.signal_off)
            // 11d: Auto, in the middle of a crossfade (0.5 s). Towards analog = something
            // is wrong ahead in the HD (amber, like "weak").
            s.auto && s.blendState == "toHd" -> R.color.signal_good to getString(R.string.signal_blending_hd)
            s.auto && s.blendState == "toAnalog" -> R.color.signal_holding to
                getString(R.string.signal_blending_analog) + "  (${s.blendReason})"
            // M11: analog - the dot shows the FM carrier; the HD state follows in brackets.
            // (11d: also Auto while it plays the analog.)
            s.analog -> analogLine(s)
            !s.everSynced -> R.color.signal_off to
                getString(if (s.am) R.string.signal_searching_am else R.string.signal_searching,
                          String.format(Locale.US, "%.1f s", s.tunedMs / 1000.0))
            !s.synced -> R.color.signal_lost to getString(R.string.signal_lost)
            // M10c step 3: weak -> amber (like "holding") and the word "weak".
            // 9c: AM sends no MER - BER only.
            s.am -> (if (s.holding || weak != null) R.color.signal_holding else R.color.signal_good) to
                String.format(Locale.US, "HD %s  %sBER %s",
                    s.mode, if (weak != null) getString(R.string.signal_weak) + "  " else "",
                    Signal.percent(s.ber))
            else -> (if (s.holding || weak != null) R.color.signal_holding else R.color.signal_good) to
                String.format(Locale.US, "HD %s  %sMER %.1f / %.1f  BER %s",
                    s.mode, if (weak != null) getString(R.string.signal_weak) + "  " else "",
                    s.merLower, s.merUpper, Signal.percent(s.ber))
        }
        // 9e step 3: too much gain -> a warning sign on the line too.
        val line = if (running && s.peakDbfs > OVERLOAD_DBFS) "$text  ⚠" else text
        binding.signalDot.setTextColor(color(dotColor))
        binding.signalLine.text = line

        // M12 step 2: the HD here is ANOTHER station's (RDS PI vs the HD's call sign) -
        // say so under the signal line; tap = listen to the other one (RadioService).
        val other = running && s.otherHd
        if (other) {
            binding.otherHdNotice.text = when {
                s.analog -> getString(R.string.other_hd_listen, s.hdOtherName.ifEmpty { s.station })
                s.hdSameCall.isNotEmpty() -> getString(R.string.other_analog_listen, s.hdSameCall)
                else -> getString(R.string.other_analog_listen_unnamed)
            }
        }
        binding.otherHdNotice.visibility = if (other) View.VISIBLE else View.GONE

        binding.signalArrow.text = if (detailsOpen) "▴" else "▾"
        binding.signalTable.visibility = if (detailsOpen) View.VISIBLE else View.GONE
        if (detailsOpen) showDetails(signalDetails(running, s, weak))
    }

    /**
     * M11: the signal line while the analog demodulator plays.
     * 9k: judged by FM QUIETING, not by the carrier level. The carrier level
     * depends on the tuner gain, which the strongest station in the band sets
     * (KLLI on the stubby antenna played fine at -35 dB and was called "no
     * station"; so was XHITZ 90.3 from Tijuana). Quieting = how much the station
     * silences the noise where it sends nothing (60-100 kHz of the MPX), shown
     * as a 5-bar S-meter (Derek's choice). The HD state is still shown, in brackets.
     */
    private fun analogLine(s: Signal): Pair<Int, String> {
        val measuring = s.fmQuietDb <= -98f
        val hd = when {
            !s.hdOn -> getString(R.string.signal_hd_off)            // 12a step 3: Analog only
            !s.everSynced -> "no HD yet"
            !s.synced -> "HD lost"
            // 11d: the HD1 never lines up with the analog = another station's HD (rare).
            s.blendMismatch -> getString(R.string.signal_hd_mismatch)
            else -> String.format(Locale.US, "HD %s MER %.1f / %.1f", s.mode, s.merLower, s.merUpper)
        }
        val quiet = -s.fmQuietDb                                    // 9k: positive, higher = cleaner
        val bars = QUIET_BARS_DB.count { quiet >= it }              // 0..5
        val meter = "▮".repeat(bars) + "▯".repeat(QUIET_BARS_DB.size - bars)
        // 11b: the classic "ST" of a tuner display, once at least half the stereo is playing
        // (the blend takes it away gradually as the signal gets noisy; no pilot = nothing).
        val st = (if (s.fmStereoPct >= 50) " ${getString(R.string.signal_stereo)}" else "") +
                 // M12 step 2: and the "RDS" light, once the station is identified and the data flows.
                 (if (s.rdsPi.isNotEmpty() && s.rdsSync) " ${getString(R.string.signal_rds)}" else "")
        val level = when {
            measuring -> "…"
            quiet < QUIET_NOISE_DB -> "$meter ${getString(R.string.signal_noise)}"
            quiet < QUIET_WEAK_DB -> "$meter ${getString(R.string.signal_weak)}$st"
            else -> "$meter$st"
        }
        // An overloaded dongle can look like a strong, silent station (87.9 test,
        // 21 % of the samples at the rails read "very quiet") -> never green then.
        val overload = s.peakDbfs > OVERLOAD_DBFS
        val color = when {
            measuring -> R.color.signal_off
            quiet < QUIET_NOISE_DB -> R.color.signal_lost
            quiet < QUIET_WEAK_DB || overload -> R.color.signal_holding
            else -> R.color.signal_good
        }
        return color to "${getString(R.string.signal_analog)}  $level  ($hd)"
    }

    /**
     * 9d step 4: puts the details in a two-column table (name | value), so
     * they line up on any font. (Step 2 used spaces + "monospace", but the
     * Moto's font isn't really monospace, so the columns came out ragged.)
     * The rows are made once and then only their text changes.
     */
    private fun showDetails(rows: List<Pair<String, String>>) {
        val table = binding.signalTable
        if (table.childCount != rows.size) {
            table.removeAllViews()
            for (i in rows.indices) {
                val name = TextView(this)
                name.textSize = 12f
                name.alpha = 0.7f                                   // a bit dimmer than the values
                name.setPadding(0, 0, dpToPx(16), dpToPx(3))
                val value = TextView(this)
                value.textSize = 12f
                value.setPadding(0, 0, 0, dpToPx(3))
                val row = TableRow(this)
                row.addView(name)
                row.addView(value)
                table.addView(row)
            }
        }
        val warning = getString(R.string.overload)
        val warningAuto = getString(R.string.overload_auto)       // M10c
        val warningDirect = getString(R.string.overload_direct)   // 14c
        for ((i, pair) in rows.withIndex()) {
            val row = table.getChildAt(i) as TableRow
            (row.getChildAt(0) as TextView).text = pair.first
            val value = row.getChildAt(1) as TextView
            value.text = pair.second
            // 9e step 3: a warning ("overload!") in red, everything else normal.
            val red = pair.second.contains(warning) || pair.second.contains(warningAuto) ||
                      pair.second.contains(warningDirect)
            value.setTextColor(if (red) color(R.color.signal_lost)
                               else binding.signalLine.currentTextColor)
        }
    }

    /** All the numbers, nrsc5-dui style: (name, value) pairs for the table. */
    private fun signalDetails(running: Boolean, s: Signal, weak: String?): List<Pair<String, String>> {
        if (!running) return listOf("Radio" to "off")
        fun f(format: String, vararg args: Any) = String.format(Locale.US, format, *args)
        // M10c: auto also says how it found its start: the gain remembered for
        // this station (no search), or a search (and how long that took).
        val gain = when {
            // 14c: a V3-type dongle on AM feeds the antenna straight into its converter
            // ("direct sampling") - the tuner and its gain are not in the signal path.
            s.direct -> getString(R.string.gain_direct)
            s.gainDb < 0 -> "tuner AGC"
            !s.gainAuto -> f("%.1f dB (manual)", s.gainDb)
            s.gainRemembered -> f("%.1f dB (auto, remembered)", s.gainDb)
            s.gainSearchMs >= 0 -> f("%.1f dB (auto, searched in %d ms)", s.gainDb, s.gainSearchMs)
            else -> f("%.1f dB (auto)", s.gainDb)
        }
        val overload = getString(when {
            s.direct -> R.string.overload_direct           // 14c: no gain to lower
            s.gainAuto -> R.string.overload_auto
            else -> R.string.overload
        })
        val kbps = if (s.kbps > 0) f("%.1f kbps", s.kbps) else "-"
        val state = when {
            s.holding -> "holding (sync lost a moment ago)"
            s.locked -> "synced" + (if (s.mode.isNotEmpty()) ", mode ${s.mode}" else "") +
                        (if (weak != null) " - weak ($weak)" else "")        // M10c step 3
            s.everSynced -> "lost"
            else -> "searching"
        }
        return stationRow(s) + listOf(                                      // 13b
            "Sync" to state,
            "MER" to if (s.am) "- (AM sends none; see BER)"                 // 9c
                     else f("lower %.1f dB, upper %.1f dB", s.merLower, s.merUpper),
            "BER" to "now ${Signal.percent(s.ber, 4)}, avg ${Signal.percent(s.berAvg, 4)}\n" +
                     "min ${Signal.percent(s.berMin, 4)}, max ${Signal.percent(s.berMax, 4)}",
            "Audio" to "$kbps, CRC errors ${s.crcErrors}",
        ) + analogRow(s) + rdsRow(s) + alignRow(s) + blendRow(s) + listOf(
            "Gain" to gain,
        ) + watchdogRow(s) + listOf(
            // 9e step 3: near 0 dBFS the dongle's converter is at its limit ("overload"):
            // the signal gets distorted and HD is lost. Harmless, but lower the gain.
            // (M10c: on auto, the watchdog lowers it by itself.)
            "Peak" to f("%.1f dBFS", s.peakDbfs) +
                if (s.peakDbfs > OVERLOAD_DBFS) " - $overload" else "",
            "Offset" to "${s.offsetHz} Hz",
            // M10b: + "sound" = from asking for the station until you hear it
            // (includes stopping the old station, auto-gain and the pre-buffer).
            "Tuning" to "sync after ${Signal.seconds(s.syncMs)}, audio after ${Signal.seconds(s.audioMs)}\n" +
                        "sound after ${Signal.seconds(RadioService.instance?.soundMs() ?: -1L)}",
        )
    }

    /**
     * 13b: who the station is, from the built-in FCC list (Stations.kt):
     *   "KGB-FM · San Diego, CA
     *    50 kW, class B, HAAT 150 m, HD"
     * (HAAT = antenna height above average terrain; HD = the FCC has an HD Radio
     * notification on file.) The station is looked up by its call letters - from
     * HD Radio or from RDS - among the stations licensed on this frequency. A call
     * from RDS that is licensed on ANOTHER frequency is a translator or booster
     * carrying that station:
     *   "KUSC · Los Angeles, CA
     *    licensed on 91.5 FM - relayed here"
     * No row until call letters are known.
     */
    private fun stationRow(s: Signal): List<Pair<String, String>> {
        val freqHz = RadioService.instance?.freqHz ?: shownFreqHz
        // Whose call letters? The station you HEAR: where the HD belongs to another
        // station than the analog (Signal.otherHd), the analog's while the analog plays.
        val fromRds = listOf(s.rdsCall, s.hdSameCall).filter { it.isNotBlank() }
        val names = if (s.otherHd && s.analog) fromRds else listOf(s.station).filter { it.isNotBlank() } + fromRds
        for (name in names) {
            val st = Stations.find(this, freqHz, name) ?: continue
            val facts = listOf(
                st.power, st.kind,
                if (st.haatM > 0) "HAAT ${st.haatM} m" else "",
                when (st.directional) { "d" -> "directional by day"; "n" -> "directional at night"; "dn" -> "directional"; else -> "" },
                if (st.hd) "HD" else "",
            ).filter { it.isNotEmpty() }.joinToString(", ")
            return listOf("Station" to "${st.call} · ${st.place}" + if (facts.isEmpty()) "" else "\n$facts")
        }
        // Not licensed here. Only for a call worked out from the RDS code (always real call
        // letters) - an HD NAME that isn't licensed here may be just a name ("WAVE").
        for (name in fromRds) {
            val st = Stations.elsewhere(this, freqHz, name) ?: continue
            return listOf("Station" to "${st.call} · ${st.place}\nlicensed on ${FmBand.bandText(st.freqHz)} - relayed here")
        }
        return emptyList()
    }

    /**
     * M11: the analog demodulator's numbers:
     * "carrier -10 dB, offset -19 Hz, demod 8 % CPU".
     * (11c: the demodulator now runs in every mode, so the row is always there -
     * the carrier level is a handy signal meter for analog-only stations.)
     */
    private fun analogRow(s: Signal): List<Pair<String, String>> {
        // 11d: in Digital only the demodulator is off (saves CPU) - say so.
        if (s.am) return listOf("Analog" to "off (AM is HD only)")          // 9c
        if (!s.demodOn) return listOf("Analog" to "off (Digital only)")
        // 9k: quieting first (what the S-meter bars show), then the carrier level.
        val quiet = if (s.fmQuietDb <= -98f) "quieting measuring…"
                    else String.format(Locale.US, "quieting %.1f dB", -s.fmQuietDb)
        val carrier = if (s.fmCarrierDb <= -98f) "$quiet, carrier measuring…"
                      else String.format(Locale.US, "%s, carrier %.1f dB", quiet, s.fmCarrierDb)
        // 11b: stereo - what plays and why. "stereo 100 %, pilot 9.1 %" in full stereo;
        // the % drops as the blend takes stereo away on a noisy signal.
        val stereo = when {
            s.fmStereoMode == "mono" -> "mono (always mono)"
            !s.fmPilot -> "mono (no pilot)"
            s.fmStereoPct == 0 -> String.format(Locale.US, "mono (blended - noisy), pilot %.1f %%", s.fmPilotPct)
            else -> String.format(Locale.US, "stereo %d %%, pilot %.1f %%", s.fmStereoPct, s.fmPilotPct)
        }
        // 11d: the HD decode's own CPU time next to the demodulator's, for comparison.
        return listOf("Analog" to "$carrier, offset ${s.fmOffsetHz} Hz\n" +
                                  "$stereo\n" +
                                  "demod ${s.fmLoadPct} % CPU, HD decode " +
                                  if (s.hdOn) "${s.hdLoadPct} %" else "off")          // 12a step 3
    }

    /**
     * M12: what the analog station sends over RDS:
     *   "KKLA (PI 2B86), Religious talk, errors 0 %
     *    PS "HERE."
     *    99.5 Find Hope Here! KKLA"
     * Nothing while the demodulator is off (the Analog row says so already).
     */
    private fun rdsRow(s: Signal): List<Pair<String, String>> {
        if (s.am || !s.demodOn) return emptyList()
        if (s.rdsPi.isEmpty()) return listOf("RDS" to if (s.rdsSync) "found, identifying…" else "none (yet)")
        val who = if (s.rdsCall.isNotEmpty()) "${s.rdsCall} (PI ${s.rdsPi})" else "PI ${s.rdsPi}"
        val type = Rds.ptyName(s.rdsPty, rbds())
        val lines = mutableListOf(
            who + (if (type.isNotEmpty()) ", $type" else "") +
                if (s.rdsSync) ", errors ${s.rdsBler} %" else ", lost"
        )
        if (s.rdsPs.isNotBlank()) lines += "PS \"${s.rdsPs}\""
        if (s.rdsTitle.isNotBlank()) lines += "♪ " + listOf(s.rdsTitle, s.rdsArtist).filter { it.isNotBlank() }.joinToString(" – ")
        if (s.rdsRt.isNotBlank()) lines += s.rdsRt
        return listOf("RDS" to lines.joinToString("\n"))
    }

    /**
     * 11d: what the blend (Auto) is doing:
     *   "HD for 42 s, analog -4.2 dB
     *    to HD 3×, to analog 2×, HD 250 s in all, last: dropout ahead"
     * or "analog - the HD is another station's", "sub-channel plays live"...
     */
    private fun blendRow(s: Signal): List<Pair<String, String>> {
        if (!s.auto) return emptyList()
        val what = when (s.blendState) {
            "sub" -> "sub-channel plays live (no analog to blend with)"
            "hd" -> "HD" + (if (s.blendAligned) "" else " (unaligned)") + " for ${s.blendSinceSecs} s"
            "toHd" -> "fading to HD"
            "toAnalog" -> "fading to analog"
            else -> "analog" + (if (s.blendMismatch) " - the HD is another station's" else "")
        }
        val level = if (s.blendMatchDb != 0f) String.format(Locale.US, ", analog %+.1f dB", s.blendMatchDb) else ""
        val history = if (s.blendToHd == 0) "" else
            "\nto HD ${s.blendToHd}×, to analog ${s.blendToAnalog}×, HD ${s.blendHdSecs} s in all" +
            (if (s.blendReason.isNotEmpty()) ", last: ${s.blendReason}" else "")
        return listOf("Blend" to what + level + history)
    }

    /**
     * 11c: how far the analog audio lags the HD1 audio, and how much louder it
     * is - what the blend (11d) will need:
     *   "analog lags HD1 by 2485.1 ms (r 0.96), analog -0.2 dB
     *    5 measurements"
     * Before the first result: "waiting for HD audio" / "measuring…", or
     * "no match" when the two streams don't correlate (e.g. HD1 and analog
     * carrying different programs).
     */
    private fun alignRow(s: Signal): List<Pair<String, String>> {
        fun f(format: String, vararg args: Any) = String.format(Locale.US, format, *args)
        val text = when {
            s.am -> "off (AM is HD only)"                          // 9c
            !s.demodOn -> "off (Digital only)"                     // 11d
            s.alignState == "waiting" -> "waiting for HD audio"
            s.alignState == "nomatch" -> f("no match (r %.2f, %d tries)", s.alignCorr, s.alignTries)
            s.alignCount == 0 -> "measuring…"
            else -> {
                val lag = if (s.alignMs >= 0) f("analog lags HD1 by %.1f ms", s.alignMs)
                          else f("analog is %.1f ms AHEAD of HD1", -s.alignMs)
                val sure = if (s.alignState == "ok") "" else " (unconfirmed)"
                val jumps = if (s.alignJumps > 0) ", changed ${s.alignJumps}×" else ""
                f("%s (r %.2f), analog %+.1f dB%s\n%d measurement%s of %d%s",
                  lag, s.alignCorr, s.alignGainDb, sure,
                  s.alignCount, if (s.alignCount == 1) "" else "s", s.alignTries, jumps)
            }
        }
        // M12 step 2: RDS against the HD's call sign.
        val call = s.hdSameCall.ifEmpty { "PI ${s.rdsPi}".takeIf { s.rdsPi.isNotEmpty() } ?: "RDS" }
        val same = when (s.hdSame) {
            Signal.HD_SAME_YES -> "\nsame station (RDS $call = HD ${s.station})"
            Signal.HD_SAME_NO -> when (s.alignState) {
                "ok" -> "\nRDS says $call, HD says ${s.hdOtherName} - but the audio matches"
                "nomatch" -> "\nHD = ${s.hdOtherName}, another station than $call (RDS)"
                else -> "\nRDS says $call, HD says ${s.hdOtherName} - checking the audio…"   // 12a step 4
            }
            else -> ""
        }
        return listOf("Alignment" to text + same)
    }

    /**
     * M10c: what the gain watchdog did on this station (auto gain only):
     * "↓1 ↑2 · last ↑ 7.7 → 8.7 dB, 42 s ago". Empty list = no row.
     * Step 2, second line: the MER tries - "MER tries 3, kept 1" or
     * "trying 15.7 dB…" while one runs (the Gain row shows the tried gain then).
     */
    private fun watchdogRow(s: Signal): List<Pair<String, String>> {
        val keeper = RadioService.instance?.gainKeeper ?: return emptyList()
        if (!s.gainAuto || s.gainDb < 0) return emptyList()
        val changes = if (keeper.lastTo < 0) "no changes" else {
            val arrow = if (keeper.lastTo < keeper.lastFrom) "↓" else "↑"
            val ago = (SystemClock.elapsedRealtime() - keeper.lastAtMs) / 1000
            val agoText = if (ago < 120) "$ago s ago" else "${ago / 60} min ago"
            String.format(Locale.US, "↓%d ↑%d · last %s %.1f → %.1f dB, %s",
                keeper.downs, keeper.ups, arrow, keeper.lastFrom / 10.0, keeper.lastTo / 10.0, agoText)
        }
        val tries = if (keeper.tryingTo >= 0)
            String.format(Locale.US, "trying %.1f dB…", keeper.tryingTo / 10.0)
        else "MER tries ${keeper.tries}, kept ${keeper.kept}"
        return listOf("Watchdog" to "$changes\n$tries")
    }

    /** Tap the signal line = open/close the details. The button = open/close the debug text. */
    private fun setUpToggles() {
        val prefs = getSharedPreferences(UI_PREFS, Context.MODE_PRIVATE)
        detailsOpen = prefs.getBoolean(KEY_DETAILS_OPEN, false)
        debugOpen = prefs.getBoolean(KEY_DEBUG_OPEN, false)
        showDebugOpen()

        binding.otherHdNotice.setOnClickListener {                 // M12 step 2
            RadioService.instance?.listenToOtherStation()
            refreshScreen()
        }
        binding.signalBox.setOnClickListener {
            detailsOpen = !detailsOpen
            prefs.edit().putBoolean(KEY_DETAILS_OPEN, detailsOpen).apply()
            refreshScreen()
        }
        binding.debugToggle.setOnClickListener {
            debugOpen = !debugOpen
            prefs.edit().putBoolean(KEY_DEBUG_OPEN, debugOpen).apply()
            showDebugOpen()
        }
    }

    private fun showDebugOpen() {
        binding.sampleText.visibility = if (debugOpen) View.VISIBLE else View.GONE
        binding.debugToggle.setText(if (debugOpen) R.string.hide_debug else R.string.show_debug)
    }

    // ------------------------------------------------------------------ sticky header (9g)

    // Is the compact "now playing" bar showing? (Only changed by updateSticky.)
    private var stickyShown = false

    /**
     * 9g: watch the ScrollView. Every scroll movement asks updateSticky whether
     * the bar should be on. Tapping the bar scrolls smoothly back to the top
     * (which then hides it again).
     */
    private fun setUpStickyBar() {
        binding.scroll.setOnScrollChangeListener { _, _, scrollY, _, _ -> updateSticky(scrollY) }
        binding.nowSticky.setOnClickListener { binding.scroll.smoothScrollTo(0, 0) }
    }

    /**
     * Shows the sticky bar once the big station name is half-way out of view
     * (the bar then covers the spot where the name was - a like-for-like swap),
     * hides it when you scroll back up or the radio is off. 150 ms fade each way.
     * station_row's top/height are measured inside the ScrollView's content,
     * which is exactly what scrollY counts in.
     */
    private fun updateSticky(scrollY: Int) {
        val row = binding.stationRow
        val show = RadioService.instance != null && row.height > 0 &&
            scrollY > row.top + row.height / 2
        if (show == stickyShown) return
        stickyShown = show

        val bar = binding.nowSticky
        bar.animate().cancel()
        if (show) {
            if (bar.visibility != View.VISIBLE) bar.alpha = 0f
            bar.visibility = View.VISIBLE
            bar.animate().alpha(1f).setDuration(STICKY_FADE_MS).start()
        } else {
            bar.animate().alpha(0f).setDuration(STICKY_FADE_MS)
                .withEndAction { if (!stickyShown) bar.visibility = View.GONE }
                .start()
        }
    }

    /** Shows the text, or hides the line completely if there's nothing to show. */
    private fun setTextOrHide(view: TextView, text: String) {
        view.text = text
        view.visibility = if (text.isBlank()) View.GONE else View.VISIBLE
    }

    private fun color(id: Int): Int = ContextCompat.getColor(this, id)

    /** M12: North America uses the RBDS program-type names, the rest of the world RDS. */
    private fun rbds(): Boolean = Settings.band(this) == FmBand.AMERICAS

    /** M12 step 2: the program line of an RDS station: "FM · Religious talk" ("" if no type). */
    private fun rdsTypeLine(s: Signal): String {
        val type = Rds.ptyName(s.rdsPty, rbds())
        return if (type.isEmpty()) "" else "FM · $type"
    }

    // ------------------------------------------------------------------ presets (9e step 1)

    /**
     * Makes the preset strip match the saved list, and sets the ☆/★.
     * [program] = the program playing (or, radio off, the one from last time).
     */
    private fun updatePresets(programs: List<Program>, program: Int, freqHz: Int) {
        // Keep the preset's call sign + program name up to date (9e step 2).
        // "sure" = the name is a real one (yours, or the station's own name
        // for the program), not just the program type like "Top 40".
        val current = programs.firstOrNull { it.number == program }
        // (M12 step 2: not while the HD here is ANOTHER station's - its name would end up on
        // the analog station's preset.)
        if (station.isNotEmpty() && current != null && !shownSignal.otherHd) {
            val label = ProgramNames.label(this, freqHz, current, station)
            val sure = ProgramNames.get(this, freqHz, program) != null || label != current.type
            Presets.refresh(this, freqHz, program, station, label, sure)
        }
        // M12 step 2: an analog FM preset saved before RDS had named the station.
        if (shownSignal.station.isEmpty()) Presets.refreshAnalog(this, freqHz, shownSignal.rdsName)

        val presets = Presets.load(this)
        val currentIndex = presets.indexOfFirst { it.matches(freqHz, program) }

        // The star: ★ (gold) if what you're hearing is a preset, else ☆.
        val isPreset = currentIndex >= 0
        binding.presetStar.text = if (isPreset) "★" else "☆"
        if (isPreset) binding.presetStar.setTextColor(color(R.color.preset_star_on))
        else binding.presetStar.setTextColor(binding.stationName.currentTextColor)
        binding.presetStar.contentDescription =
            getString(if (isPreset) R.string.preset_remove else R.string.preset_add)

        // 13a: not while a tile is being dragged - the rebuild would pull it out of
        // your hand. (The drop rebuilds the strip itself.)
        if (dragTile != null) return

        // Rebuild the tiles only if something changed: the list, which one is
        // playing, or a newly saved logo (LogoCache.version).
        val key = Preset.encodeList(presets) + "|$currentIndex|${LogoCache.version}"
        if (key == shownPresetsKey) return
        shownPresetsKey = key

        binding.presetHint.visibility = if (presets.isEmpty()) View.VISIBLE else View.GONE
        binding.presetScroll.visibility = if (presets.isEmpty()) View.GONE else View.VISIBLE

        val row = binding.presetRow
        row.removeAllViews()
        for ((i, preset) in presets.withIndex()) {
            // A copy of res/layout/item_preset.xml ("false" = we add it to the row ourselves).
            val tile = ItemPresetBinding.inflate(layoutInflater, row, false)
            tile.presetTitle.text = preset.title
            tile.presetFreq.text = preset.freqText

            // The saved logo; or, if there's none yet, the call sign in big letters.
            val logo = LogoCache.load(this, preset.freqHz, preset.program)?.let { decode(it) }
            tile.presetLogo.setImageBitmap(logo)
            tile.presetLogo.visibility = if (logo != null) View.VISIBLE else View.GONE
            tile.presetInitials.text = preset.initials                  // M12 step 2: "FM" for analog
            tile.presetInitials.visibility = if (logo == null) View.VISIBLE else View.GONE

            // The tile you're listening to gets a thick colored border.
            val playing = i == currentIndex
            tile.root.strokeWidth = dpToPx(if (playing) 2 else 1)
            tile.root.strokeColor = color(if (playing) R.color.preset_tile_current else R.color.preset_tile_edge)

            tile.root.setOnClickListener { tunePreset(preset) }
            setUpTileDrag(tile.root, i, preset)         // 13a: hold = menu, hold + drag = move
            row.addView(tile.root)
        }

        // When you switch to another preset, make sure its tile can be seen
        // (scroll to it) - but not on every redraw, so you can still scroll freely.
        // (13a: nor right after you dropped a tile - the strip stays where you left it.)
        if (currentIndex >= 0 && currentIndex != scrolledToIndex && !keepPresetScroll) {
            val tile = row.getChildAt(currentIndex)
            binding.presetScroll.post {
                val x = tile.left - (binding.presetScroll.width - tile.width) / 2
                binding.presetScroll.smoothScrollTo(maxOf(0, x), 0)
            }
        }
        scrolledToIndex = currentIndex
        keepPresetScroll = false
    }

    // ------------------------------------------------------------------ presets: drag & drop (13a)

    /**
     * Gives a tile its "hold" behaviour. Android tells us about a long-press but
     * not where the finger goes afterwards, so a touch listener watches the finger:
     * it returns false ("not mine") for everything until a tile is held, so taps,
     * long-presses and sideways scrolling of the strip work as before.
     */
    @SuppressLint("ClickableViewAccessibility")     // taps still go through performClick
    private fun setUpTileDrag(tile: View, index: Int, preset: Preset) {
        tile.setOnTouchListener { v, event ->
            val mine = dragTile === v && !dragSettling
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    dragFingerX = event.rawX
                    false
                }
                MotionEvent.ACTION_MOVE -> {
                    dragFingerX = event.rawX
                    if (mine) moveDraggedTile()
                    mine                                    // true = "we handled it"
                }
                MotionEvent.ACTION_UP -> {
                    if (mine) dropTile()
                    false           // let the tile finish its own "finger up" (no click after a hold)
                }
                MotionEvent.ACTION_CANCEL -> {
                    if (mine) cancelDrag()
                    false
                }
                else -> false
            }
        }
        tile.setOnLongClickListener { v ->
            if (dragTile == null) pickUpTile(v, index, preset)
            true                                            // true = the phone gives its little buzz
        }
    }

    /** A tile has been held long enough: it's "in your hand" now. */
    private fun pickUpTile(tile: View, index: Int, preset: Preset) {
        val row = binding.presetRow
        dragTile = tile
        dragPreset = preset
        dragFrom = index
        dragTarget = index
        dragMoved = false
        dragSettling = false
        dragStartX = dragFingerX
        dragStartScroll = binding.presetScroll.scrollX
        val gap = (tile.layoutParams as? ViewGroup.MarginLayoutParams)?.marginEnd ?: 0
        dragPitch = tile.width + gap

        // From now on the finger moves the TILE, not the strip or the page.
        tile.parent?.requestDisallowInterceptTouchEvent(true)
        tile.isPressed = false                              // no "pressed" shading while it's held

        // The look: the held tile gets the thick colored border and floats above the
        // others (translationZ); the others shrink and fade a little.
        (tile as? MaterialCardView)?.let {
            it.strokeWidth = dpToPx(2)
            it.strokeColor = color(R.color.preset_tile_current)
        }
        tile.translationZ = dpToPx(8).toFloat()
        for (i in 0 until row.childCount) {
            val other = row.getChildAt(i)
            if (other !== tile)
                other.animate().alpha(DRAG_DIM_ALPHA).scaleX(DRAG_DIM_SCALE).scaleY(DRAG_DIM_SCALE)
                    .setDuration(DRAG_ANIM_MS).start()
        }
        dragFrameMs = SystemClock.uptimeMillis()
        tile.postOnAnimation(dragScroller)
    }

    /**
     * The finger (or the strip under it) moved: put the held tile under the finger
     * and slide the tiles between its old and its new place one step aside.
     */
    private fun moveDraggedTile() {
        val tile = dragTile ?: return
        val row = binding.presetRow
        if (dragPitch <= 0) return
        val travel = dragFingerX - dragStartX
        if (abs(travel) > ViewConfiguration.get(this).scaledTouchSlop) dragMoved = true

        // Finger travel + how far the strip scrolled meanwhile; never past the ends of the row.
        val last = row.childCount - 1
        val shift = (travel + (binding.presetScroll.scrollX - dragStartScroll))
            .coerceIn(-dragFrom * dragPitch.toFloat(), (last - dragFrom) * dragPitch.toFloat())
        tile.translationX = shift

        // More than half a tile over a neighbour = its place is taken.
        val target = (dragFrom + (shift / dragPitch).roundToInt()).coerceIn(0, last)
        if (target == dragTarget) return
        dragTarget = target
        for (i in 0..last) {
            if (i == dragFrom) continue
            val aside = when {
                i in (dragFrom + 1)..target -> -dragPitch   // tiles on the right move left
                i in target until dragFrom -> dragPitch     // tiles on the left move right
                else -> 0
            }
            row.getChildAt(i).animate().translationX(aside.toFloat()).setDuration(DRAG_ANIM_MS).start()
        }
    }

    /** The finger let go of the held tile. */
    private fun dropTile() {
        val tile = dragTile ?: return
        val preset = dragPreset
        val from = dragFrom
        val to = dragTarget
        tile.removeCallbacks(dragScroller)

        if (!dragMoved) {
            // Held but not moved = the long-press of old: the menu.
            cancelDrag()
            if (preset != null) showPresetMenu(from, preset)
            return
        }
        // Glide into the new place (or back home), then save the new order and
        // draw the strip again in that order.
        dragSettling = true
        val row = binding.presetRow
        for (i in 0 until row.childCount) {
            val other = row.getChildAt(i)
            if (other !== tile)
                other.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(DRAG_ANIM_MS).start()
        }
        tile.animate().translationX((to - from) * dragPitch.toFloat()).translationZ(0f)
            .setDuration(DRAG_ANIM_MS)
            .withEndAction {
                if (dragTile !== tile) return@withEndAction         // cancelled meanwhile
                if (to != from) {
                    Presets.moveTo(this, from, to)
                    keepPresetScroll = true
                }
                endDrag()
            }
            .start()
    }

    /** Puts a held tile back without changing anything (also: the screen went away). */
    private fun cancelDrag() {
        val tile = dragTile ?: return
        tile.removeCallbacks(dragScroller)
        endDrag()
    }

    /** The end of every drag: forget it and draw the strip afresh from the saved list. */
    private fun endDrag() {
        val tile = dragTile ?: return
        tile.animate().cancel()
        tile.parent?.requestDisallowInterceptTouchEvent(false)
        dragTile = null
        dragPreset = null
        dragSettling = false
        shownPresetsKey = ""                                // force the rebuild...
        binding.presetRow.post { refreshScreen() }          // ...once this touch is dealt with
    }

    /** A preset tile was tapped: tune to its frequency AND program. */
    private fun tunePreset(preset: Preset) {
        uiHandler.removeCallbacks(delayedTune)      // a preset beats a pending ◀ ▶
        tunePending = false
        shownFreqHz = preset.freqHz
        showFrequency(preset.freqHz)
        tuneTo(preset.freqHz, preset.program)
    }

    /** The star: save what you're hearing as a preset, or remove it if it is one. */
    private fun togglePreset() {
        val service = RadioService.instance
        val freqHz = service?.freqHz ?: shownFreqHz
        val selected = service?.currentProgram() ?: RadioService.loadProgram(this)
        val program = if (selected >= 0) selected else 0

        val index = Presets.indexOf(this, freqHz, program)
        if (index >= 0) {
            removePreset(index)
        } else {
            val current = service?.programs()?.firstOrNull { it.number == program }
            val label = current?.let { ProgramNames.label(this, freqHz, it, station) }.orEmpty()
            // M12 step 2: no HD here (no programs, no HD name) -> an analog FM preset named
            // by RDS: "KKLA / 99.5 FM". (If HD turns up later, Presets.refresh makes it HD.)
            val s = shownSignal
            val analog = service != null && !FmBand.isAm(freqHz) &&
                         ((service.programs().isEmpty() && s.station.isEmpty()) || (s.otherHd && s.analog))
            val preset = if (analog) Preset(freqHz, 0, station = s.rdsName.ifEmpty { s.hdSameCall }, analog = true)
                         else Preset(freqHz, program, station = station, label = label)
            Presets.add(this, preset)
            Toast.makeText(this, getString(R.string.preset_saved, preset.freqText), Toast.LENGTH_SHORT).show()
        }
        refreshScreen()
    }

    /** Removes a preset, with an "Undo" button at the bottom for a few seconds. */
    private fun removePreset(index: Int) {
        val preset = Presets.load(this).getOrNull(index) ?: return
        Presets.remove(this, index)
        Snackbar.make(binding.root, getString(R.string.preset_removed, preset.freqText), Snackbar.LENGTH_LONG)
            .setAction(R.string.undo) {
                Presets.add(this, preset, index)        // put it back where it was
                refreshScreen()
            }
            .show()
        refreshScreen()
    }

    /**
     * Hold a tile and let go without moving it: Rename / Delete.
     * (13a: "Move left / right" are gone - hold the tile and drag it instead.)
     */
    private fun showPresetMenu(index: Int, preset: Preset) {
        val items = arrayOf(
            getString(R.string.preset_rename),
            getString(R.string.preset_delete),
        )
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("${preset.freqText} · ${preset.title}")
            .setItems(items) { _, which ->
                when (which) {
                    0 -> showPresetRenameDialog(index, preset)
                    1 -> removePreset(index)
                }
                refreshScreen()
            }
        // 13a step 2: a small gray line under the two choices says that tiles can be
        // dragged - every time, so it can't be missed. (Step 1 showed it once, as a
        // pop-up at the bottom of the screen; Derek never saw it.) Only when there is
        // something to reorder.
        if (Presets.load(this).size > 1) {
            val tip = TextView(this)
            tip.setText(R.string.preset_drag_tip)
            tip.textSize = 13f                              // sp
            tip.alpha = 0.6f
            tip.setPadding(dpToPx(24), dpToPx(12), dpToPx(24), dpToPx(8))
            dialog.setView(tip)                             // shown below the list
        }
        dialog.show()
    }

    /** Same idea as the program rename dialog (9b step 3b), for a preset. */
    private fun showPresetRenameDialog(index: Int, preset: Preset) {
        val input = EditText(this)
        input.setSingleLine()
        input.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
        input.filters = arrayOf<InputFilter>(InputFilter.LengthFilter(MAX_NAME_LENGTH))
        input.setText(preset.title)
        input.setSelectAllOnFocus(true)
        input.hint = preset.station.ifEmpty { preset.freqText }

        val box = FrameLayout(this)
        box.setPadding(dpToPx(24), dpToPx(8), dpToPx(24), 0)
        box.addView(input)

        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.rename_title, preset.freqText))
            .setMessage(R.string.preset_rename_message)
            .setView(box)
            .setPositiveButton(R.string.rename_save) { _, _ ->
                Presets.rename(this, index, input.text.toString())
                refreshScreen()
            }
            .setNeutralButton(R.string.rename_reset) { _, _ ->
                Presets.rename(this, index, "")
                refreshScreen()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        input.requestFocus()
    }

    // ------------------------------------------------------------------ programs (9b)

    /**
     * Makes the row of program buttons match what the station has on the air.
     * The selected program gets a filled button, the others an outlined one.
     */
    private fun updateProgramButtons(programs: List<Program>, selected: Int, freqHz: Int) {
        // 3b: the name on each button - yours if you renamed it, else the station's.
        val labels = programs.map { ProgramNames.label(this, freqHz, it, station) }

        val key = programs.indices.joinToString(";") { "${programs[it].number}=${labels[it]}" } +
            " sel=$selected"
        if (key == shownProgramsKey) return                  // nothing changed
        shownProgramsKey = key

        // If any button has a name (2 lines, e.g. "HD1" + "Rock"), make ALL of
        // them 2 lines tall, so the row lines up.
        val lines = if (labels.any { it.isNotEmpty() }) 2 else 1

        val row = binding.programRow
        row.removeAllViews()
        for ((i, program) in programs.withIndex()) {
            val label = labels[i]
            // Outlined style for the programs that aren't playing.
            val style = if (program.number == selected)
                com.google.android.material.R.attr.materialButtonStyle
            else
                com.google.android.material.R.attr.materialButtonOutlinedStyle
            val button = MaterialButton(this, null, style)
            button.isAllCaps = false                        // keep "TikTok Radio" as sent
            // 9h: no 88 dp minimum width and less padding -> "HD1" is ~56 dp wide,
            // so HD1-HD4 fit next to the ⚙ without being cut off.
            button.minWidth = 0
            button.minimumWidth = 0
            button.setPaddingRelative(dpToPx(14), button.paddingTop, dpToPx(14), button.paddingBottom)
            button.minLines = lines
            // "HD2" on top, the name (if any) underneath.
            button.text = if (label.isEmpty()) program.hdName
                          else "${program.hdName}\n$label"
            button.setOnClickListener { selectProgram(program.number) }
            // 3b: long-press = rename. "true" = we handled it (no normal click after).
            button.setOnLongClickListener {
                showRenameDialog(freqHz, program)
                true
            }

            val params = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            params.marginEnd = dpToPx(6)                    // 9h: was 8
            row.addView(button, params)
        }
    }

    /**
     * 3b: a small window to rename a program. It shows your current name (or the
     * station's), selected, so you can just type over it.
     *   Save          -> keep your name (an empty box = back to the station's name)
     *   Station name  -> forget your name
     *   Cancel        -> change nothing
     */
    private fun showRenameDialog(freqHz: Int, program: Program) {
        val input = EditText(this)
        input.setSingleLine()
        input.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
        input.filters = arrayOf<InputFilter>(InputFilter.LengthFilter(MAX_NAME_LENGTH))
        input.setText(ProgramNames.label(this, freqHz, program, station))
        input.setSelectAllOnFocus(true)
        input.hint = program.hdName

        // Wrap the text box so it gets the same side margins as the dialog's text.
        val box = FrameLayout(this)
        box.setPadding(dpToPx(24), dpToPx(8), dpToPx(24), 0)
        box.addView(input)

        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.rename_title,
                "${RadioService.formatMhz(freqHz)} ${program.hdName}"))
            .setMessage(R.string.rename_message)
            .setView(box)
            .setPositiveButton(R.string.rename_save) { _, _ ->
                ProgramNames.set(this, freqHz, program.number, input.text.toString())
                shownProgramsKey = ""                       // force the buttons to redraw
                refreshScreen()
            }
            .setNeutralButton(R.string.rename_reset) { _, _ ->
                ProgramNames.set(this, freqHz, program.number, null)
                shownProgramsKey = ""
                refreshScreen()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        input.requestFocus()
    }

    /** A program button was tapped. */
    private fun selectProgram(number: Int) {
        RadioService.instance?.selectProgram(number)
        refreshScreen()                                     // move the highlight now
    }

    /** Converts dp (size on screen, same on every phone) to pixels. */
    private fun dpToPx(dp: Int): Int = (dp * resources.displayMetrics.density).roundToInt()

    // ------------------------------------------------------------------ USB

    /** Look for a supported dongle; get permission; start the radio service. */
    private fun findAndStartRadio() {
        if (RadioService.instance != null) return   // already playing

        val device = usbManager.deviceList.values.firstOrNull { d ->
            Pair(d.vendorId, d.productId) in supportedIds
        }
        if (device == null) {
            // 9h: the status line under the station name says "Plug in the
            // RTL-SDR" (and stays until you do), so no message here.
            showMessage("")
            return
        }

        if (usbManager.hasPermission(device)) {
            startRadioService(device)
        } else {
            showMessage("Found ${describe(device)}. Asking for permission…")
            requestPermission(device)
        }
    }

    private fun startRadioService(device: UsbDevice) {
        showMessage("Starting radio…")
        val intent = Intent(this, RadioService::class.java)
            .setAction(RadioService.ACTION_START)
            .putExtra(RadioService.EXTRA_DEVICE, device)
        // A "foreground" service: allowed to keep running in the background
        // because it shows a notification.
        ContextCompat.startForegroundService(this, intent)
    }

    /** Pops up Android's "Allow HiDef Radio to access…?" dialog. */
    private fun requestPermission(device: UsbDevice) {
        // The answer comes back as a broadcast to permissionReceiver (below).
        // setPackage() keeps the broadcast private to our app.
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
        // MUTABLE lets Android attach the device and the yes/no answer.
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            PendingIntent.FLAG_MUTABLE else 0
        val pendingIntent = PendingIntent.getBroadcast(this, 0, intent, flags)
        usbManager.requestPermission(device, pendingIntent)
    }

    /** Receives the USB permission answer. */
    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val device = IntentCompat.getParcelableExtra(
                intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (granted && device != null) {
                startRadioService(device)
            } else {
                Log.i(TAG, "USB permission denied")
                showMessage("USB permission was denied.")
            }
        }
    }

    /** Reads the vendor/product ID pairs out of res/xml/device_filter.xml. */
    private fun loadSupportedIds(): Set<Pair<Int, Int>> {
        val ids = mutableSetOf<Pair<Int, Int>>()
        val parser = resources.getXml(R.xml.device_filter)
        while (parser.eventType != XmlPullParser.END_DOCUMENT) {
            if (parser.eventType == XmlPullParser.START_TAG && parser.name == "usb-device") {
                val vid = parser.getAttributeIntValue(null, "vendor-id", -1)
                val pid = parser.getAttributeIntValue(null, "product-id", -1)
                ids.add(Pair(vid, pid))
            }
            parser.next()
        }
        parser.close()
        return ids
    }

    /** e.g. "RTL2838UHIDIR (0bda:2838)" */
    private fun describe(d: UsbDevice): String =
        "${d.productName ?: "USB device"} (%04x:%04x)".format(d.vendorId, d.productId)

    /** 9d: a message for when the radio isn't running - shown where the song goes. */
    private fun showMessage(text: String) {
        message = text
        showDebug(text)
        refreshScreen()
    }

    /** The debug text (behind "Show debug info"), under the native "Hello" line. */
    private fun showDebug(status: String) {
        binding.sampleText.text = "${RadioEngine.stringFromJNI()}\n\n$status"
    }

    companion object {
        private const val TAG = "HiDefRadio"
        private const val ACTION_USB_PERMISSION =
            "io.github.derek20la.hidefradio.USB_PERMISSION"

        // Wait this long after the last ◀ ▶ tap before retuning.
        // (M10b: 500, was 700 - retuning no longer freezes the screen.)
        private const val TUNE_DELAY_MS = 500L
        // 9g: fade time of the sticky "now playing" bar.
        private const val STICKY_FADE_MS = 150L

        // 13a: dragging a preset tile.
        private const val DRAG_ANIM_MS = 150L           // tiles sliding aside / settling
        private const val DRAG_DIM_ALPHA = 0.55f        // the tiles you're NOT holding
        private const val DRAG_DIM_SCALE = 0.94f
        private const val DRAG_EDGE_DP = 56             // this close to the strip's edge = scroll
        private const val DRAG_SCROLL_DP_PER_S = 900    // fastest scroll while dragging

        // 3b: longest program name you can type (keeps the buttons a sane size).
        private const val MAX_NAME_LENGTH = 24

        // 9e step 3: Peak above this = overload (too much gain).
        private const val OVERLOAD_DBFS = -1.0f
        // 9k: FM quieting (dB, positive = how far the noise is pushed down), from the
        // Sept 28 recordings: empty 90.5 = 8, Z90 fading = 12, weak KGGI = 31,
        // KZNO/KDAY = 38-40, KKLA = 44, KLOS = 50. Below NOISE = red "noise",
        // below WEAK = amber "weak". One S-meter bar per threshold reached.
        private const val QUIET_NOISE_DB = 15f
        private const val QUIET_WEAK_DB = 25f
        private val QUIET_BARS_DB = floatArrayOf(15f, 20f, 28f, 36f, 44f)
        // M10c step 3: "weak" = CRC errors within the last 5 s, or synced 5 s without audio.
        private const val WEAK_HOLD_MS = 5_000L
        private const val WEAK_NO_AUDIO_MS = 5_000L
        private const val WEAK_NO_AUDIO_AM_MS = 10_000L   // 9c

        // 9d: remembers whether the signal details / debug text are open.
        private const val UI_PREFS = "ui"
        private const val KEY_DETAILS_OPEN = "details_open"
        private const val KEY_DEBUG_OPEN = "debug_open"
    }
}
