package com.aurora.browser.ui

import android.app.Activity
import android.app.DownloadManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.util.Base64
import android.util.Log
import android.webkit.CookieManager
import android.webkit.MimeTypeMap
import android.webkit.URLUtil
import android.widget.Toast
import com.aurora.browser.R
import java.io.File

/** Hands web downloads to the system DownloadManager. */
object Downloads {
    private const val TAG = "AuroraDownloads"

    fun start(
        activity: Activity,
        url: String,
        userAgent: String,
        contentDisposition: String,
        mimeType: String,
        cookies: CookieManager,
    ) {
        if (url.startsWith("data:", ignoreCase = true)) {
            saveDataUrl(activity, url, mimeType)
            return
        }
        if (!url.startsWith("http://", true) && !url.startsWith("https://", true)) {
            Toast.makeText(activity, R.string.download_unsupported, Toast.LENGTH_SHORT).show()
            return
        }
        try {
            val fileName = URLUtil.guessFileName(url, contentDisposition, mimeType.ifEmpty { null })
            val request = DownloadManager.Request(Uri.parse(url)).apply {
                setTitle(fileName)
                setMimeType(mimeType.ifEmpty { guessMime(fileName) })
                addRequestHeader("User-Agent", userAgent)
                cookies.getCookie(url)?.let { addRequestHeader("Cookie", it) }
                setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    // Public Downloads needs no storage permission on Android 10+.
                    setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, fileName)
                } else {
                    // Older releases: app-specific external dir, also permission-free.
                    setDestinationInExternalFilesDir(activity, Environment.DIRECTORY_DOWNLOADS, fileName)
                }
            }
            val dm = activity.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
            dm.enqueue(request)
            Toast.makeText(activity, activity.getString(R.string.download_started, fileName), Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Log.w(TAG, "download failed", e)
            Toast.makeText(activity, R.string.download_failed, Toast.LENGTH_SHORT).show()
        }
    }

    private fun guessMime(fileName: String): String? =
        MimeTypeMap.getSingleton().getMimeTypeFromExtension(fileName.substringAfterLast('.', "").lowercase())

    /** data: URLs (e.g. canvas exports) are decoded locally into the app's Downloads dir. */
    private fun saveDataUrl(activity: Activity, url: String, mimeType: String) {
        try {
            val header = url.substringBefore(',')
            val payload = url.substringAfter(',', "")
            val mime = header.removePrefix("data:").substringBefore(';').ifEmpty { mimeType.ifEmpty { "application/octet-stream" } }
            val bytes = if (header.contains(";base64")) Base64.decode(payload, Base64.DEFAULT) else Uri.decode(payload).toByteArray()
            val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime) ?: "bin"
            val dir = activity.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: activity.filesDir
            val file = File(dir, "download-${System.currentTimeMillis()}.$ext")
            file.writeBytes(bytes)
            val dm = activity.getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
            @Suppress("DEPRECATION")
            dm.addCompletedDownload(file.name, file.name, true, mime, file.absolutePath, file.length(), true)
            Toast.makeText(activity, activity.getString(R.string.download_started, file.name), Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Log.w(TAG, "data: download failed", e)
            Toast.makeText(activity, R.string.download_failed, Toast.LENGTH_SHORT).show()
        }
    }
}
