package io.github.derek20la.hidefradio

import android.content.Context
import java.security.MessageDigest

/**
 * Pictures you don't want to see again: stations often send an ad as the "album art". Holding a
 * finger on the picture blocks it. The block goes by the picture's content, so it stays blocked
 * when the station sends it again under another name or on another day; Settings can clear them.
 * The list stays on the phone.
 */
object ArtBlock {

    private const val PREFS = "art_block"
    private const val KEY_HASHES = "hashes"

    /** Goes up when the list changes, so the screen shows the picture again or stops showing it. */
    @Volatile var version = 0
        private set

    private var blocked: MutableSet<String>? = null

    @Synchronized
    private fun list(context: Context): MutableSet<String> =
        blocked ?: HashSet(prefs(context).getStringSet(KEY_HASHES, emptySet()) ?: emptySet()).also { blocked = it }

    private fun prefs(context: Context) = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun hash(picture: ByteArray) =
        MessageDigest.getInstance("SHA-256").digest(picture).joinToString("") { "%02x".format(it) }

    fun isBlocked(context: Context, picture: ByteArray): Boolean {
        val list = list(context)
        return list.isNotEmpty() && synchronized(this) { hash(picture) in list }
    }

    @Synchronized
    fun block(context: Context, picture: ByteArray) {
        val list = list(context)
        list.add(hash(picture))
        prefs(context).edit().putStringSet(KEY_HASHES, HashSet(list)).apply()
        version++
    }

    fun count(context: Context): Int = list(context).size

    @Synchronized
    fun clear(context: Context) {
        list(context).clear()
        prefs(context).edit().remove(KEY_HASHES).apply()
        version++
    }
}
