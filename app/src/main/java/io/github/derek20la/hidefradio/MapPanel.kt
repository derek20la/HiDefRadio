package io.github.derek20la.hidefradio

import android.content.Context
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.text.format.DateFormat
import android.util.AttributeSet
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import java.util.Date

/**
 * The traffic and weather maps (MapFeed), under the signal line while the signal details are
 * closed: two tabs, the map, and for the radar a slider through the frames and a play button.
 */
class MapPanel @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) :
    LinearLayout(context, attrs) {

    private val trafficTab = tabButton(R.string.map_traffic)
    private val weatherTab = tabButton(R.string.map_weather)
    private val note = TextView(context)
    private val image = ImageView(context)
    private val radarRow = LinearLayout(context)
    private val playButton = Button(context, null, 0, com.google.android.material.R.style.Widget_MaterialComponents_Button_TextButton)
    private val slider = SeekBar(context)
    private val handler = Handler(Looper.getMainLooper())

    private var showWeather = false
    private var seen = -1
    private var frame = 0
    private var frameShown: MapFeed.WeatherFrame? = null
    private var playing = false

    private val player = object : Runnable {
        override fun run() {
            val frames = MapFeed.frames
            if (!playing || frames.size < 2) {
                setPlaying(false)
                return
            }
            frame = (frame + 1) % frames.size
            show()
            handler.postDelayed(this, if (frame == frames.size - 1) PLAY_HOLD_MS else PLAY_STEP_MS)
        }
    }

    init {
        orientation = VERTICAL
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        showWeather = prefs.getBoolean(KEY_WEATHER, false)

        val tabs = LinearLayout(context).apply { gravity = Gravity.CENTER_VERTICAL }
        tabs.addView(trafficTab)
        tabs.addView(weatherTab)
        addView(tabs)
        trafficTab.setOnClickListener { pick(false) }
        weatherTab.setOnClickListener { pick(true) }

        note.textSize = 12f
        addView(note, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))

        radarRow.gravity = Gravity.CENTER_VERTICAL
        playButton.text = PLAY
        playButton.setOnClickListener { setPlaying(!playing) }
        radarRow.addView(playButton)
        radarRow.addView(slider, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
        addView(radarRow)          // (above the map, which can fill the pane)

        image.adjustViewBounds = true
        image.scaleType = ImageView.ScaleType.FIT_CENTER
        addView(image, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        slider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                if (!fromUser) return
                setPlaying(false)
                frame = progress
                show()
            }
            override fun onStartTrackingTouch(bar: SeekBar) {}
            override fun onStopTrackingTouch(bar: SeekBar) {}
        })
    }

    private fun tabButton(text: Int) =
        Button(context, null, 0, com.google.android.material.R.style.Widget_MaterialComponents_Button_TextButton)
            .apply { setText(text) }

    private fun pick(weather: Boolean) {
        showWeather = weather
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_WEATHER, weather).apply()
        setPlaying(false)
        frame = max(0, MapFeed.frames.size - 1)
        show()
    }

    private fun max(a: Int, b: Int) = if (a > b) a else b

    private fun setPlaying(on: Boolean) {
        playing = on
        playButton.text = if (on) PAUSE else PLAY
        handler.removeCallbacks(player)
        if (on) handler.postDelayed(player, PLAY_STEP_MS)
    }

    /** Called once a second by the screen; only does something when there's something new. */
    fun refresh() {
        if (seen == MapFeed.generation) return
        val atNewest = frame >= MapFeed.frames.size - 2     // (stays on the newest as frames come in)
        seen = MapFeed.generation
        if (atNewest || frame >= MapFeed.frames.size) frame = max(0, MapFeed.frames.size - 1)
        show()
    }

    private fun show() {
        trafficTab.alpha = if (showWeather) 0.5f else 1f
        weatherTab.alpha = if (showWeather) 1f else 0.5f
        if (showWeather) showWeatherFrame() else showTraffic()
    }

    private fun showTraffic() {
        radarRow.visibility = View.GONE
        frameShown = null
        val map = MapFeed.traffic
        image.setImageBitmap(map)
        image.visibility = if (map == null) View.GONE else View.VISIBLE
        note.text = if (map == null) context.getString(R.string.map_waiting_traffic)
        else context.getString(R.string.map_updated, clock(MapFeed.trafficTimeMs))
    }

    private fun showWeatherFrame() {
        val frames = MapFeed.frames
        if (frames.isEmpty()) {
            frameShown = null
            image.setImageBitmap(null)
            image.visibility = View.GONE
            radarRow.visibility = View.GONE
            note.text = context.getString(R.string.map_waiting_weather)
            return
        }
        val current = frames[frame.coerceIn(0, frames.size - 1)]
        if (current !== frameShown) {
            frameShown = current
            image.setImageBitmap(BitmapFactory.decodeByteArray(current.jpeg, 0, current.jpeg.size))
        }
        image.visibility = View.VISIBLE
        radarRow.visibility = if (frames.size > 1) View.VISIBLE else View.GONE
        slider.max = frames.size - 1
        slider.progress = frame
        note.text = context.getString(R.string.map_radar_time, clock(current.timeMs), frame + 1, frames.size)
    }

    private fun clock(ms: Long) = DateFormat.getTimeFormat(context).format(Date(ms))

    override fun onDetachedFromWindow() {
        setPlaying(false)
        super.onDetachedFromWindow()
    }

    private companion object {
        const val PREFS = "map_panel"
        const val KEY_WEATHER = "weather"
        const val PLAY = "▶"
        const val PAUSE = "⏸"
        const val PLAY_STEP_MS = 600L
        const val PLAY_HOLD_MS = 1800L       // a rest on the newest picture before it starts over
    }
}
