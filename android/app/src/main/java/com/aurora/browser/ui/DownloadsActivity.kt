package com.aurora.browser.ui

import android.app.DownloadManager
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.format.Formatter
import android.widget.Toast
import com.aurora.browser.R

/** Lists this app's DownloadManager downloads (DownloadManager only returns our own). */
class DownloadsActivity : ListPageActivity() {
    private lateinit var dm: DownloadManager
    private lateinit var adapter: RowAdapter

    private data class Item(val id: Long, val title: String, val status: Int, val size: Long, val done: Long, val mime: String?)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        dm = getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
        adapter = RowAdapter(
            onClick = { row -> open(row.payload as Item) },
            onAction = { row ->
                dm.remove(row.id)
                reload()
            },
        )
        list.adapter = adapter
        toolbar.inflateMenu(R.menu.downloads_menu)
        toolbar.setOnMenuItemClickListener {
            if (it.itemId == R.id.action_system_downloads) {
                try {
                    startActivity(Intent(DownloadManager.ACTION_VIEW_DOWNLOADS))
                } catch (_: ActivityNotFoundException) {
                }
            }
            true
        }
    }

    private fun open(item: Item) {
        if (item.status != DownloadManager.STATUS_SUCCESSFUL) return
        val uri = dm.getUriForDownloadedFile(item.id) ?: return
        val intent = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, item.mime ?: dm.getMimeTypeForDownloadedFile(item.id))
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        try {
            startActivity(intent)
        } catch (_: ActivityNotFoundException) {
            Toast.makeText(this, R.string.no_app_for_link, Toast.LENGTH_SHORT).show()
        }
    }

    override fun reload() {
        val items = ArrayList<Item>()
        dm.query(DownloadManager.Query()).use { c ->
            val idCol = c.getColumnIndexOrThrow(DownloadManager.COLUMN_ID)
            val titleCol = c.getColumnIndexOrThrow(DownloadManager.COLUMN_TITLE)
            val statusCol = c.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS)
            val sizeCol = c.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES)
            val doneCol = c.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR)
            val mimeCol = c.getColumnIndexOrThrow(DownloadManager.COLUMN_MEDIA_TYPE)
            while (c.moveToNext()) {
                items.add(
                    Item(c.getLong(idCol), c.getString(titleCol).orEmpty(), c.getInt(statusCol),
                        c.getLong(sizeCol), c.getLong(doneCol), c.getString(mimeCol)),
                )
            }
        }
        items.sortByDescending { it.id }
        adapter.submit(
            items.map {
                val status = when (it.status) {
                    DownloadManager.STATUS_SUCCESSFUL -> Formatter.formatShortFileSize(this, it.size)
                    DownloadManager.STATUS_FAILED -> getString(R.string.download_failed)
                    DownloadManager.STATUS_RUNNING, DownloadManager.STATUS_PENDING, DownloadManager.STATUS_PAUSED -> {
                        val total = if (it.size > 0) " / " + Formatter.formatShortFileSize(this, it.size) else ""
                        Formatter.formatShortFileSize(this, it.done) + total
                    }
                    else -> ""
                }
                Row(it.id, it.title, status, R.drawable.ic_download, payload = it)
            },
        )
        showEmpty(items.isEmpty(), R.string.no_downloads)
    }
}
