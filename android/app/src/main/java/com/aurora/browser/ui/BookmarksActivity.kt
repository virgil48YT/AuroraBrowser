package com.aurora.browser.ui

import android.os.Bundle
import androidx.core.view.isVisible
import com.aurora.browser.R
import com.aurora.browser.core.Omnibox
import com.aurora.browser.data.Bookmark
import com.aurora.browser.data.BrowserDb
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText

class BookmarksActivity : ListPageActivity() {
    override val searchable = true
    private lateinit var db: BrowserDb
    private lateinit var adapter: RowAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        db = BrowserDb.get(this)
        adapter = RowAdapter(
            onClick = { row -> UiUtil.openInBrowser(this, (row.payload as Bookmark).url, newTab = true); finish() },
            onLongClick = { row -> showRowMenu(row.payload as Bookmark) },
            onAction = { row ->
                db.removeBookmarkById(row.id)
                reload()
            },
        )
        list.adapter = adapter
        fab.setText(R.string.add)
        fab.isVisible = true
        fab.setOnClickListener { editBookmark(null) }
    }

    private fun showRowMenu(b: Bookmark) {
        val items = arrayOf(
            getString(R.string.open_in_new_tab), getString(R.string.open_in_incognito),
            getString(R.string.edit), getString(R.string.delete),
        )
        MaterialAlertDialogBuilder(this)
            .setTitle(b.title.ifBlank { b.url })
            .setItems(items) { _, which ->
                when (which) {
                    0 -> { UiUtil.openInBrowser(this, b.url, newTab = true); finish() }
                    1 -> { UiUtil.openInBrowser(this, b.url, newTab = true, incognito = true); finish() }
                    2 -> editBookmark(b)
                    3 -> { db.removeBookmarkById(b.id); reload() }
                }
            }
            .show()
    }

    private fun editBookmark(existing: Bookmark?) {
        val view = layoutInflater.inflate(R.layout.dialog_bookmark, null)
        val title = view.findViewById<TextInputEditText>(R.id.bookmarkTitle)
        val url = view.findViewById<TextInputEditText>(R.id.bookmarkUrl)
        title.setText(existing?.title.orEmpty())
        url.setText(existing?.url.orEmpty())
        MaterialAlertDialogBuilder(this)
            .setTitle(if (existing == null) R.string.add else R.string.edit)
            .setView(view)
            .setPositiveButton(R.string.save) { _, _ ->
                val input = url.text?.toString().orEmpty()
                val resolved = (Omnibox.classify(input) as? Omnibox.Input.Navigate)?.url ?: return@setPositiveButton
                val name = title.text?.toString().orEmpty().ifBlank { Omnibox.displayUrl(resolved) }
                if (existing == null) db.addBookmark(resolved, name) else db.updateBookmark(existing.id, resolved, name)
                reload()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    override fun reload() {
        val items = db.bookmarks(query)
        adapter.submit(
            items.map { Row(it.id, it.title.ifBlank { it.url }, Omnibox.displayUrl(it.url), R.drawable.ic_bookmark, payload = it) },
        )
        showEmpty(items.isEmpty(), R.string.no_bookmarks)
    }
}
