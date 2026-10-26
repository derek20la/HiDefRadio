package io.github.derek20la.hidefradio

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import org.xmlpull.v1.XmlPullParser

/**
 * Build 35: which USB devices are radio dongles, and is one plugged in?
 *
 * The list is res/xml/device_filter.xml - the same file that makes Android offer to open the
 * app when a dongle is plugged in. (Until build 34 only MainActivity read it; now RadioService
 * needs it too, for "a plugged-in dongle wins over rtl_tcp".)
 */
object Dongles {

    @Volatile private var ids: Set<Pair<Int, Int>>? = null

    /** The vendor / product ID pairs of every supported dongle (read once). */
    fun supportedIds(context: Context): Set<Pair<Int, Int>> {
        ids?.let { return it }
        val found = mutableSetOf<Pair<Int, Int>>()
        val parser = context.resources.getXml(R.xml.device_filter)
        while (parser.eventType != XmlPullParser.END_DOCUMENT) {
            if (parser.eventType == XmlPullParser.START_TAG && parser.name == "usb-device") {
                val vid = parser.getAttributeIntValue(null, "vendor-id", -1)
                val pid = parser.getAttributeIntValue(null, "product-id", -1)
                found.add(Pair(vid, pid))
            }
            parser.next()
        }
        parser.close()
        ids = found
        return found
    }

    fun isSupported(context: Context, device: UsbDevice): Boolean =
        Pair(device.vendorId, device.productId) in supportedIds(context)

    /** A supported dongle plugged into this phone right now, or null. */
    fun find(context: Context): UsbDevice? {
        val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
        return usb.deviceList.values.firstOrNull { isSupported(context, it) }
    }
}
