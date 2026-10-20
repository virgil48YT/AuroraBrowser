package com.aurora.browser.data

import android.content.Context
import android.content.SharedPreferences
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.edit
import androidx.preference.PreferenceManager

/** Typed access to the app's default SharedPreferences (shared with SettingsFragment). */
class Prefs(context: Context) {
    val raw: SharedPreferences = PreferenceManager.getDefaultSharedPreferences(context.applicationContext)

    var theme: String
        get() = raw.getString(KEY_THEME, "system") ?: "system"
        set(v) = raw.edit { putString(KEY_THEME, v) }

    var searchEngineKey: String
        get() = raw.getString(KEY_SEARCH_ENGINE, "duckduckgo") ?: "duckduckgo"
        set(v) = raw.edit { putString(KEY_SEARCH_ENGINE, v) }

    var customEnginesJson: String
        get() = raw.getString(KEY_CUSTOM_ENGINES, "[]") ?: "[]"
        set(v) = raw.edit { putString(KEY_CUSTOM_ENGINES, v) }

    val adblockEnabled: Boolean get() = raw.getBoolean(KEY_ADBLOCK, true)
    val cosmeticFiltering: Boolean get() = raw.getBoolean(KEY_COSMETIC, true)

    var customFilters: String
        get() = raw.getString(KEY_CUSTOM_FILTERS, "") ?: ""
        set(v) = raw.edit { putString(KEY_CUSTOM_FILTERS, v) }

    var shieldsDisabledSites: Set<String>
        get() = raw.getStringSet(KEY_SHIELDS_OFF, emptySet())?.toSet() ?: emptySet()
        set(v) = raw.edit { putStringSet(KEY_SHIELDS_OFF, v) }

    val httpsOnly: Boolean get() = raw.getBoolean(KEY_HTTPS_ONLY, false)
    val blockThirdPartyCookies: Boolean get() = raw.getBoolean(KEY_BLOCK_3P_COOKIES, true)
    val javascriptEnabled: Boolean get() = raw.getBoolean(KEY_JAVASCRIPT, true)
    val restoreTabs: Boolean get() = raw.getBoolean(KEY_RESTORE_TABS, true)
    val saveHistory: Boolean get() = raw.getBoolean(KEY_SAVE_HISTORY, true)
    val showSuggestions: Boolean get() = raw.getBoolean(KEY_SUGGESTIONS, true)
    val desktopMode: Boolean get() = raw.getBoolean(KEY_DESKTOP_MODE, false)

    /** Route all browsing through the bundled Tor (see tor/TorManager). */
    var torEnabled: Boolean
        get() = raw.getBoolean(KEY_TOR, false)
        set(v) = raw.edit { putBoolean(KEY_TOR, v) }

    var sessionJson: String
        get() = raw.getString(KEY_SESSION, "") ?: ""
        set(v) = raw.edit { putString(KEY_SESSION, v) }

    var totalBlocked: Long
        get() = raw.getLong(KEY_TOTAL_BLOCKED, 0L)
        set(v) = raw.edit { putLong(KEY_TOTAL_BLOCKED, v) }

    fun applyTheme() {
        AppCompatDelegate.setDefaultNightMode(
            when (theme) {
                "light" -> AppCompatDelegate.MODE_NIGHT_NO
                "dark" -> AppCompatDelegate.MODE_NIGHT_YES
                else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
            },
        )
    }

    companion object {
        const val KEY_THEME = "theme"
        const val KEY_SEARCH_ENGINE = "search_engine"
        const val KEY_CUSTOM_ENGINES = "custom_search_engines"
        const val KEY_ADBLOCK = "adblock_enabled"
        const val KEY_COSMETIC = "cosmetic_filtering"
        const val KEY_CUSTOM_FILTERS = "custom_filters"
        const val KEY_SHIELDS_OFF = "shields_disabled_sites"
        const val KEY_HTTPS_ONLY = "https_only"
        const val KEY_BLOCK_3P_COOKIES = "block_third_party_cookies"
        const val KEY_JAVASCRIPT = "javascript_enabled"
        const val KEY_RESTORE_TABS = "restore_tabs"
        const val KEY_SAVE_HISTORY = "save_history"
        const val KEY_SUGGESTIONS = "omnibox_suggestions"
        const val KEY_DESKTOP_MODE = "desktop_mode"
        const val KEY_SESSION = "session_tabs"
        const val KEY_TOTAL_BLOCKED = "total_blocked"
        const val KEY_TOR = "tor_enabled"
        const val KEY_TOR_NEW_IDENTITY = "tor_new_identity"
    }
}
