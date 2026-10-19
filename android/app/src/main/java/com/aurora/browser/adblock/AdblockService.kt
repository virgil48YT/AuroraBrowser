package com.aurora.browser.adblock

import android.content.Context
import android.util.Log
import com.aurora.browser.data.Prefs
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/**
 * Owns the live [AdblockEngine] (built-in list from assets + the user's
 * custom filters) and the per-site Shields allow list. The engine is
 * rebuilt off the main thread and swapped atomically.
 */
class AdblockService private constructor(context: Context) {
    private val app = context.applicationContext
    private val prefs = Prefs(app)
    private val builder = Executors.newSingleThreadExecutor()

    @Volatile var engine: AdblockEngine = AdblockEngine.EMPTY
        private set

    @Volatile private var disabledSites: Set<String> = prefs.shieldsDisabledSites

    private val blockedSinceStart = AtomicLong(0)

    val builtinRules: String by lazy {
        app.assets.open(BUILTIN_ASSET).bufferedReader().use { it.readText() }
    }

    init {
        rebuild()
    }

    fun rebuild(onDone: (() -> Unit)? = null) {
        builder.execute {
            val started = System.nanoTime()
            val rules = builtinRules + "\n" + prefs.customFilters
            engine = AdblockEngine.build(rules)
            Log.i(TAG, "engine built: ${engine.networkRuleCount} network, ${engine.cosmeticRuleCount} cosmetic rules " +
                "in ${(System.nanoTime() - started) / 1_000_000} ms")
            onDone?.invoke()
        }
    }

    val enabled: Boolean get() = prefs.adblockEnabled
    val cosmeticEnabled: Boolean get() = prefs.cosmeticFiltering

    /** Site key used by the Shields toggle (registrable domain of the page). */
    fun siteKey(host: String): String = DomainUtil.registrableDomain(host)

    fun shieldsUp(pageHost: String): Boolean =
        enabled && (pageHost.isEmpty() || siteKey(pageHost) !in disabledSites)

    fun setShields(pageHost: String, up: Boolean) {
        val key = siteKey(pageHost)
        val next = disabledSites.toMutableSet().apply { if (up) remove(key) else add(key) }
        disabledSites = next
        prefs.shieldsDisabledSites = next
    }

    fun disabledSiteList(): List<String> = disabledSites.sorted()

    fun noteBlocked() {
        blockedSinceStart.incrementAndGet()
    }

    /** Persist the running total (called when the activity pauses). */
    fun flushStats() {
        val n = blockedSinceStart.getAndSet(0)
        if (n > 0) prefs.totalBlocked = prefs.totalBlocked + n
    }

    fun totalBlocked(): Long = prefs.totalBlocked + blockedSinceStart.get()

    companion object {
        private const val TAG = "AuroraAdblock"
        const val BUILTIN_ASSET = "filters/aurora-builtin.txt"

        @Volatile private var instance: AdblockService? = null

        fun get(context: Context): AdblockService =
            instance ?: synchronized(this) { instance ?: AdblockService(context).also { instance = it } }
    }
}
