package com.aurora.browser.data

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import com.aurora.browser.core.Omnibox
import com.aurora.browser.core.SiteNames
import java.util.concurrent.Executors

data class HistoryEntry(val id: Long, val url: String, val title: String, val visitCount: Int, val lastVisit: Long)
data class Bookmark(val id: Long, val url: String, val title: String, val created: Long)

/** A frequently visited site (all pages of one domain folded together). */
data class TopSite(val url: String, val host: String, val name: String, val visits: Int, val lastVisit: Long)

/**
 * History and bookmarks (SQLite). Incognito tabs never call into this class.
 * Writes go through a single background thread; reads are small indexed
 * queries and run on the caller's thread.
 */
class BrowserDb private constructor(context: Context) :
    SQLiteOpenHelper(context.applicationContext, "aurora.db", null, 1) {

    private val writer = Executors.newSingleThreadExecutor()

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                url TEXT NOT NULL UNIQUE,
                title TEXT NOT NULL DEFAULT '',
                visit_count INTEGER NOT NULL DEFAULT 1,
                last_visit INTEGER NOT NULL)""",
        )
        db.execSQL("CREATE INDEX history_last_visit ON history(last_visit DESC)")
        db.execSQL(
            """CREATE TABLE bookmarks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                url TEXT NOT NULL UNIQUE,
                title TEXT NOT NULL DEFAULT '',
                created INTEGER NOT NULL)""",
        )
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) = Unit

    // --- History -------------------------------------------------------------

    fun recordVisit(url: String, title: String?) {
        writer.execute {
            val db = writableDatabase
            val now = System.currentTimeMillis()
            db.beginTransaction()
            try {
                val cv = ContentValues().apply {
                    put("url", url)
                    put("title", title.orEmpty())
                    put("last_visit", now)
                }
                if (db.insertWithOnConflict("history", null, cv, SQLiteDatabase.CONFLICT_IGNORE) == -1L) {
                    db.execSQL(
                        "UPDATE history SET visit_count = visit_count + 1, last_visit = ?, " +
                            "title = CASE WHEN ? = '' THEN title ELSE ? END WHERE url = ?",
                        arrayOf<Any>(now, title.orEmpty(), title.orEmpty(), url),
                    )
                }
                db.setTransactionSuccessful()
            } finally {
                db.endTransaction()
            }
        }
    }

    fun updateTitle(url: String, title: String) {
        if (title.isBlank()) return
        writer.execute {
            writableDatabase.execSQL("UPDATE history SET title = ? WHERE url = ?", arrayOf(title, url))
        }
    }

    fun recentHistory(limit: Int = 500, filter: String = ""): List<HistoryEntry> {
        val where = if (filter.isBlank()) null else "url LIKE ? OR title LIKE ?"
        val args = if (filter.isBlank()) null else arrayOf("%$filter%", "%$filter%")
        return readableDatabase.query(
            "history", null, where, args, null, null, "last_visit DESC", limit.toString(),
        ).use { c ->
            buildList {
                while (c.moveToNext()) {
                    add(HistoryEntry(c.getLong(0), c.getString(1), c.getString(2), c.getInt(3), c.getLong(4)))
                }
            }
        }
    }

    /** Omnibox suggestions: prefix-of-host matches first, then frecency. */
    fun suggestHistory(query: String, limit: Int): List<HistoryEntry> {
        val q = query.trim()
        if (q.isEmpty()) return emptyList()
        val like = "%" + q.replace("%", "\\%").replace("_", "\\_") + "%"
        return readableDatabase.rawQuery(
            "SELECT id, url, title, visit_count, last_visit FROM history " +
                "WHERE url LIKE ? ESCAPE '\\' OR title LIKE ? ESCAPE '\\' " +
                "ORDER BY (url LIKE ? OR url LIKE ? OR url LIKE ?) DESC, visit_count DESC, last_visit DESC LIMIT ?",
            arrayOf(like, like, "https://$q%", "https://www.$q%", "http://$q%", limit.toString()),
        ).use { c ->
            buildList {
                while (c.moveToNext()) {
                    add(HistoryEntry(c.getLong(0), c.getString(1), c.getString(2), c.getInt(3), c.getLong(4)))
                }
            }
        }
    }

    fun topSites(limit: Int): List<HistoryEntry> =
        readableDatabase.query("history", null, null, null, null, null, "visit_count DESC, last_visit DESC", limit.toString())
            .use { c ->
                buildList {
                    while (c.moveToNext()) add(HistoryEntry(c.getLong(0), c.getString(1), c.getString(2), c.getInt(3), c.getLong(4)))
                }
            }

    /**
     * Most-visited *sites* for the start page's shortcut tiles: history is
     * folded per registrable domain (every DuckDuckGo search counts towards
     * one "DuckDuckGo" tile), each tile points at the site's root rather than
     * the last page visited there, and is named after the site, not a page
     * title. Ranked by total visits, then by how recently the site was used.
     */
    fun topSitesByDomain(limit: Int): List<TopSite> {
        val rows = readableDatabase.query(
            "history", arrayOf("url", "visit_count", "last_visit"), null, null, null, null,
            "last_visit DESC", "2000",
        ).use { c ->
            buildList { while (c.moveToNext()) add(Triple(c.getString(0), c.getInt(1), c.getLong(2))) }
        }
        class Acc(var visits: Int, var lastVisit: Long, val roots: HashMap<String, Int>)
        val byDomain = LinkedHashMap<String, Acc>()
        for ((url, visits, last) in rows) {
            val root = SiteNames.siteRoot(url) ?: continue
            val host = Omnibox.hostOf(url) ?: continue
            val domain = SiteNames.registrableDomain(host)
            val acc = byDomain.getOrPut(domain) { Acc(0, 0L, HashMap()) }
            acc.visits += visits
            acc.lastVisit = maxOf(acc.lastVisit, last)
            // The host the user actually lands on most within this domain
            // (e.g. www.example.com rather than login.example.com).
            acc.roots[root] = (acc.roots[root] ?: 0) + visits
        }
        return byDomain.entries
            .sortedWith(compareByDescending<Map.Entry<String, Acc>> { it.value.visits }.thenByDescending { it.value.lastVisit })
            .take(limit)
            .map { (domain, acc) ->
                val root = acc.roots.maxByOrNull { it.value }!!.key
                TopSite(root, Omnibox.hostOf(root) ?: domain, SiteNames.forHost(domain), acc.visits, acc.lastVisit)
            }
    }

    fun deleteHistory(id: Long) = writer.execute { writableDatabase.delete("history", "id = ?", arrayOf(id.toString())) }

    fun clearHistory(onDone: (() -> Unit)? = null) = writer.execute {
        writableDatabase.delete("history", null, null)
        onDone?.invoke()
    }

    // --- Bookmarks -----------------------------------------------------------

    fun bookmarks(filter: String = ""): List<Bookmark> {
        val where = if (filter.isBlank()) null else "url LIKE ? OR title LIKE ?"
        val args = if (filter.isBlank()) null else arrayOf("%$filter%", "%$filter%")
        return readableDatabase.query("bookmarks", null, where, args, null, null, "created DESC").use { c ->
            buildList { while (c.moveToNext()) add(Bookmark(c.getLong(0), c.getString(1), c.getString(2), c.getLong(3))) }
        }
    }

    fun isBookmarked(url: String): Boolean =
        readableDatabase.rawQuery("SELECT 1 FROM bookmarks WHERE url = ? LIMIT 1", arrayOf(url)).use { it.moveToFirst() }

    fun addBookmark(url: String, title: String) {
        val cv = ContentValues().apply {
            put("url", url)
            put("title", title)
            put("created", System.currentTimeMillis())
        }
        writableDatabase.insertWithOnConflict("bookmarks", null, cv, SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun updateBookmark(id: Long, url: String, title: String) {
        val cv = ContentValues().apply {
            put("url", url)
            put("title", title)
        }
        writableDatabase.update("bookmarks", cv, "id = ?", arrayOf(id.toString()))
    }

    fun removeBookmark(url: String) {
        writableDatabase.delete("bookmarks", "url = ?", arrayOf(url))
    }

    fun removeBookmarkById(id: Long) {
        writableDatabase.delete("bookmarks", "id = ?", arrayOf(id.toString()))
    }

    companion object {
        @Volatile private var instance: BrowserDb? = null

        fun get(context: Context): BrowserDb =
            instance ?: synchronized(this) { instance ?: BrowserDb(context).also { instance = it } }
    }
}
