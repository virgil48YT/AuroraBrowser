package com.aurora.browser

import android.app.Application
import com.aurora.browser.adblock.AdblockService
import com.aurora.browser.data.Prefs
import com.aurora.browser.tabs.IncognitoProfile

class AuroraApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Prefs(this).applyTheme()
        // Build the filter engine early, off the main thread.
        AdblockService.get(this)
        // A crash or force-stop can leave incognito data behind; wipe it.
        IncognitoProfile.wipeLeftovers()
    }
}
