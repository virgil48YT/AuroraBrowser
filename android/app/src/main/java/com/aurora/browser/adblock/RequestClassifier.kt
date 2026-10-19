package com.aurora.browser.adblock

import java.util.Locale

/**
 * Android's WebResourceRequest carries no resource type, so infer one from
 * the URL's extension and the Accept header (what Chromium itself sends per
 * destination). Unknown requests are "other"/"xmlhttprequest"; rules with
 * explicit type options only fire when the inference agrees, which errs on
 * the side of allowing.
 */
object RequestClassifier {
    private val scriptExt = setOf("js", "mjs")
    private val styleExt = setOf("css")
    private val imageExt = setOf("png", "jpg", "jpeg", "gif", "webp", "svg", "ico", "bmp", "avif")
    private val fontExt = setOf("woff", "woff2", "ttf", "otf", "eot")
    private val mediaExt = setOf("mp4", "webm", "m3u8", "mpd", "mp3", "ogg", "m4a", "aac", "ts", "m4s")

    fun classify(url: String, accept: String?, isMainFrame: Boolean, requestedWith: String? = null): ResourceType {
        if (isMainFrame) return ResourceType.DOCUMENT
        val path = url.substringBefore('#').substringBefore('?')
        val ext = path.substringAfterLast('/').substringAfterLast('.', "").lowercase(Locale.ROOT)
        when (ext) {
            in scriptExt -> return ResourceType.SCRIPT
            in styleExt -> return ResourceType.STYLESHEET
            in imageExt -> return ResourceType.IMAGE
            in fontExt -> return ResourceType.FONT
            in mediaExt -> return ResourceType.MEDIA
        }
        val a = accept?.lowercase(Locale.ROOT).orEmpty()
        return when {
            a.startsWith("text/css") -> ResourceType.STYLESHEET
            a.startsWith("image/") -> ResourceType.IMAGE
            a.startsWith("text/html") -> ResourceType.SUBDOCUMENT
            a.startsWith("video/") || a.startsWith("audio/") -> ResourceType.MEDIA
            requestedWith.equals("XMLHttpRequest", ignoreCase = true) -> ResourceType.XMLHTTPREQUEST
            a.contains("application/json") -> ResourceType.XMLHTTPREQUEST
            url.startsWith("ws://") || url.startsWith("wss://") -> ResourceType.WEBSOCKET
            else -> ResourceType.OTHER
        }
    }
}
