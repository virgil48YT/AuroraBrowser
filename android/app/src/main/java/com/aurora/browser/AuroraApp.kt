package com.aurora.browser

import android.app.Application
import com.aurora.browser.adblock.AdblockService
import com.aurora.browser.data.Prefs
import com.aurora.browser.tabs.IncognitoProfile
import com.aurora.browser.tor.TorManager
import com.aurora.browser.ui.Favicons

class AuroraApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Prefs(this).applyTheme()
        // Build the filter engine early, off the main thread.
        AdblockService.get(this)
        // A crash or force-stop can leave incognito data behind; wipe it.
        IncognitoProfile.wipeLeftovers()
        // The favicon fetcher talks to the network directly; never let it
        // bypass Tor, so it only fetches while Tor mode is off.
        Favicons.networkAllowed = { !TorManager.enabled }
        // Resume Tor mode (fail-closed until tor has bootstrapped).
        TorManager.init(this)
    }
}
