package io.github.derek20la.hidefradio

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapRegionDecoder
import android.graphics.Canvas
import android.graphics.Rect
import android.os.Build
import android.util.Log
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors
import kotlin.math.asinh
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.tan

/**
 * Traffic and weather maps, for the stations that send them over the air.
 *
 * Two kinds of feed carry the same two maps:
 *  - TTN (Total Traffic Network): files. TMT_<area>_<row>_<col>_<date>_<time>_<id>.png are the
 *    nine traffic tiles; DWRO_* is a radar overlay, DWRI_* the text that says where it belongs.
 *  - HERE: nrsc5 hands over the images itself, traffic tiles named trafficMap_<row>_<col>_... and
 *    weather images with their corners.
 *
 * Traffic: nine tiles side by side, as soon as all nine are here.
 * Weather: the radar picture on top of a piece of the US map in assets, cut out with the corners
 * that came with it. The radar repeats itself between updates, so only a picture that changed
 * makes a new frame; about 90 minutes of frames are kept for the slider.
 *
 * RadioService calls poll() while the radio runs, so the maps are ready when someone looks;
 * the work is done on a thread of its own. The screen (MapPanel) reads traffic and frames, and
 * looks at generation to know something changed.
 */
object MapFeed {

    class WeatherFrame(val jpeg: ByteArray, val timeMs: Long)

    private const val TAG = "MapFeed"
    private const val BASE_MAP = "usa_basemap.jpg"
    private const val MAX_FRAMES = 12               // a new radar picture comes every 8 minutes or so
    private const val MIN_FRAME = 981               // the radar pictures: at least this many pixels
    private const val MAX_FRAME = 2000              // and at most this, or as sharp as the map is
    private const val JPEG_QUALITY = 90

    private val worker = Executors.newSingleThreadExecutor()

    @Volatile var generation = 0
        private set
    @Volatile var traffic: Bitmap? = null
        private set
    @Volatile var trafficTimeMs = 0L
        private set
    @Volatile var frames: List<WeatherFrame> = emptyList()
        private set

    private var epoch = -1

    // the thread's own: the tiles so far
    private val tiles = arrayOfNulls<Bitmap>(9)
    private var trafficHere: Boolean? = null        // which feed the tiles are from; null: none yet

    private var corners: DoubleArray? = null        // north, west, south, east
    private var baseMap: Bitmap? = null
    private var waitingOverlay: ByteArray? = null   // a radar picture that came before its corners
    private var lastOverlay = 0

    /** Takes what the decoder has collected (RadioService, every couple of seconds). */
    fun poll(context: Context) {
        val now = RadioEngine.getMapEpochNative()
        if (now != epoch) {
            epoch = now
            worker.execute { forgetStation() }
        }
        while (true) {
            val item = RadioEngine.takeMapItemNative() ?: break
            worker.execute {
                try {
                    handle(context.applicationContext, item)
                } catch (e: Exception) {
                    Log.w(TAG, "map file: $e")
                } catch (e: OutOfMemoryError) {
                    Log.w(TAG, "map file: $e")
                }
            }
        }
    }

    private fun forgetStation() {
        tiles.fill(null)
        trafficHere = null
        traffic = null
        frames = emptyList()
        corners = null
        baseMap = null
        waitingOverlay = null
        lastOverlay = 0
        generation++
    }

    private fun handle(context: Context, item: ByteArray) {
        val end = item.indexOf('\n'.code.toByte())
        if (end < 0) return
        val head = String(item, 0, end, Charsets.UTF_8).split('\t')
        if (head.size < 6) return
        val data = item.copyOfRange(end + 1, item.size)
        val name = head[1]
        val box = DoubleArray(4) { head[2 + it].toDoubleOrNull() ?: 0.0 }
        when (head[0]) {
            "T" -> trafficTile(data, hereTilePosition(name), here = true)
            "W" -> {
                setCorners(context, box)
                weather(context, data)
            }
            else -> when {
                name.startsWith("TMT_") -> trafficTile(data, ttnTilePosition(name), here = false)
                name.startsWith("DWRI_") -> weatherInfo(context, String(data, Charsets.UTF_8))
                name.startsWith("DWRO_") -> weather(context, data)
            }
        }
    }

    // ------------------------------------------------------------------ traffic

    /** "TMT_02qa5k_3_2_20220702_0654_03c2.png": row 3, column 2 -> 0..8, or -1. */
    private fun ttnTilePosition(name: String): Int {
        val parts = name.split('_')
        if (parts.size < 4) return -1
        val row = parts[2].toIntOrNull() ?: return -1
        val col = parts[3].toIntOrNull() ?: return -1
        return if (row in 1..3 && col in 1..3) (row - 1) * 3 + (col - 1) else -1
    }

    /** "trafficMap_1_2_rdhs.png" -> 0..8, or -1. */
    private fun hereTilePosition(name: String): Int {
        val parts = name.removeSuffix(".png").split('_')
        val at = parts.indexOf("trafficMap")
        if (at < 0 || parts.size <= at + 2) return -1
        val row = parts[at + 1].toIntOrNull() ?: return -1
        val col = parts[at + 2].toIntOrNull() ?: return -1
        // (seen counted from 1 and from 0)
        val r = if (row == 3 || col == 3 || row == 1 && col == 1) row - 1 else row
        val c = if (row == 3 || col == 3 || row == 1 && col == 1) col - 1 else col
        return if (r in 0..2 && c in 0..2) r * 3 + c else -1
    }

    private fun trafficTile(data: ByteArray, position: Int, here: Boolean) {
        if (position < 0) return
        if (trafficHere != null && trafficHere != here) tiles.fill(null)
        trafficHere = here
        tiles[position] = BitmapFactory.decodeByteArray(data, 0, data.size) ?: return
        if (tiles.any { it == null }) return
        val w = tiles[0]!!.width
        val h = tiles[0]!!.height
        val map = Bitmap.createBitmap(w * 3, h * 3, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(map)
        for (i in tiles.indices) canvas.drawBitmap(tiles[i]!!, (i % 3) * w.toFloat(), (i / 3) * h.toFloat(), null)
        traffic = map
        trafficTimeMs = System.currentTimeMillis()
        generation++
    }

    // ------------------------------------------------------------------ weather

    /** The DWRI text: Coordinates="(35.63475,-114.6349)";"(31.13642,-109.2450)" */
    private fun weatherInfo(context: Context, text: String) {
        val line = text.lineSequence().firstOrNull { it.startsWith("Coordinates=") } ?: return
        val numbers = Regex("-?\\d+(?:\\.\\d+)?").findAll(line).map { it.value.toDouble() }.toList()
        if (numbers.size < 4) return
        setCorners(context, numbers.take(4).toDoubleArray())
        waitingOverlay?.let {
            waitingOverlay = null
            weather(context, it)
        }
    }

    private fun setCorners(context: Context, box: DoubleArray) {
        if (corners?.contentEquals(box) == true && baseMap != null) return
        corners = box
        baseMap = cutBaseMap(context, box)
        // the frames so far belong to another area
        if (frames.isNotEmpty()) {
            frames = emptyList()
            generation++
        }
        lastOverlay = 0
    }

    /** The piece of the US map under the radar. Null if the corners are outside the map. */
    private fun cutBaseMap(context: Context, box: DoubleArray): Bitmap? {
        context.assets.open(BASE_MAP).use { stream ->
            val decoder = if (Build.VERSION.SDK_INT >= 31) BitmapRegionDecoder.newInstance(stream)
            else @Suppress("DEPRECATION") BitmapRegionDecoder.newInstance(stream, false)
            decoder ?: return null
            try {
                val area = mapArea(box, decoder.width, decoder.height)
                if (area.isEmpty) return null
                return decoder.decodeRegion(area, BitmapFactory.Options())
            } finally {
                decoder.recycle()
            }
        }
    }

    /**
     * Corners to pixels on the base map, a Web Mercator projection. The numbers belong to that
     * picture: its top left corner is 52.48278 N, 130.78125 W, and at 12032 x 6912 pixels 39.34135
     * degrees of longitude span 7162 pixels, and the sum works for any multiple of that size (the
     * same as nrsc5-gui and NRSC5 Studio use; the asset is their double-size map).
     */
    private fun mapArea(box: DoubleArray, width: Int, height: Int): Rect {
        fun mercator(deg: Double) = asinh(tan(Math.toRadians(deg)))
        val top = mercator(52.482780)
        val span = top - mercator(38.898)
        val xScale = 7162.0 * width / 12032.0
        val yScale = 3565.0 * height / 6912.0
        val x1 = ((box[1] + 130.781250) * xScale / 39.34135).roundToInt()
        val x2 = ((box[3] + 130.781250) * xScale / 39.34135).roundToInt()
        val y1 = ((top - mercator(box[0])) * yScale / span).roundToInt()
        val y2 = ((top - mercator(box[2])) * yScale / span).roundToInt()
        return Rect(max(x1, 0), max(y1, 0), min(x2, width), min(y2, height))
    }

    private fun weather(context: Context, data: ByteArray) {
        val base = baseMap
        if (corners == null) {
            waitingOverlay = data       // its corners are on their way (the DWRI file)
            return
        }
        val hash = data.contentHashCode()
        if (hash == lastOverlay) return
        val overlay = BitmapFactory.decodeByteArray(data, 0, data.size) ?: return
        lastOverlay = hash

        val picture = if (base != null) compose(base, overlay) else overlay
        val out = ByteArrayOutputStream(256 * 1024)
        picture.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, out)
        frames = (frames + WeatherFrame(out.toByteArray(), System.currentTimeMillis())).takeLast(MAX_FRAMES)
        generation++
    }

    /** The radar over the map: black is "nothing here", the brighter the more solid. */
    private fun compose(base: Bitmap, overlay: Bitmap): Bitmap {
        val size = base.width.coerceIn(MIN_FRAME, MAX_FRAME)
        val map = IntArray(size * size)
        val radar = IntArray(size * size)
        Bitmap.createScaledBitmap(base, size, size, true).getPixels(map, 0, size, 0, 0, size, size)
        Bitmap.createScaledBitmap(overlay, size, size, true).getPixels(radar, 0, size, 0, 0, size, size)
        for (i in map.indices) {
            val p = radar[i]
            val r = p shr 16 and 255
            val g = p shr 8 and 255
            val b = p and 255
            val brightness = (r + g + b) / 3
            if (brightness <= 8) continue
            val alpha = max(brightness, p ushr 24)
            val m = map[i]
            val mr = m shr 16 and 255
            val mg = m shr 8 and 255
            val mb = m and 255
            map[i] = (255 shl 24) or
                ((r * alpha + mr * (255 - alpha)) / 255 shl 16) or
                ((g * alpha + mg * (255 - alpha)) / 255 shl 8) or
                ((b * alpha + mb * (255 - alpha)) / 255)
        }
        return Bitmap.createBitmap(map, size, size, Bitmap.Config.ARGB_8888)
    }
}
