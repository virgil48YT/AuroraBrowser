package com.aurora.browser.adblock

/**
 * Site boundaries without the full Public Suffix List — the same trade-off
 * as the desktop build (src/privacy/CookieSite.cpp): a small built-in list
 * of shared suffixes, otherwise "last two labels".
 */
object DomainUtil {
    private val sharedSuffixes = setOf(
        "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk",
        "com.au", "net.au", "org.au", "edu.au", "gov.au",
        "co.nz", "org.nz", "co.jp", "ne.jp", "or.jp", "co.kr", "or.kr",
        "com.br", "com.cn", "com.hk", "com.tw", "com.sg", "com.my", "com.mx",
        "com.ar", "com.tr", "com.ua", "com.pl", "com.ro", "co.in", "co.za",
        "co.id", "co.il", "com.vn", "com.ph", "com.pk", "com.eg", "com.sa",
        "github.io", "gitlab.io", "blogspot.com", "herokuapp.com", "netlify.app",
        "vercel.app", "pages.dev", "workers.dev", "web.app", "firebaseapp.com",
        "appspot.com", "azurewebsites.net", "cloudfront.net", "s3.amazonaws.com",
    )

    fun registrableDomain(host: String): String {
        val h = host.trimEnd('.').lowercase()
        if (h.isEmpty() || h.all { it.isDigit() || it == '.' } || h.contains(':')) return h
        val labels = h.split('.')
        if (labels.size <= 2) return h
        val lastTwo = labels.takeLast(2).joinToString(".")
        return if (lastTwo in sharedSuffixes && labels.size >= 3) labels.takeLast(3).joinToString(".") else lastTwo
    }

    fun isSameOrSubdomain(host: String, domain: String): Boolean =
        host == domain || (host.length > domain.length && host.endsWith(domain) && host[host.length - domain.length - 1] == '.')

    fun matchesAny(host: String, domains: Set<String>): Boolean {
        if (host.isEmpty()) return false
        if (host in domains) return true
        var idx = host.indexOf('.')
        while (idx >= 0) {
            if (host.substring(idx + 1) in domains) return true
            idx = host.indexOf('.', idx + 1)
        }
        return false
    }

    /** "a.b.example.com" -> ["a.b.example.com", "b.example.com", "example.com", "com"]. */
    fun suffixes(host: String): Sequence<String> = sequence {
        var h = host
        while (h.isNotEmpty()) {
            yield(h)
            val dot = h.indexOf('.')
            if (dot < 0) break
            h = h.substring(dot + 1)
        }
    }
}
