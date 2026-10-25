package io.github.derek20la.hidefradio

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat

/**
 * Build 33: Android 17's "local network" permission, for the rtl_tcp source.
 *
 * From Android 17 (API 37) on, an app that targets API 37 - as this one does - can't reach ANY
 * address on the home network (192.168.x.x, 10.x.x.x, raspberrypi.local ...) until the listener
 * allows it: the runtime permission ACCESS_LOCAL_NETWORK, which Android files under "Nearby
 * devices". Without it the system drops the packets deep in the network stack, so for our native
 * rtl_tcp socket a connection simply gets no answer ("can't connect ... (no answer)") - that is
 * what a tester's Android 17 phone showed (10-25). Internet addresses are not affected, and the
 * USB dongle has nothing to do with the network at all.
 * (developer.android.com/privacy-and-security/local-network-permission)
 *
 * Before Android 17 the INTERNET permission alone still covers the local network, and Google's
 * guide says not to ask for this one there - so every check below says "fine" on older phones.
 * The permission's name is written out as text, so this compiles and runs whatever the SDK
 * names its constant.
 */
object LocalNetwork {
    const val PERMISSION = "android.permission.ACCESS_LOCAL_NETWORK"
    private const val SINCE_API = 37            // Android 17

    /** True when this phone wants the permission at all (Android 17 or later). */
    fun asked(): Boolean = Build.VERSION.SDK_INT >= SINCE_API

    /** True when the app may reach the local network: always before Android 17, else when allowed. */
    fun allowed(context: Context): Boolean =
        !asked() || ContextCompat.checkSelfPermission(context, PERMISSION) == PackageManager.PERMISSION_GRANTED
}
