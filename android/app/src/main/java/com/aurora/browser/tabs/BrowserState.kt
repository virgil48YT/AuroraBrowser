package com.aurora.browser.tabs

/**
 * Process-wide browser state. Tabs (and their WebViews) outlive a single
 * MainActivity instance so a theme change or other recreation does not
 * reload pages or drop incognito tabs.
 */
object BrowserState {
    val tabs = TabManager()

    /** The live MainActivity, or null while none is attached. */
    @Volatile var host: BrowserHost? = null
}
