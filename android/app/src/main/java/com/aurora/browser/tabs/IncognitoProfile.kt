package com.aurora.browser.tabs

import android.annotation.SuppressLint
import android.util.Log
import android.webkit.CookieManager
import android.webkit.WebView
import androidx.webkit.ProfileStore
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature

/**
 * Incognito isolation. When the installed WebView supports multiple
 * profiles (androidx.webkit MULTI_PROFILE, WebView 123+), incognito tabs run
 * in a dedicated profile with its own cookie jar, storage and cache, which
 * is deleted as soon as the last incognito tab closes (and again on the next
 * launch in case the process was killed).
 *
 * Without that feature incognito tabs share the default cookie jar; they
 * still never write history, session state, thumbnails or HTTP cache
 * (LOAD_NO_CACHE). See android/README.md "Limitations".
 */
// Every ProfileStore call is guarded by [isolated] (WebViewFeature.MULTI_PROFILE),
// which lint cannot see through.
@SuppressLint("RequiresFeature")
object IncognitoProfile {
    private const val TAG = "AuroraIncognito"
    const val NAME = "aurora-incognito"

    val isolated: Boolean by lazy {
        try {
            WebViewFeature.isFeatureSupported(WebViewFeature.MULTI_PROFILE)
        } catch (t: Throwable) {
            false
        }
    }

    /** Must be called right after constructing the WebView, before any use. */
    fun attach(webView: WebView) {
        if (!isolated) return
        try {
            ProfileStore.getInstance().getOrCreateProfile(NAME)
            WebViewCompat.setProfile(webView, NAME)
        } catch (t: Throwable) {
            Log.w(TAG, "could not attach incognito profile", t)
        }
    }

    fun cookieManager(): CookieManager =
        if (isolated) {
            try {
                ProfileStore.getInstance().getOrCreateProfile(NAME).cookieManager
            } catch (t: Throwable) {
                CookieManager.getInstance()
            }
        } else {
            CookieManager.getInstance()
        }

    /** Delete all incognito data. Call only when no incognito WebView exists. */
    fun wipe() {
        if (!isolated) return
        try {
            val store = ProfileStore.getInstance()
            val profile = store.getProfile(NAME) ?: return
            // Clear first: deleteProfile fails while a destroyed WebView is
            // still being torn down, and clearing guarantees no data survives.
            profile.cookieManager.removeAllCookies(null)
            profile.webStorage.deleteAllData()
            if (!store.deleteProfile(NAME)) Log.i(TAG, "profile not deleted (still in use?); data cleared")
        } catch (t: Throwable) {
            Log.w(TAG, "incognito wipe failed", t)
        }
    }

    /** Called on startup: no incognito WebView can exist yet. */
    fun wipeLeftovers() {
        // Accessing ProfileStore loads WebView; only do it if a profile may exist.
        if (!isolated) return
        try {
            if (ProfileStore.getInstance().allProfileNames.contains(NAME)) wipe()
        } catch (t: Throwable) {
            Log.w(TAG, "startup wipe failed", t)
        }
    }
}
