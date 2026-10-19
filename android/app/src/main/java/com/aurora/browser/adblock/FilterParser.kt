package com.aurora.browser.adblock

import java.util.Locale

/** Resource types, spelled like Adblock Plus / EasyList options. */
enum class ResourceType(val option: String) {
    DOCUMENT("document"),
    SUBDOCUMENT("subdocument"),
    SCRIPT("script"),
    IMAGE("image"),
    STYLESHEET("stylesheet"),
    XMLHTTPREQUEST("xmlhttprequest"),
    MEDIA("media"),
    FONT("font"),
    WEBSOCKET("websocket"),
    PING("ping"),
    OBJECT("object"),
    OTHER("other");

    companion object {
        private val byOption = entries.associateBy { it.option } + mapOf(
            "css" to STYLESHEET, "xhr" to XMLHTTPREQUEST, "frame" to SUBDOCUMENT,
            "doc" to DOCUMENT, "object-subrequest" to OBJECT, "beacon" to PING,
        )

        fun fromOption(name: String): ResourceType? = byOption[name]
    }
}

/** A parsed network (URL) filter. */
class NetworkFilter(
    val raw: String,
    val isException: Boolean,
    /** Compiled URL pattern; matched against the full request URL. */
    val regex: Regex,
    /** For "||host^" style rules: the anchored host, else null. */
    val anchoredHost: String?,
    /** True when the pattern is exactly "||host^" (pure host rule). */
    val isPureHostRule: Boolean,
    /** Allowed resource types (null = all types except document). */
    val types: Set<ResourceType>?,
    /** null = either; true = only third-party; false = only first-party. */
    val thirdParty: Boolean?,
    val includeDomains: Set<String>,
    val excludeDomains: Set<String>,
    /** A literal token from the pattern used for indexing, or null. */
    val token: String?,
) {
    fun matches(req: FilterRequest): Boolean {
        if (types != null) {
            if (req.type !in types) return false
        } else if (req.type == ResourceType.DOCUMENT) {
            // Never block top-level documents unless the rule says $document.
            return false
        }
        if (thirdParty != null && thirdParty != req.isThirdParty) return false
        if (includeDomains.isNotEmpty() && !DomainUtil.matchesAny(req.sourceHost, includeDomains)) return false
        if (excludeDomains.isNotEmpty() && DomainUtil.matchesAny(req.sourceHost, excludeDomains)) return false
        if (isPureHostRule && anchoredHost != null) {
            return DomainUtil.isSameOrSubdomain(req.host, anchoredHost)
        }
        return regex.containsMatchIn(req.url)
    }

    override fun toString(): String = raw
}

/** Element-hiding rule: "example.com##.ad", "##.banner", "site#@#.ad". */
class CosmeticFilter(
    val selector: String,
    val isException: Boolean,
    val includeDomains: Set<String>,
    val excludeDomains: Set<String>,
) {
    val isGeneric: Boolean get() = includeDomains.isEmpty()

    fun appliesTo(host: String): Boolean {
        if (excludeDomains.isNotEmpty() && DomainUtil.matchesAny(host, excludeDomains)) return false
        if (includeDomains.isEmpty()) return true
        return DomainUtil.matchesAny(host, includeDomains)
    }
}

class FilterRequest(
    val url: String,
    val host: String,
    val sourceHost: String,
    val type: ResourceType,
) {
    val isThirdParty: Boolean =
        sourceHost.isNotEmpty() && DomainUtil.registrableDomain(host) != DomainUtil.registrableDomain(sourceHost)
}

/**
 * EasyList / Adblock Plus syntax parser. Supports the subset that matters
 * for request blocking and simple element hiding:
 *  - comments ("!"), headers ("[Adblock Plus 2.0]")
 *  - "||host^" host anchors, "|" start/end anchors, "*" wildcards, "^" separators
 *  - "/regex/" patterns
 *  - "@@" exceptions
 *  - options: resource types (and ~negations), third-party / ~third-party
 *    (also 3p / 1p), domain=a.com|~b.com, match-case, important (ignored)
 *  - "##" / "#@#" element hiding with optional domain lists
 * Rules using options we do not understand are dropped (allowing is always
 * the safe default), as are scriptlet/procedural cosmetic rules.
 */
object FilterParser {

    data class Result(val network: List<NetworkFilter>, val cosmetic: List<CosmeticFilter>, val skipped: Int)

    private val pureHostRegex = Regex("^\\|\\|([a-z0-9.\\-]+)\\^?$")
    private val tokenRegex = Regex("[a-z0-9%]{3,}")

    fun parse(text: String): Result {
        val network = ArrayList<NetworkFilter>()
        val cosmetic = ArrayList<CosmeticFilter>()
        var skipped = 0
        text.lineSequence().forEach { rawLine ->
            val line = rawLine.trim()
            if (line.isEmpty() || line.startsWith("!") || line.startsWith("[")) return@forEach
            val parsedCosmetic = parseCosmetic(line)
            if (parsedCosmetic != null) {
                if (parsedCosmetic === UNSUPPORTED_COSMETIC) skipped++ else cosmetic.add(parsedCosmetic)
                return@forEach
            }
            val filter = parseNetwork(line)
            if (filter != null) network.add(filter) else skipped++
        }
        return Result(network, cosmetic, skipped)
    }

    private val UNSUPPORTED_COSMETIC = CosmeticFilter("", false, emptySet(), emptySet())

    /** Returns null if [line] is not a cosmetic rule at all. */
    fun parseCosmetic(line: String): CosmeticFilter? {
        // Procedural / scriptlet / CSS-injection variants we do not support.
        for (marker in arrayOf("#?#", "#$#", "#%#", "#@?#", "#@$#", "#@%#")) {
            if (line.contains(marker)) return UNSUPPORTED_COSMETIC
        }
        val exceptionIdx = line.indexOf("#@#")
        val hideIdx = line.indexOf("##")
        val (idx, sepLen, isException) = when {
            exceptionIdx >= 0 -> Triple(exceptionIdx, 3, true)
            hideIdx >= 0 -> Triple(hideIdx, 2, false)
            else -> return null
        }
        val domains = line.substring(0, idx)
        val selector = line.substring(idx + sepLen).trim()
        if (selector.isEmpty() || selector.startsWith("+js(") || selector.contains(":style(") ||
            selector.contains(":-abp-") || selector.contains("{") || selector.contains("}")
        ) {
            return UNSUPPORTED_COSMETIC
        }
        val include = HashSet<String>()
        val exclude = HashSet<String>()
        if (domains.isNotEmpty()) {
            for (d in domains.split(',')) {
                val dom = d.trim().lowercase(Locale.ROOT)
                if (dom.isEmpty()) continue
                if (dom.startsWith("~")) exclude.add(dom.substring(1)) else include.add(dom)
            }
        }
        return CosmeticFilter(selector, isException, include, exclude)
    }

    fun parseNetwork(line: String): NetworkFilter? {
        var rule = line
        val isException = rule.startsWith("@@")
        if (isException) rule = rule.substring(2)
        if (rule.isEmpty()) return null

        // Split off "$options" (but not a "$" inside a /regex/).
        var optionsText: String? = null
        val isRegexRule = rule.startsWith("/") && rule.lastIndexOf('/') > 0 &&
            (rule.endsWith("/") || rule.substring(rule.lastIndexOf('/')).startsWith("/$"))
        val dollar = rule.lastIndexOf('$')
        if (dollar >= 0 && (!isRegexRule || dollar > rule.lastIndexOf('/'))) {
            val candidate = rule.substring(dollar + 1)
            if (candidate.isNotEmpty() && candidate.all { it.isLetterOrDigit() || it in ",=~|.-_*" }) {
                optionsText = candidate
                rule = rule.substring(0, dollar)
            }
        }

        var types: MutableSet<ResourceType>? = null
        val negatedTypes = HashSet<ResourceType>()
        var thirdParty: Boolean? = null
        val include = HashSet<String>()
        val exclude = HashSet<String>()
        var matchCase = false

        if (optionsText != null) {
            for (opt in optionsText.split(',')) {
                val o = opt.trim().lowercase(Locale.ROOT)
                if (o.isEmpty()) continue
                val negated = o.startsWith("~")
                val name = o.removePrefix("~")
                when {
                    name == "third-party" || name == "3p" -> thirdParty = !negated
                    name == "first-party" || name == "1p" -> thirdParty = negated
                    name.startsWith("domain=") -> {
                        for (d in name.removePrefix("domain=").split('|')) {
                            if (d.isEmpty()) continue
                            if (d.startsWith("~")) exclude.add(d.substring(1)) else include.add(d)
                        }
                    }
                    name == "match-case" -> matchCase = true
                    name == "important" -> Unit
                    else -> {
                        val type = ResourceType.fromOption(name) ?: return null // unknown option
                        if (negated) negatedTypes.add(type) else {
                            if (types == null) types = HashSet()
                            types!!.add(type)
                        }
                    }
                }
            }
        }
        if (types == null && negatedTypes.isNotEmpty()) {
            types = ResourceType.entries.filter { it != ResourceType.DOCUMENT && it !in negatedTypes }.toMutableSet()
        } else if (types != null) {
            types!!.removeAll(negatedTypes)
            if (types!!.isEmpty()) return null
        }

        if (rule.isEmpty() || rule == "*") {
            // A bare "$domain=..." rule would block everything on a site; skip.
            return null
        }

        val lowerRule = if (matchCase) rule else rule.lowercase(Locale.ROOT)
        val regex: Regex
        var anchoredHost: String? = null
        var pureHost = false
        var token: String? = null
        if (isRegexRule && lowerRule.length > 2 && lowerRule.endsWith("/")) {
            regex = try {
                if (matchCase) Regex(rule.substring(1, rule.length - 1))
                else Regex(rule.substring(1, rule.length - 1), RegexOption.IGNORE_CASE)
            } catch (_: Exception) {
                return null
            }
        } else {
            val pure = pureHostRegex.find(lowerRule)
            if (pure != null) {
                anchoredHost = pure.groupValues[1].trimEnd('.')
                pureHost = true
            } else if (lowerRule.startsWith("||")) {
                anchoredHost = lowerRule.substring(2).takeWhile { it.isLetterOrDigit() || it == '.' || it == '-' }
                    .ifEmpty { null }
            }
            regex = try {
                compilePattern(lowerRule, matchCase)
            } catch (_: Exception) {
                return null
            }
            token = longestToken(lowerRule)
        }

        return NetworkFilter(
            raw = line,
            isException = isException,
            regex = regex,
            anchoredHost = anchoredHost,
            isPureHostRule = pureHost,
            types = types,
            thirdParty = thirdParty,
            includeDomains = include,
            excludeDomains = exclude,
            token = token,
        )
    }

    /** Convert an ABP pattern into a regular expression. */
    fun compilePattern(pattern: String, matchCase: Boolean = false): Regex {
        val sb = StringBuilder()
        var p = pattern
        when {
            p.startsWith("||") -> {
                sb.append("^[a-z][a-z0-9+.\\-]*://([^/?#]*\\.)?")
                p = p.substring(2)
            }
            p.startsWith("|") -> {
                sb.append('^')
                p = p.substring(1)
            }
        }
        var endAnchor = false
        if (p.endsWith("|") && p.length > 0) {
            endAnchor = true
            p = p.dropLast(1)
        }
        for (c in p) {
            when (c) {
                '*' -> sb.append(".*")
                '^' -> sb.append("(?:[^a-zA-Z0-9_\\-.%]|$)")
                else -> if (c.isLetterOrDigit()) sb.append(c) else sb.append('\\').append(c)
            }
        }
        if (endAnchor) sb.append('$')
        return if (matchCase) Regex(sb.toString()) else Regex(sb.toString(), RegexOption.IGNORE_CASE)
    }

    /**
     * Longest literal token of a pattern, used to index rules so a request
     * only has to be tested against rules sharing one of its URL tokens.
     * Tokens adjacent to a wildcard are not safe (they may be partial), so
     * only fully delimited runs qualify.
     */
    fun longestToken(pattern: String): String? {
        var best: String? = null
        for (m in tokenRegex.findAll(pattern)) {
            val start = m.range.first
            val end = m.range.last + 1
            val before = if (start > 0) pattern[start - 1] else null
            val after = if (end < pattern.length) pattern[end] else null
            if (before == '*' || after == '*') continue
            // A token at the very start/end of an unanchored pattern may be a
            // fragment of a longer URL token ("ads" in "/ads" matches "/adsx").
            if (start == 0 || end == pattern.length) continue
            if (best == null || m.value.length > best.length) best = m.value
        }
        return best
    }

    fun tokenize(url: String): List<String> = tokenRegex.findAll(url.lowercase(Locale.ROOT)).map { it.value }.toList()
}
