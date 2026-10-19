package com.aurora.browser.adblock

import java.util.Locale

/**
 * Immutable request matcher built from EasyList-syntax rules. Thread-safe
 * after construction: shouldInterceptRequest() calls [match] from WebView's
 * network threads while the UI thread may swap in a rebuilt engine.
 *
 * Rules are indexed so most requests are tested against a handful of rules:
 *  - "||host..." rules by their anchored host (looked up per host suffix)
 *  - other rules by their longest literal token (looked up per URL token)
 *  - the few rules with neither go into a short linear list
 */
class AdblockEngine private constructor(
    private val blocks: Index,
    private val exceptions: Index,
    private val genericHide: Set<String>,
    private val specificHide: List<CosmeticFilter>,
    private val hideExceptions: List<CosmeticFilter>,
    val networkRuleCount: Int,
    val cosmeticRuleCount: Int,
) {
    data class Match(val blocked: Boolean, val filter: NetworkFilter?, val exception: NetworkFilter? = null)

    private class Index {
        val byHost = HashMap<String, MutableList<NetworkFilter>>()
        val byToken = HashMap<String, MutableList<NetworkFilter>>()
        val rest = ArrayList<NetworkFilter>()

        fun add(f: NetworkFilter) {
            when {
                f.anchoredHost != null -> byHost.getOrPut(f.anchoredHost) { ArrayList(1) }.add(f)
                f.token != null -> byToken.getOrPut(f.token) { ArrayList(1) }.add(f)
                else -> rest.add(f)
            }
        }

        fun find(req: FilterRequest, tokens: List<String>): NetworkFilter? {
            for (suffix in DomainUtil.suffixes(req.host)) {
                byHost[suffix]?.firstOrNull { it.matches(req) }?.let { return it }
            }
            if (byToken.isNotEmpty()) {
                for (t in tokens) {
                    byToken[t]?.firstOrNull { it.matches(req) }?.let { return it }
                }
            }
            return rest.firstOrNull { it.matches(req) }
        }
    }

    /**
     * @param url the resource URL
     * @param sourceUrlHost host of the page that issued the request ("" if unknown)
     */
    fun match(url: String, host: String, sourceUrlHost: String, type: ResourceType): Match {
        val req = FilterRequest(url, host.lowercase(Locale.ROOT), sourceUrlHost.lowercase(Locale.ROOT), type)
        val tokens = FilterParser.tokenize(url)
        val block = blocks.find(req, tokens) ?: return Match(false, null)
        val exception = exceptions.find(req, tokens)
        return if (exception != null) Match(false, block, exception) else Match(true, block)
    }

    fun shouldBlock(url: String, host: String, sourceUrlHost: String, type: ResourceType): Boolean =
        match(url, host, sourceUrlHost, type).blocked

    /** Selectors to hide on [host]. */
    fun hiddenSelectors(host: String): List<String> {
        val h = host.lowercase(Locale.ROOT)
        val excluded = hideExceptions.filter { it.appliesTo(h) }.map { it.selector }.toHashSet()
        val out = LinkedHashSet<String>()
        for (s in genericHide) if (s !in excluded) out.add(s)
        for (f in specificHide) if (f.appliesTo(h) && f.selector !in excluded) out.add(f.selector)
        return out.toList()
    }

    /** A stylesheet hiding [hiddenSelectors]; one rule per selector so one bad selector cannot void the rest. */
    fun cosmeticCss(host: String): String =
        hiddenSelectors(host).joinToString("\n") { "$it{display:none!important}" }

    companion object {
        fun build(rulesText: String): AdblockEngine {
            val parsed = FilterParser.parse(rulesText)
            val blocks = Index()
            val exceptions = Index()
            for (f in parsed.network) if (f.isException) exceptions.add(f) else blocks.add(f)
            val genericHide = LinkedHashSet<String>()
            val specific = ArrayList<CosmeticFilter>()
            val hideExceptions = ArrayList<CosmeticFilter>()
            for (c in parsed.cosmetic) {
                when {
                    c.isException -> hideExceptions.add(c)
                    c.isGeneric && c.excludeDomains.isEmpty() -> genericHide.add(c.selector)
                    else -> specific.add(c)
                }
            }
            // Generic hides with exclusions behave like specific rules.
            return AdblockEngine(
                blocks, exceptions, genericHide, specific, hideExceptions,
                parsed.network.size, parsed.cosmetic.size,
            )
        }

        val EMPTY: AdblockEngine by lazy { build("") }
    }
}
