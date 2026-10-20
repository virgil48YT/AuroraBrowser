package com.aurora.browser.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.LruCache
import android.view.View
import android.widget.ImageView
import com.aurora.browser.R
import com.aurora.browser.core.Omnibox
import com.aurora.browser.core.SiteNames
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.Collections
import java.util.concurrent.Executors

/**
 * Site icons for the start page shortcuts, "Recently visited", History and
 * Bookmarks. Previously those surfaces only had Font Awesome brand glyphs, so
 * every site that is not a hard-coded brand showed the same globe.
 *
 * Icons come from, in order:
 *  1. the in-memory LRU cache;
 *  2. the on-disk cache (noBackupFilesDir/favicons/<domain>.png), which is
 *     filled whenever a normal (non-incognito) tab's WebView reports a page
 *     icon via onReceivedIcon — see [remember];
 *  3. a background fetch of the site's own icons (apple-touch-icon.png, then
 *     /favicon.ico), falling back to DuckDuckGo's favicon service
 *     (icons.duckduckgo.com, which does not set cookies or track).
 *
 * Icons are keyed by registrable domain, so every page of a site shares one
 * file. Failed lookups are remembered for a day to avoid hammering servers.
 * Network fetching is skipped entirely while [networkAllowed] says no (Tor
 * mode), so the cache can never leak browsing outside the Tor circuit.
 */
object Favicons {
    private const val TAG = "AuroraFavicons"
    private const val MAX_BYTES = 512 * 1024
    private const val MIN_EDGE = 16
    private const val STORE_EDGE = 96
    private const val MISS_TTL_MS = 24L * 60 * 60 * 1000

    private val memory = object : LruCache<String, Bitmap>(4 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val misses = Collections.synchronizedMap(HashMap<String, Long>())
    private val inFlight = Collections.synchronizedSet(HashSet<String>())
    private val io = Executors.newFixedThreadPool(3)
    private val main = Handler(Looper.getMainLooper())

    /** Set by the Tor integration: false = never fetch icons over the network. */
    @Volatile var networkAllowed: () -> Boolean = { true }

    private fun dir(ctx: Context): File = File(ctx.applicationContext.noBackupFilesDir, "favicons").apply { mkdirs() }

    /** Cache key for [url]: the registrable domain, or null for non-web URLs. */
    fun keyFor(url: String): String? {
        if (!Omnibox.isWebUrl(url)) return null
        val host = Omnibox.hostOf(url)?.takeIf { it.isNotEmpty() } ?: return null
        return SiteNames.registrableDomain(host).takeIf { it.isNotEmpty() }
    }

    private fun fileFor(ctx: Context, key: String) =
        File(dir(ctx), key.replace(Regex("[^a-z0-9._-]"), "_") + ".png")

    /** Memory-cached icon for [url], if any (main thread, no I/O). */
    fun cached(url: String): Bitmap? = keyFor(url)?.let { memory.get(it) }

    /**
     * Stores an icon a WebView reported for [url]. Never call this for
     * incognito tabs: the cache is part of the normal profile.
     */
    fun remember(ctx: Context, url: String, icon: Bitmap?) {
        val key = keyFor(url) ?: return
        if (icon == null || icon.width < MIN_EDGE || icon.height < MIN_EDGE) return
        val existing = memory.get(key)
        if (existing != null && existing.width >= icon.width) return
        val scaled = scaleForStore(icon)
        memory.put(key, scaled)
        misses.remove(key)
        val app = ctx.applicationContext
        io.execute { writePng(fileFor(app, key), scaled) }
    }

    /**
     * Shows the icon for [url] in [target]. [fallback] is shown right away
     * (and kept when no icon can be found); the real icon replaces it when
     * it arrives, unless the view has been re-bound to another URL.
     */
    fun load(target: ImageView, url: String, fallback: View? = null) {
        val key = keyFor(url)
        target.setTag(R.id.favicon_key, key)
        val hit = key?.let { memory.get(it) }
        if (hit != null) {
            show(target, hit, fallback)
            return
        }
        target.setImageDrawable(null)
        target.visibility = View.GONE
        fallback?.visibility = View.VISIBLE
        if (key == null) return
        val app = target.context.applicationContext
        val allowNetwork = networkAllowed()
        io.execute {
            val bmp = loadBlocking(app, key, url, allowNetwork)
            if (bmp != null) {
                main.post {
                    if (target.getTag(R.id.favicon_key) == key) show(target, bmp, fallback)
                }
            }
        }
    }

    /** Drops every cached icon (Settings > clear browsing data). */
    fun clear(ctx: Context) {
        memory.evictAll()
        misses.clear()
        val app = ctx.applicationContext
        io.execute { dir(app).listFiles()?.forEach { it.delete() } }
    }

    private fun show(target: ImageView, bmp: Bitmap, fallback: View?) {
        target.setImageBitmap(bmp)
        target.visibility = View.VISIBLE
        fallback?.visibility = View.GONE
    }

    private fun loadBlocking(ctx: Context, key: String, url: String, allowNetwork: Boolean): Bitmap? {
        memory.get(key)?.let { return it }
        val file = fileFor(ctx, key)
        if (file.isFile) {
            BitmapFactory.decodeFile(file.path)?.let {
                memory.put(key, it)
                return it
            }
        }
        if (!allowNetwork) return null
        val missedAt = misses[key]
        if (missedAt != null && System.currentTimeMillis() - missedAt < MISS_TTL_MS) return null
        if (!inFlight.add(key)) return null
        try {
            val host = Omnibox.hostOf(url) ?: return null
            val root = SiteNames.siteRoot(url)?.removeSuffix("/") ?: "https://$host"
            val candidates = listOf(
                "$root/apple-touch-icon.png",
                "$root/favicon.ico",
                "https://icons.duckduckgo.com/ip3/$key.ico",
            )
            for (candidate in candidates) {
                val bmp = fetch(candidate) ?: continue
                val scaled = scaleForStore(bmp)
                memory.put(key, scaled)
                writePng(file, scaled)
                return scaled
            }
            misses[key] = System.currentTimeMillis()
            return null
        } finally {
            inFlight.remove(key)
        }
    }

    private fun fetch(url: String): Bitmap? {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 5000
                readTimeout = 5000
                instanceFollowRedirects = true
                useCaches = false
                setRequestProperty("Accept", "image/png,image/x-icon,image/*;q=0.8")
            }
            if (conn.responseCode !in 200..299) return null
            val type = conn.contentType.orEmpty().lowercase()
            if (type.startsWith("text/")) return null // HTML 404 pages served with 200
            val bytes = conn.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                val buf = ByteArray(16 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    if (out.size() > MAX_BYTES) return null
                }
                out.toByteArray()
            }
            // BitmapFactory decodes PNG, JPEG, GIF, WebP and ICO.
            val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return null
            if (bmp.width < MIN_EDGE || bmp.height < MIN_EDGE) null else bmp
        } catch (e: Exception) {
            Log.d(TAG, "favicon fetch failed for $url: ${e.javaClass.simpleName}")
            null
        } finally {
            conn?.disconnect()
        }
    }

    private fun scaleForStore(bmp: Bitmap): Bitmap {
        val edge = maxOf(bmp.width, bmp.height)
        if (edge <= STORE_EDGE) return bmp
        val f = STORE_EDGE.toFloat() / edge
        return Bitmap.createScaledBitmap(bmp, (bmp.width * f).toInt().coerceAtLeast(1), (bmp.height * f).toInt().coerceAtLeast(1), true)
    }

    private fun writePng(file: File, bmp: Bitmap) {
        try {
            val tmp = File(file.path + ".tmp")
            FileOutputStream(tmp).use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            if (!tmp.renameTo(file)) tmp.delete()
        } catch (e: Exception) {
            Log.d(TAG, "could not cache favicon ${file.name}: ${e.javaClass.simpleName}")
        }
    }
}
