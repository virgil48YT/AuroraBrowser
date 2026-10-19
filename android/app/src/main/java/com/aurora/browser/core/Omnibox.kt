package com.aurora.browser.core

import java.net.URI
import java.util.Locale

/**
 * Omnibox input classification — a Kotlin port of the desktop rules in
 * src/core/Url.cpp: "has a scheme, or looks like a bare domain with no
 * spaces -> URL; anything else -> search query".
 *
 * Pure Kotlin (no android.* imports) so it is covered by JVM unit tests.
 */
object Omnibox {

    /** Internal page shown for new tabs (the Aurora start page). */
    const val HOME_URL = "aurora://home"

    sealed class Input {
        /** A navigable URL (already normalized, e.g. "https://example.com/"). */
        data class Navigate(val url: String) : Input()

        /** Free text to send to the search engine. */
        data class Search(val terms: String) : Input()

        /** Nothing typed. */
        data object Empty : Input()
    }

    private val schemeRegex = Regex("^[a-zA-Z][a-zA-Z0-9+.\\-]*:")
    private val ipv4Regex = Regex("^\\d{1,3}(\\.\\d{1,3}){3}$")

    /** Schemes that are always taken literally when typed. */
    private val literalSchemes = setOf(
        "http", "https", "file", "about", "data", "javascript", "aurora",
        "view-source", "mailto", "tel", "intent", "market", "geo", "sms",
    )

    fun classify(raw: String): Input {
        val text = raw.trim()
        if (text.isEmpty()) return Input.Empty
        if (text.any { it.isWhitespace() }) {
            // "view-source: x" etc. are not URLs either; a space means a query.
            return Input.Search(text)
        }

        val schemeMatch = schemeRegex.find(text)
        if (schemeMatch != null) {
            val scheme = schemeMatch.value.dropLast(1).lowercase(Locale.ROOT)
            if (scheme in literalSchemes) {
                if (scheme == "http" || scheme == "https") {
                    val host = hostOf(text)
                    return if (host.isNullOrEmpty()) Input.Search(text) else Input.Navigate(text)
                }
                return Input.Navigate(text)
            }
            // "localhost:8080" or "example.com:8443/x" parse as a scheme too.
            val hostPort = text.substringBefore('/')
            val maybePort = hostPort.substringAfter(':', "")
            if (maybePort.isNotEmpty() && maybePort.all { it.isDigit() }) {
                val host = hostPort.substringBefore(':')
                if (looksLikeHost(host)) return Input.Navigate("http://$text".let(::defaultScheme))
            }
            return Input.Search(text)
        }

        var candidate = text
        if (candidate.endsWith('/')) candidate = candidate.dropLast(1)
        val hostPart = candidate.substringBefore('/').substringBefore('?').substringBefore('#')
        val host = hostPart.substringBefore(':')
        val port = hostPart.substringAfter(':', "")
        if (port.isNotEmpty() && !port.all { it.isDigit() }) return Input.Search(text)
        if (looksLikeHost(host)) {
            val scheme = if (host == "localhost" || ipv4Regex.matches(host)) "http" else "https"
            return Input.Navigate("$scheme://$text")
        }
        return Input.Search(text)
    }

    private fun defaultScheme(url: String): String {
        val host = hostOf(url) ?: return url
        return if (host == "localhost" || ipv4Regex.matches(host)) url else url.replaceFirst("http://", "https://")
    }

    private fun looksLikeHost(host: String): Boolean {
        if (host.equals("localhost", ignoreCase = true)) return true
        if (ipv4Regex.matches(host)) return host.split('.').all { (it.toIntOrNull() ?: 999) <= 255 }
        return looksLikeDomain(host)
    }

    /** At least two labels of letters/digits/hyphens; TLD not purely numeric. */
    fun looksLikeDomain(host: String): Boolean {
        if (host.isEmpty() || host.length > 253) return false
        val labels = host.split('.')
        if (labels.size < 2) return false
        for (label in labels) {
            if (label.isEmpty() || label.length > 63) return false
            if (label.first() == '-' || label.last() == '-') return false
            if (!label.all { it.isLetterOrDigit() || it == '-' }) return false
        }
        return !labels.last().all { it.isDigit() }
    }

    /**
     * Resolve typed text to the URL to load: a URL as-is (optionally
     * upgraded to https), or the search template expanded with the terms.
     */
    fun resolve(raw: String, searchTemplate: String, httpsOnly: Boolean = false): String? =
        when (val input = classify(raw)) {
            is Input.Empty -> null
            is Input.Navigate -> if (httpsOnly) upgradeHttp(input.url) else input.url
            is Input.Search -> SearchTemplates.expand(searchTemplate, input.terms)
        }

    fun upgradeHttp(url: String): String {
        if (!url.regionMatches(0, "http://", 0, 7, ignoreCase = true)) return url
        val host = hostOf(url) ?: return url
        if (host == "localhost" || ipv4Regex.matches(host)) return url
        return "https://" + url.substring(7)
    }

    /** Lower-case host of an http(s)/ws URL, or null. */
    fun hostOf(url: String): String? = try {
        URI(url).host?.lowercase(Locale.ROOT)?.removePrefix("[")?.removeSuffix("]")
    } catch (_: Exception) {
        // URIs with characters java.net.URI rejects (e.g. "|"): fall back
        // to a manual split so ad-block matching still sees a host.
        val afterScheme = url.substringAfter("://", "")
        if (afterScheme.isEmpty()) null
        else afterScheme.substringBefore('/').substringBefore('?').substringBefore('#')
            .substringAfter('@').substringBefore(':').lowercase(Locale.ROOT).ifEmpty { null }
    }

    /** What the address bar shows when not editing: host + path, no scheme. */
    fun displayUrl(url: String): String {
        if (url == HOME_URL) return ""
        return url.removePrefix("https://").removePrefix("http://").removePrefix("www.")
            .removeSuffix("/")
    }

    fun isWebUrl(url: String?): Boolean =
        url != null && (url.startsWith("http://", true) || url.startsWith("https://", true))
}
