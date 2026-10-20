package com.aurora.browser.core

import java.util.Locale

/**
 * Short, human names for sites, used for the start page's shortcut tiles
 * ("DuckDuckGo", "GitHub", "Example") instead of whatever page title happened
 * to be loaded last ("googke at DuckDuckGo"). Pure Kotlin so it is unit
 * testable on the JVM.
 */
object SiteNames {
    /** Multi-label public suffixes common enough to matter for tile names. */
    private val TWO_LEVEL_SUFFIXES = setOf(
        "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk",
        "com.au", "net.au", "org.au", "edu.au", "gov.au",
        "co.nz", "org.nz", "co.jp", "ne.jp", "or.jp", "ac.jp", "co.kr", "or.kr",
        "com.br", "net.br", "org.br", "com.mx", "com.ar", "com.tr", "com.cn",
        "com.hk", "com.tw", "com.sg", "com.my", "co.in", "net.in", "org.in",
        "co.za", "co.il", "com.ua", "com.pl", "com.ro", "com.es", "com.pt",
        "co.id", "or.id", "com.ph", "com.vn", "com.eg", "com.sa", "com.ng",
    )

    /** Brand capitalisation for well-known sites (keyed by registrable label). */
    private val PRETTY = mapOf(
        "duckduckgo" to "DuckDuckGo",
        "github" to "GitHub",
        "gitlab" to "GitLab",
        "youtube" to "YouTube",
        "linkedin" to "LinkedIn",
        "tiktok" to "TikTok",
        "stackoverflow" to "Stack Overflow",
        "stackexchange" to "Stack Exchange",
        "whatsapp" to "WhatsApp",
        "paypal" to "PayPal",
        "ebay" to "eBay",
        "icloud" to "iCloud",
        "soundcloud" to "SoundCloud",
        "wordpress" to "WordPress",
        "openai" to "OpenAI",
        "chatgpt" to "ChatGPT",
        "imdb" to "IMDb",
        "bbc" to "BBC",
        "cnn" to "CNN",
        "nytimes" to "NYTimes",
        "x" to "X",
        "vk" to "VK",
        "msn" to "MSN",
        "aliexpress" to "AliExpress",
        "deviantart" to "DeviantArt",
        "duolingo" to "Duolingo",
        "startpage" to "Startpage",
        "torproject" to "Tor Project",
    )

    /** "www.", "m." and similar prefixes that are not part of a site's name. */
    private val NOISE_PREFIXES = listOf("www.", "www1.", "www2.", "m.", "mobile.", "amp.")

    /** Host without "www." / "m." style prefixes, lower-cased. */
    fun normalizeHost(host: String): String {
        var h = host.trim().lowercase(Locale.ROOT).trimEnd('.')
        var changed = true
        while (changed) {
            changed = false
            for (p in NOISE_PREFIXES) {
                if (h.startsWith(p) && h.length > p.length && h.substring(p.length).contains('.')) {
                    h = h.substring(p.length)
                    changed = true
                }
            }
        }
        return h
    }

    /** Registrable domain ("news.bbc.co.uk" -> "bbc.co.uk"); IPs and single labels unchanged. */
    fun registrableDomain(host: String): String {
        val h = normalizeHost(host)
        if (isIpLiteral(h)) return h
        val labels = h.split('.').filter { it.isNotEmpty() }
        if (labels.size <= 2) return labels.joinToString(".")
        val lastTwo = labels.takeLast(2).joinToString(".")
        val keep = if (lastTwo in TWO_LEVEL_SUFFIXES) 3 else 2
        return labels.takeLast(keep).joinToString(".")
    }

    /** Short display name for [host]: "duckduckgo.com" -> "DuckDuckGo". */
    fun forHost(host: String): String {
        val h = normalizeHost(host)
        if (h.isEmpty()) return host
        if (isIpLiteral(h) || !h.contains('.')) return h
        val domain = registrableDomain(h)
        val label = domain.substringBefore('.')
        if (label.isEmpty()) return h
        PRETTY[label]?.let { return it }
        // Onion services: the 56-char address is not a name; keep it short.
        if (domain.endsWith(".onion")) return label.take(10) + "….onion"
        return label.replaceFirstChar { it.titlecase(Locale.ROOT) }
    }

    /** Display name for [url], or null when it has no host (about:, data:...). */
    fun forUrl(url: String): String? = Omnibox.hostOf(url)?.takeIf { it.isNotEmpty() }?.let { forHost(it) }

    /**
     * Label for a shortcut tile. A bookmark keeps its own title when it is a
     * short, hand-picked name ("Work mail"); long page titles ("googke at
     * DuckDuckGo", "Foo - Bar | Site") fall back to the site's name.
     */
    fun shortcutLabel(url: String, title: String?): String {
        val site = forUrl(url) ?: url
        val t = title?.trim().orEmpty()
        if (t.isEmpty() || t.length > 18) return site
        val separators = listOf(" - ", " – ", " — ", " | ", " · ", " • ", ": ", " at ", " on ")
        if (separators.any { t.contains(it, ignoreCase = true) }) return site
        if (t.startsWith("http://") || t.startsWith("https://")) return site
        return t
    }

    /** Root URL of the site behind [url] ("https://duckduckgo.com/?q=x" -> "https://duckduckgo.com/"). */
    fun siteRoot(url: String): String? {
        val host = Omnibox.hostOf(url)?.takeIf { it.isNotEmpty() } ?: return null
        val scheme = url.substringBefore("://", "").lowercase(Locale.ROOT)
        if (scheme != "http" && scheme != "https") return null
        val authority = url.substringAfter("://").substringBefore('/').substringBefore('?').substringBefore('#')
            .substringAfter('@')
        val port = authority.substringAfterLast(':', "").takeIf { p -> p.isNotEmpty() && p.all { it.isDigit() } && !authority.endsWith("]") }
        val hostPart = if (host.contains(':')) "[$host]" else host
        return "$scheme://$hostPart${if (port != null) ":$port" else ""}/"
    }

    private fun isIpLiteral(h: String): Boolean =
        h.contains(':') || h.split('.').let { parts -> parts.size == 4 && parts.all { p -> p.isNotEmpty() && p.all { it.isDigit() } } }
}
