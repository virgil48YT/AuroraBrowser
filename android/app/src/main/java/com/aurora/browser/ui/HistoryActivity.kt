package com.aurora.browser.ui

import android.os.Bundle
import com.aurora.browser.R
import com.aurora.browser.core.Omnibox
import com.aurora.browser.data.BrowserDb
import com.aurora.browser.data.HistoryEntry
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class HistoryActivity : ListPageActivity() {
    override val searchable = true
    private lateinit var db: BrowserDb
    private lateinit var adapter: RowAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        db = BrowserDb.get(this)
        adapter = RowAdapter(
            onClick = { row -> UiUtil.openInBrowser(this, (row.payload as HistoryEntry).url, newTab = true); finish() },
            onLongClick = { row -> showRowMenu(row.payload as HistoryEntry) },
            onAction = { row ->
                db.deleteHistory(row.id)
                list.postDelayed({ reload() }, 100)
            },
        )
        list.adapter = adapter
        toolbar.inflateMenu(R.menu.history_menu)
        toolbar.setOnMenuItemClickListener {
            if (it.itemId == R.id.action_clear_history) {
                MaterialAlertDialogBuilder(this)
                    .setMessage(R.string.clear_history_confirm)
                    .setPositiveButton(R.string.clear) { _, _ -> db.clearHistory { runOnUiThread { reload() } } }
                    .setNegativeButton(R.string.cancel, null)
                    .show()
            }
            true
        }
    }

    private fun showRowMenu(entry: HistoryEntry) {
        val items = arrayOf(getString(R.string.open_in_new_tab), getString(R.string.open_in_incognito), getString(R.string.delete))
        MaterialAlertDialogBuilder(this)
            .setTitle(entry.title.ifBlank { entry.url })
            .setItems(items) { _, which ->
                when (which) {
                    0 -> { UiUtil.openInBrowser(this, entry.url, newTab = true); finish() }
                    1 -> { UiUtil.openInBrowser(this, entry.url, newTab = true, incognito = true); finish() }
                    2 -> { db.deleteHistory(entry.id); list.postDelayed({ reload() }, 100) }
                }
            }
            .show()
    }

    override fun reload() {
        val entries = db.recentHistory(1000, query)
        adapter.submit(
            entries.map {
                Row(
                    it.id,
                    it.title.ifBlank { Omnibox.displayUrl(it.url) },
                    "${Omnibox.displayUrl(it.url)}\n${UiUtil.relativeTime(it.lastVisit)}",
                    R.drawable.ic_history,
                    payload = it,
                    faviconUrl = it.url,
                )
            },
        )
        showEmpty(entries.isEmpty(), R.string.no_history_page)
    }
}
