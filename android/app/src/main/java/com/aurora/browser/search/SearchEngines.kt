package com.aurora.browser.search

import com.aurora.browser.core.SearchTemplates
import com.aurora.browser.data.Prefs
import org.json.JSONArray
import org.json.JSONObject

data class SearchEngine(
    /** Preference key: a built-in id ("duckduckgo") or "custom:<id>". */
    val key: String,
    val name: String,
    val template: String,
    val isCustom: Boolean,
) {
    val home: String get() = SearchTemplates.home(template) ?: "https://duckduckgo.com/"
}

/**
 * Built-in and user-defined search engines (desktop parity with
 * src/searchengines). DuckDuckGo is first and is the fallback for an
 * unknown or deleted default.
 */
class SearchEngines(private val prefs: Prefs) {

    val builtins: List<SearchEngine> = listOf(
        SearchEngine("duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q=%s", false),
        SearchEngine("google", "Google", "https://www.google.com/search?q=%s", false),
        SearchEngine("bing", "Bing", "https://www.bing.com/search?q=%s", false),
        SearchEngine("startpage", "Startpage", "https://www.startpage.com/sp/search?query=%s", false),
        SearchEngine("brave", "Brave Search", "https://search.brave.com/search?q=%s", false),
        SearchEngine("ecosia", "Ecosia", "https://www.ecosia.org/search?q=%s", false),
    )

    fun custom(): List<SearchEngine> {
        return try {
            val arr = JSONArray(prefs.customEnginesJson)
            (0 until arr.length()).map {
                val o = arr.getJSONObject(it)
                SearchEngine("custom:" + o.getLong("id"), o.getString("name"), o.getString("template"), true)
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    fun all(): List<SearchEngine> = builtins + custom()

    fun default(): SearchEngine = all().firstOrNull { it.key == prefs.searchEngineKey } ?: builtins.first()

    fun setDefault(key: String) {
        prefs.searchEngineKey = key
    }

    /** Adds a custom engine; returns an error message or null on success. */
    fun addCustom(name: String, template: String): String? {
        val n = name.trim()
        val t = template.trim()
        SearchTemplates.validateName(n).let { if (it != SearchTemplates.NameError.OK) return it.message }
        SearchTemplates.validateTemplate(t).let { if (it != SearchTemplates.TemplateError.OK) return it.message }
        val existing = custom()
        if (existing.any { it.name.equals(n, ignoreCase = true) } || builtins.any { it.name.equals(n, true) }) {
            return "An engine with this name already exists."
        }
        val id = (existing.maxOfOrNull { it.key.removePrefix("custom:").toLongOrNull() ?: 0L } ?: 0L) + 1
        val arr = JSONArray(prefs.customEnginesJson.ifEmpty { "[]" })
        arr.put(JSONObject().put("id", id).put("name", n).put("template", t))
        prefs.customEnginesJson = arr.toString()
        return null
    }

    fun removeCustom(key: String) {
        val arr = JSONArray()
        custom().filter { it.key != key }.forEach {
            arr.put(JSONObject().put("id", it.key.removePrefix("custom:").toLong()).put("name", it.name).put("template", it.template))
        }
        prefs.customEnginesJson = arr.toString()
        if (prefs.searchEngineKey == key) prefs.searchEngineKey = builtins.first().key
    }
}
