package com.aurora.browser.tabs

import android.graphics.Bitmap
import android.webkit.WebView
import com.aurora.browser.core.Omnibox
import java.util.concurrent.atomic.AtomicInteger

/**
 * One browser tab. The WebView is created lazily (restored tabs and new
 * tabs on the start page have none until they navigate somewhere).
 *
 * Fields read from WebView network threads (shouldInterceptRequest) are
 * @Volatile / atomic; everything else is main-thread only.
 */
class Tab(
    val id: Long,
    val incognito: Boolean,
    initialUrl: String = Omnibox.HOME_URL,
    initialTitle: String = "",
    /** Tab that opened this one via window.open / target=_blank, if any. */
    val openerId: Long? = null,
) {
    var webView: WebView? = null

    /** Current URL; [Omnibox.HOME_URL] while the start page is shown. */
    @Volatile var url: String = initialUrl

    var title: String = initialTitle
    var favicon: Bitmap? = null

    /** In-memory only (never written to disk, also for normal tabs). */
    var thumbnail: Bitmap? = null

    var progress: Int = 100
    var isLoading: Boolean = false

    /** Host of the top-level page; the "source" for third-party checks. */
    @Volatile var pageHost: String = Omnibox.hostOf(initialUrl).orEmpty()

    /** Ads/trackers blocked on the current page. */
    val blockedOnPage = AtomicInteger(0)

    /** True when the start page was opened over a page (Back returns to it). */
    var homeDetour: Boolean = false

    /** A load was held back while Tor was still connecting; reload when ready. */
    @Volatile var waitingForTor: Boolean = false

    /** Per-tab "request desktop site". */
    var desktopMode: Boolean = false

    val showingHome: Boolean get() = url == Omnibox.HOME_URL

    val displayTitle: String
        get() = when {
            showingHome -> if (incognito) "Incognito tab" else "New tab"
            title.isNotBlank() -> title
            else -> Omnibox.displayUrl(url)
        }
}
