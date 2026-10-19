package com.aurora.browser.tabs

import com.aurora.browser.core.Omnibox
import org.json.JSONArray
import org.json.JSONObject

/**
 * Ordered tab list plus the active tab. Normal and incognito tabs live in
 * one list (the switcher shows them in separate pages); only normal tabs
 * are ever serialized.
 */
class TabManager {
    private val _tabs = ArrayList<Tab>()
    val tabs: List<Tab> get() = _tabs
    private var nextId = 1L

    var active: Tab? = null
        private set

    /** Recently closed normal tabs, newest last (for "reopen closed tab"). */
    private val closed = ArrayDeque<Pair<String, String>>()

    fun normalTabs(): List<Tab> = _tabs.filter { !it.incognito }
    fun incognitoTabs(): List<Tab> = _tabs.filter { it.incognito }

    fun create(incognito: Boolean, url: String = Omnibox.HOME_URL, title: String = "", openerId: Long? = null, afterActive: Boolean = false): Tab {
        val tab = Tab(nextId++, incognito, url, title, openerId)
        val anchor = active
        val idx = if (afterActive && anchor != null) _tabs.indexOf(anchor) + 1 else _tabs.size
        _tabs.add(idx.coerceIn(0, _tabs.size), tab)
        return tab
    }

    fun setActive(tab: Tab?) {
        active = tab
    }

    fun find(id: Long): Tab? = _tabs.firstOrNull { it.id == id }

    /**
     * Removes [tab] and returns the tab that should become active (same
     * mode neighbour, then opener, then any tab), or null if none remain.
     */
    fun remove(tab: Tab): Tab? {
        val idx = _tabs.indexOf(tab)
        if (idx < 0) return active
        _tabs.removeAt(idx)
        if (!tab.incognito && !tab.showingHome) {
            closed.addLast(tab.url to tab.title)
            while (closed.size > 20) closed.removeFirst()
        }
        if (active != tab) return active
        val opener = tab.openerId?.let { find(it) }
        val next = _tabs.drop(idx).firstOrNull { it.incognito == tab.incognito }
            ?: _tabs.take(idx).lastOrNull { it.incognito == tab.incognito }
        return opener ?: next ?: _tabs.lastOrNull()
    }

    fun popClosed(): Pair<String, String>? = closed.removeLastOrNull()

    // --- Session (normal tabs only) -------------------------------------------

    fun serialize(): String {
        val arr = JSONArray()
        val normal = normalTabs()
        for (t in normal) arr.put(JSONObject().put("url", t.url).put("title", t.title))
        val activeIdx = normal.indexOf(active).coerceAtLeast(0)
        return JSONObject().put("tabs", arr).put("active", activeIdx).toString()
    }

    /** Restores tabs (without WebViews); returns the tab to activate. */
    fun restore(json: String): Tab? {
        if (json.isBlank()) return null
        return try {
            val o = JSONObject(json)
            val arr = o.getJSONArray("tabs")
            val restored = (0 until arr.length()).map {
                val t = arr.getJSONObject(it)
                create(false, t.optString("url", Omnibox.HOME_URL), t.optString("title", ""))
            }
            restored.getOrNull(o.optInt("active", 0)) ?: restored.lastOrNull()
        } catch (_: Exception) {
            null
        }
    }
}
