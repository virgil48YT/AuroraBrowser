package com.aurora.browser.tabs

import android.annotation.SuppressLint
import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.MutableContextWrapper
import android.graphics.Bitmap
import android.net.Uri
import android.os.Message
import android.util.Log
import android.view.View
import android.webkit.CookieManager
import android.webkit.RenderProcessGoneDetail
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import com.aurora.browser.adblock.AdblockService
import com.aurora.browser.adblock.RequestClassifier
import com.aurora.browser.adblock.ResourceType
import com.aurora.browser.core.Omnibox
import com.aurora.browser.data.Prefs
import com.aurora.browser.tor.TorManager
import java.io.ByteArrayInputStream
import java.net.URISyntaxException

/** Callbacks from a tab's WebView into the browser UI. */
interface BrowserHost {
    val activity: Activity
    fun onTabStateChanged(tab: Tab)
    fun onTabProgress(tab: Tab)
    /** May be called from any thread. */
    fun onTabBlockedCountChanged(tab: Tab)
    fun onPageFinished(tab: Tab, url: String)
    fun openPopupTab(opener: Tab, resultMsg: Message): Boolean
    fun closeTab(tab: Tab)
    fun showCustomView(view: View, callback: WebChromeClient.CustomViewCallback)
    fun hideCustomView()
    fun showFileChooser(callback: ValueCallback<Array<Uri>>, params: WebChromeClient.FileChooserParams): Boolean
    fun startDownload(tab: Tab, url: String, userAgent: String, contentDisposition: String, mimeType: String)
    fun onRenderProcessGone(tab: Tab)
}

/** Creates and configures the WebView behind a [Tab]. */
object BrowserWebView {
    private const val TAG = "AuroraWebView"

    private val emptyResponse: WebResourceResponse
        get() = WebResourceResponse("text/plain", "utf-8", ByteArrayInputStream(ByteArray(0)))

    private val blockedResponse: WebResourceResponse
        get() = WebResourceResponse(
            "text/plain", "utf-8", 503, "Tor not ready", mapOf("Cache-Control" to "no-store"),
            ByteArrayInputStream(ByteArray(0)),
        )

    /** Shown instead of a page while Tor mode is on but tor is still connecting. */
    private fun torWaitingPage(): WebResourceResponse {
        val html = """<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
            |<title>Connecting to Tor</title><style>
            |body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
            |background:#050811;color:#F1F5F9;font:16px/1.5 sans-serif;text-align:center}
            |.c{max-width:22em;padding:24px}.o{font-size:44px;color:#A855F7}h1{font-size:20px;margin:8px 0}
            |p{color:#94A3B8;margin:0}</style></head><body><div class="c"><div class="o">&#9673;</div>
            |<h1>Connecting to Tor&hellip;</h1>
            |<p>Tor mode is on. This page will load as soon as a Tor circuit is ready.
            |Nothing is sent outside Tor in the meantime.</p></div></body></html>""".trimMargin()
        return WebResourceResponse(
            "text/html", "utf-8", 503, "Tor not ready", mapOf("Cache-Control" to "no-store"),
            ByteArrayInputStream(html.toByteArray(Charsets.UTF_8)),
        )
    }

    @SuppressLint("SetJavaScriptEnabled")
    fun create(activity: Activity, tab: Tab): WebView {
        val prefs = Prefs(activity)
        val adblock = AdblockService.get(activity)
        // MutableContextWrapper lets the WebView survive activity recreation
        // (theme switch): the new activity re-points the base context.
        val webView = WebView(MutableContextWrapper(activity))
        // Profile must be set before the WebView is used in any way.
        if (tab.incognito) IncognitoProfile.attach(webView)

        webView.settings.apply {
            javaScriptEnabled = prefs.javascriptEnabled
            domStorageEnabled = true
            loadWithOverviewMode = true
            useWideViewPort = true
            builtInZoomControls = true
            displayZoomControls = false
            setSupportZoom(true)
            setSupportMultipleWindows(true)
            javaScriptCanOpenWindowsAutomatically = false
            mediaPlaybackRequiresUserGesture = true
            mixedContentMode = WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
            allowFileAccess = false
            allowContentAccess = false
            cacheMode = if (tab.incognito) WebSettings.LOAD_NO_CACHE else WebSettings.LOAD_DEFAULT
            tab.desktopMode = tab.desktopMode || prefs.desktopMode
            applyUserAgent(activity, this, tab.desktopMode)
        }
        val cookies = if (tab.incognito) IncognitoProfile.cookieManager() else CookieManager.getInstance()
        cookies.setAcceptCookie(true)
        cookies.setAcceptThirdPartyCookies(webView, !prefs.blockThirdPartyCookies)

        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                val url = request.url.toString()
                if (url == Omnibox.HOME_URL) {
                    tab.url = Omnibox.HOME_URL
                    BrowserState.host?.onTabStateChanged(tab)
                    return true
                }
                val scheme = request.url.scheme?.lowercase().orEmpty()
                if (scheme == "http" && prefs.httpsOnly && request.isForMainFrame) {
                    val upgraded = Omnibox.upgradeHttp(url)
                    if (upgraded != url) {
                        view.loadUrl(upgraded)
                        return true
                    }
                }
                if (scheme == "http" || scheme == "https" || scheme == "about" || scheme == "data" || scheme == "blob") {
                    return false
                }
                if (scheme == "javascript") return false
                val current = BrowserState.host?.activity ?: return true
                return openExternal(current, view, url)
            }

            override fun onPageStarted(view: WebView, url: String, favicon: Bitmap?) {
                // A detached WebView behind the start page must not pull the tab back.
                if (tab.showingHome && view.parent == null) return
                if (url != tab.url) tab.favicon = favicon
                tab.url = url
                tab.pageHost = Omnibox.hostOf(url).orEmpty()
                tab.isLoading = true
                BrowserState.host?.onTabStateChanged(tab)
            }

            override fun onPageCommitVisible(view: WebView, url: String) {
                injectCosmetic(view, tab, adblock)
            }

            override fun onPageFinished(view: WebView, url: String) {
                if (tab.showingHome && view.parent == null) return
                tab.isLoading = false
                injectCosmetic(view, tab, adblock)
                BrowserState.host?.onPageFinished(tab, url)
            }

            override fun doUpdateVisitedHistory(view: WebView, url: String, isReload: Boolean) {
                if (tab.showingHome && view.parent == null) return
                if (url != tab.url && !url.startsWith("data:")) {
                    tab.url = url
                    tab.pageHost = Omnibox.hostOf(url).orEmpty()
                    BrowserState.host?.onTabStateChanged(tab)
                }
            }

            override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? {
                val url = request.url.toString()
                if (TorManager.enabled && !TorManager.ready) {
                    // Tor mode is on but tor is not usable yet: fail closed.
                    if (request.isForMainFrame) {
                        tab.waitingForTor = true
                        return torWaitingPage()
                    }
                    return blockedResponse
                }
                val reqHost = request.url.host?.lowercase() ?: return null
                if (request.isForMainFrame) {
                    // New top-level document: new page for Shields purposes.
                    tab.pageHost = reqHost
                    tab.blockedOnPage.set(0)
                    BrowserState.host?.onTabBlockedCountChanged(tab)
                    return null
                }
                if (!adblock.shieldsUp(tab.pageHost)) return null
                val type = RequestClassifier.classify(
                    url, request.requestHeaders["Accept"], false, request.requestHeaders["X-Requested-With"],
                )
                if (type == ResourceType.DOCUMENT) return null
                val engine = adblock.engine
                return if (engine.shouldBlock(url, reqHost, tab.pageHost, type)) {
                    tab.blockedOnPage.incrementAndGet()
                    adblock.noteBlocked()
                    BrowserState.host?.onTabBlockedCountChanged(tab)
                    emptyResponse
                } else {
                    null
                }
            }

            override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
                Log.w(TAG, "renderer gone for tab ${tab.id} (crash=${detail.didCrash()})")
                BrowserState.host?.onRenderProcessGone(tab)
                return true
            }
        }

        webView.webChromeClient = object : WebChromeClient() {
            override fun onProgressChanged(view: WebView, newProgress: Int) {
                tab.progress = newProgress
                BrowserState.host?.onTabProgress(tab)
            }

            override fun onReceivedTitle(view: WebView, title: String?) {
                tab.title = title.orEmpty()
                BrowserState.host?.onTabStateChanged(tab)
            }

            override fun onReceivedIcon(view: WebView, icon: Bitmap?) {
                tab.favicon = icon
                // Feed the start page / history favicon cache (normal profile only).
                if (!tab.incognito && icon != null) {
                    com.aurora.browser.ui.Favicons.remember(view.context, view.url ?: tab.url, icon)
                }
                BrowserState.host?.onTabStateChanged(tab)
            }

            override fun onCreateWindow(view: WebView, isDialog: Boolean, isUserGesture: Boolean, resultMsg: Message): Boolean {
                // Popups without a user gesture are blocked (desktop parity).
                if (!isUserGesture) return false
                return BrowserState.host?.openPopupTab(tab, resultMsg) ?: false
            }

            override fun onCloseWindow(window: WebView) {
                BrowserState.host?.closeTab(tab)
            }

            override fun onShowCustomView(view: View, callback: CustomViewCallback) {
                BrowserState.host?.showCustomView(view, callback)
            }

            override fun onHideCustomView() {
                BrowserState.host?.hideCustomView()
            }

            override fun onShowFileChooser(
                webView: WebView,
                filePathCallback: ValueCallback<Array<Uri>>,
                fileChooserParams: FileChooserParams,
            ): Boolean = BrowserState.host?.showFileChooser(filePathCallback, fileChooserParams) ?: false
        }

        webView.setDownloadListener { url, userAgent, contentDisposition, mimeType, _ ->
            if (url != null) {
                BrowserState.host?.startDownload(tab, url, userAgent.orEmpty(), contentDisposition.orEmpty(), mimeType.orEmpty())
            }
        }
        return webView
    }

    fun applyUserAgent(context: Context, settings: WebSettings, desktop: Boolean) {
        settings.userAgentString = if (desktop) {
            WebSettings.getDefaultUserAgent(context).replace(Regex("\\(Linux; Android [^)]*\\)"), "(X11; Linux x86_64)")
                .replace(" Mobile", "")
                .replace(Regex("; wv\\)"), ")")
        } else {
            null // WebView default
        }
    }

    private fun injectCosmetic(view: WebView, tab: Tab, adblock: AdblockService) {
        if (!adblock.cosmeticEnabled || !adblock.shieldsUp(tab.pageHost) || tab.pageHost.isEmpty()) return
        val css = adblock.engine.cosmeticCss(tab.pageHost)
        if (css.isEmpty()) return
        val js = "(function(){try{var id='aurora-shields-css';if(document.getElementById(id))return;" +
            "var s=document.createElement('style');s.id=id;s.textContent=" + jsString(css) + ";" +
            "(document.head||document.documentElement).appendChild(s);}catch(e){}})();"
        view.evaluateJavascript(js, null)
    }

    private fun jsString(s: String): String {
        val sb = StringBuilder("\"")
        for (c in s) {
            when (c) {
                '\\' -> sb.append("\\\\")
                '"' -> sb.append("\\\"")
                '\n' -> sb.append("\\n")
                '\r' -> sb.append("\\r")
                '<' -> sb.append("\\u003c")
                '\u2028' -> sb.append("\\u2028")
                '\u2029' -> sb.append("\\u2029")
                else -> sb.append(c)
            }
        }
        return sb.append('"').toString()
    }

    /** intent:, mailto:, tel:, market: ... hand off to other apps. */
    private fun openExternal(activity: Activity, view: WebView, url: String): Boolean {
        try {
            val intent = if (url.startsWith("intent:", ignoreCase = true)) {
                Intent.parseUri(url, Intent.URI_INTENT_SCHEME).apply {
                    addCategory(Intent.CATEGORY_BROWSABLE)
                    component = null
                    selector = null
                }
            } else {
                Intent(Intent.ACTION_VIEW, Uri.parse(url)).addCategory(Intent.CATEGORY_BROWSABLE)
            }
            try {
                activity.startActivity(intent)
            } catch (_: ActivityNotFoundException) {
                val fallback = intent.getStringExtra("browser_fallback_url")
                if (fallback != null && Omnibox.isWebUrl(fallback)) view.loadUrl(fallback)
            }
        } catch (e: URISyntaxException) {
            Log.w(TAG, "bad intent URI: $url")
        }
        return true
    }
}
