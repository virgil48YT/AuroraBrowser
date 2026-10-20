package com.aurora.browser.ui

import android.content.Intent
import android.os.Bundle
import android.webkit.CookieManager
import android.webkit.WebStorage
import android.webkit.WebView
import android.widget.EditText
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.webkit.WebViewCompat
import com.aurora.browser.BuildConfig
import com.aurora.browser.R
import com.aurora.browser.adblock.AdblockService
import com.aurora.browser.data.BrowserDb
import com.aurora.browser.data.Prefs
import com.aurora.browser.search.SearchEngines
import com.aurora.browser.tabs.BrowserState
import com.aurora.browser.tabs.IncognitoProfile
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class SettingsActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(R.layout.activity_settings)
        UiUtil.applySystemBarInsets(findViewById(R.id.pageRoot))
        findViewById<com.google.android.material.appbar.MaterialToolbar>(R.id.pageToolbar)
            .setNavigationOnClickListener { finish() }
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction().replace(R.id.settingsContainer, SettingsFragment()).commit()
        }
    }
}

class SettingsFragment : PreferenceFragmentCompat() {
    private lateinit var prefs: Prefs
    private lateinit var adblock: AdblockService

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.preferences, rootKey)
        prefs = Prefs(requireContext())
        adblock = AdblockService.get(requireContext())

        findPreference<Preference>("search_engine_screen")?.setOnPreferenceClickListener {
            startActivity(Intent(requireContext(), SearchEnginesActivity::class.java))
            true
        }
        findPreference<Preference>(Prefs.KEY_THEME)?.setOnPreferenceChangeListener { _, value ->
            prefs.theme = value as String
            prefs.applyTheme()
            true
        }
        findPreference<Preference>("custom_filters_screen")?.setOnPreferenceClickListener {
            editCustomFilters()
            true
        }
        findPreference<Preference>("shields_exceptions")?.setOnPreferenceClickListener {
            showShieldsExceptions()
            true
        }
        findPreference<Preference>("clear_data")?.setOnPreferenceClickListener {
            clearBrowsingData()
            true
        }
        val restartNote = Preference.OnPreferenceChangeListener { _, _ ->
            Toast.makeText(requireContext(), R.string.restart_to_apply, Toast.LENGTH_SHORT).show()
            true
        }
        findPreference<Preference>(Prefs.KEY_JAVASCRIPT)?.onPreferenceChangeListener = restartNote
        findPreference<Preference>(Prefs.KEY_DESKTOP_MODE)?.onPreferenceChangeListener = restartNote
        findPreference<Preference>(Prefs.KEY_BLOCK_3P_COOKIES)?.onPreferenceChangeListener = restartNote

        findPreference<Preference>("about_version")?.summary = BuildConfig.VERSION_NAME
        val webViewPackage = WebViewCompat.getCurrentWebViewPackage(requireContext())
        findPreference<Preference>("about_engine")?.summary =
            if (webViewPackage != null) {
                "Android System WebView (Chromium) ${webViewPackage.versionName}\n${webViewPackage.packageName}"
            } else {
                "Android System WebView (Chromium)"
            }
    }

    override fun onResume() {
        super.onResume()
        findPreference<Preference>("search_engine_screen")?.summary = SearchEngines(prefs).default().name
        val n = adblock.disabledSiteList().size
        findPreference<Preference>("shields_exceptions")?.summary =
            if (n == 0) getString(R.string.no_exceptions) else adblock.disabledSiteList().joinToString(", ")
    }

    private fun editCustomFilters() {
        val view = layoutInflater.inflate(R.layout.dialog_edit_text, null)
        val input = view.findViewById<EditText>(R.id.dialogInput)
        input.setText(prefs.customFilters)
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.pref_custom_filters)
            .setView(view)
            .setPositiveButton(R.string.save) { _, _ ->
                prefs.customFilters = input.text.toString()
                val ctx = requireContext().applicationContext
                adblock.rebuild {
                    val count = adblock.engine.networkRuleCount + adblock.engine.cosmeticRuleCount
                    input.post { Toast.makeText(ctx, ctx.getString(R.string.filters_saved, count), Toast.LENGTH_SHORT).show() }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showShieldsExceptions() {
        val sites = adblock.disabledSiteList()
        if (sites.isEmpty()) {
            Toast.makeText(requireContext(), R.string.no_exceptions, Toast.LENGTH_SHORT).show()
            return
        }
        val checked = BooleanArray(sites.size) { true }
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.pref_shields_exceptions)
            .setMultiChoiceItems(sites.toTypedArray(), checked) { _, which, isChecked -> checked[which] = isChecked }
            .setPositiveButton(R.string.save) { _, _ ->
                // Unchecked sites get Shields back up.
                sites.forEachIndexed { i, site -> if (!checked[i]) adblock.setShields(site, true) }
                onResume()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun clearBrowsingData() {
        val labels = arrayOf(
            getString(R.string.clear_data_history), getString(R.string.clear_data_cookies), getString(R.string.clear_data_cache),
        )
        val checked = booleanArrayOf(true, false, true)
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.pref_clear_data)
            .setMultiChoiceItems(labels, checked) { _, which, isChecked -> checked[which] = isChecked }
            .setPositiveButton(R.string.clear) { _, _ ->
                val ctx = requireContext()
                if (checked[0]) {
                    BrowserDb.get(ctx).clearHistory()
                    // Site icons reveal which sites were visited; drop them with history.
                    Favicons.clear(ctx)
                }
                if (checked[1]) {
                    CookieManager.getInstance().removeAllCookies(null)
                    CookieManager.getInstance().flush()
                    WebStorage.getInstance().deleteAllData()
                }
                if (checked[2]) {
                    // clearCache is process-wide; any live WebView will do, else a throwaway one.
                    val wv = BrowserState.tabs.tabs.firstNotNullOfOrNull { it.webView } ?: WebView(ctx)
                    wv.clearCache(true)
                    if (wv.parent == null && BrowserState.tabs.tabs.none { it.webView === wv }) wv.destroy()
                }
                if (BrowserState.tabs.incognitoTabs().isEmpty()) IncognitoProfile.wipe()
                Toast.makeText(ctx, R.string.cleared, Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }
}
