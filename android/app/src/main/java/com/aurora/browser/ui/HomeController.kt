package com.aurora.browser.ui

import android.os.Handler
import android.os.Looper
import android.text.format.DateFormat
import android.view.LayoutInflater
import android.view.View
import android.widget.GridLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.aurora.browser.R
import com.aurora.browser.adblock.AdblockService
import com.aurora.browser.core.Omnibox
import com.aurora.browser.data.BrowserDb
import com.aurora.browser.tabs.IncognitoProfile
import java.util.Date

/**
 * The Aurora start page (desktop aurora://home): aurora backdrop, logo and
 * wordmark, a glowing search pill, and glow cards for shortcuts (bookmarks,
 * falling back to most-visited), recently visited pages and quick actions.
 * Real data only: empty cards show an empty state, never made-up rows.
 */
class HomeController(
    private val root: View,
    private val db: BrowserDb,
    private val adblock: AdblockService,
    private val callbacks: Callbacks,
) {
    interface Callbacks {
        fun onHomeSearchTapped()
        fun onHomeOpen(url: String)
        fun onHomeAction(action: Action)
    }

    enum class Action { INCOGNITO, HISTORY, BOOKMARKS, DOWNLOADS, SETTINGS }

    private val ctx = root.context
    private val inflater = LayoutInflater.from(ctx)
    private val clock: TextView = root.findViewById(R.id.homeClock)
    private val date: TextView = root.findViewById(R.id.homeDate)
    private val blocked: TextView = root.findViewById(R.id.homeBlocked)
    private val searchHint: TextView = root.findViewById(R.id.homeSearchHint)
    private val incognitoCard: View = root.findViewById(R.id.homeIncognitoCard)
    private val incognitoIsolation: TextView = root.findViewById(R.id.homeIncognitoIsolation)
    private val shortcutsCard: View = root.findViewById(R.id.homeShortcutsCard)
    private val shortcuts: GridLayout = root.findViewById(R.id.homeShortcuts)
    private val shortcutsEmpty: View = root.findViewById(R.id.homeShortcutsEmpty)
    private val recentCard: View = root.findViewById(R.id.homeRecentCard)
    private val recent: LinearLayout = root.findViewById(R.id.homeRecent)
    private val recentEmpty: View = root.findViewById(R.id.homeRecentEmpty)
    private val actions: LinearLayout = root.findViewById(R.id.homeActions)
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            updateClock()
            handler.postDelayed(this, 30_000)
        }
    }

    init {
        root.findViewById<View>(R.id.homeSearchPill).setOnClickListener { callbacks.onHomeSearchTapped() }
        buildActions()
    }

    fun bind(incognito: Boolean) {
        updateClock()
        searchHint.setText(if (incognito) R.string.omnibox_hint_incognito else R.string.omnibox_hint)
        blocked.text = ctx.getString(R.string.blocked_total, UiUtil.compactCount(adblock.totalBlocked()))
        blocked.visibility = if (adblock.enabled) View.VISIBLE else View.GONE
        incognitoCard.visibility = if (incognito) View.VISIBLE else View.GONE
        incognitoIsolation.setText(if (IncognitoProfile.isolated) R.string.incognito_isolated else R.string.incognito_shared)
        // Incognito start pages show nothing from the normal profile.
        shortcutsCard.visibility = if (incognito) View.GONE else View.VISIBLE
        recentCard.visibility = if (incognito) View.GONE else View.VISIBLE
        if (!incognito) {
            bindShortcuts()
            bindRecent()
        }
    }

    fun onShown() {
        handler.removeCallbacks(tick)
        handler.post(tick)
    }

    fun onHidden() {
        handler.removeCallbacks(tick)
    }

    private fun updateClock() {
        val now = Date()
        clock.text = DateFormat.getTimeFormat(ctx).format(now)
        date.text = DateFormat.format("EEEE, d MMMM", now)
    }

    private fun bindShortcuts() {
        shortcuts.removeAllViews()
        val items = ArrayList<Pair<String, String>>()
        db.bookmarks().take(8).forEach { items.add(it.url to it.title) }
        if (items.size < 8) {
            db.topSites(16).filter { site -> items.none { it.first == site.url } }
                .distinctBy { Omnibox.hostOf(it.url) }
                .take(8 - items.size)
                .forEach { items.add(it.url to it.title) }
        }
        shortcutsEmpty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
        for ((url, title) in items) {
            val tile = inflater.inflate(R.layout.item_home_tile, shortcuts, false)
            val glyph = SiteBrand.forUrl(url)
            FaIcons.applyTo(tile.findViewById(R.id.tileLetter), glyph.codepoint, glyph.brands)
            tile.findViewById<TextView>(R.id.tileLabel).text =
                title.ifBlank { Omnibox.hostOf(url)?.removePrefix("www.") ?: url }
            tile.setOnClickListener { callbacks.onHomeOpen(url) }
            val lp = GridLayout.LayoutParams(GridLayout.spec(GridLayout.UNDEFINED), GridLayout.spec(GridLayout.UNDEFINED, 1f))
            lp.width = 0
            shortcuts.addView(tile, lp)
        }
    }

    private fun bindRecent() {
        recent.removeAllViews()
        val items = db.recentHistory(5)
        recentEmpty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
        for (h in items) {
            val row = inflater.inflate(R.layout.item_home_row, recent, false)
            val glyph = SiteBrand.forUrl(h.url)
            FaIcons.applyTo(row.findViewById(R.id.rowLetter), glyph.codepoint, glyph.brands)
            row.findViewById<TextView>(R.id.rowTitle).text = h.title.ifBlank { h.url }
            row.findViewById<TextView>(R.id.rowSubtitle).text =
                "${Omnibox.hostOf(h.url)?.removePrefix("www.") ?: ""} · ${UiUtil.relativeTime(h.lastVisit)}"
            row.setOnClickListener { callbacks.onHomeOpen(h.url) }
            recent.addView(row)
        }
    }

    private fun buildActions() {
        val defs = listOf(
            Triple(Action.INCOGNITO, FaIcons.USER_SECRET, R.string.incognito),
            Triple(Action.HISTORY, FaIcons.CLOCK_ROTATE_LEFT, R.string.history),
            Triple(Action.BOOKMARKS, FaIcons.BOOKMARK, R.string.bookmarks),
            Triple(Action.DOWNLOADS, FaIcons.DOWNLOAD, R.string.downloads),
            Triple(Action.SETTINGS, FaIcons.GEAR, R.string.settings),
        )
        for ((action, glyph, label) in defs) {
            val v = inflater.inflate(R.layout.item_home_action, actions, false)
            v.findViewById<FaIconView>(R.id.actionIcon).setFa(glyph)
            v.findViewById<TextView>(R.id.actionLabel).setText(label)
            v.setOnClickListener { callbacks.onHomeAction(action) }
            actions.addView(v)
        }
    }
}
