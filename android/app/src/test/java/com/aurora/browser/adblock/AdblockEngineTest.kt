package com.aurora.browser.adblock

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class AdblockEngineTest {

    private fun engine(rules: String) = AdblockEngine.build(rules)

    private fun blocked(e: AdblockEngine, url: String, page: String = "news.example", type: ResourceType = ResourceType.SCRIPT): Boolean {
        val host = url.substringAfter("://").substringBefore('/').substringBefore(':')
        return e.shouldBlock(url, host, page, type)
    }

    @Test
    fun hostAnchorBlocksDomainAndSubdomains() {
        val e = engine("||doubleclick.net^")
        assertTrue(blocked(e, "https://doubleclick.net/x.js"))
        assertTrue(blocked(e, "https://ad.g.doubleclick.net/pagead/ads?x=1"))
        assertFalse(blocked(e, "https://notdoubleclick.net/x.js"))
        assertFalse(blocked(e, "https://example.com/?ref=doubleclick.net"))
    }

    @Test
    fun hostAnchorWithPath() {
        val e = engine("||facebook.com/tr^")
        assertTrue(blocked(e, "https://www.facebook.com/tr?id=1", type = ResourceType.IMAGE))
        assertFalse(blocked(e, "https://www.facebook.com/track", type = ResourceType.IMAGE))
        assertFalse(blocked(e, "https://www.facebook.com/profile", type = ResourceType.IMAGE))
    }

    @Test
    fun genericPathPatternsAndWildcards() {
        val e = engine("/adserver/*\n_300x250_\n/banner/*/img^\n/popunder|")
        assertTrue(blocked(e, "https://cdn.site.com/adserver/load.js"))
        assertTrue(blocked(e, "https://cdn.site.com/img/ad_300x250_.gif", type = ResourceType.IMAGE))
        assertTrue(blocked(e, "https://x.com/banner/123/img?x", type = ResourceType.IMAGE))
        assertTrue(blocked(e, "https://x.com/popunder"))
        assertFalse(blocked(e, "https://x.com/popunder.js"))
        assertFalse(blocked(e, "https://x.com/adservers/load.js"))
        // ABP semantics: a pattern wrapped in slashes is a regular expression.
        val r = engine("/adserver/")
        assertTrue(blocked(r, "https://x.com/adservers/load.js"))
    }

    @Test
    fun exceptionsOverrideBlocks() {
        val e = engine("||ads.example.com^\n@@||ads.example.com/allowed/")
        assertTrue(blocked(e, "https://ads.example.com/track.js"))
        assertFalse(blocked(e, "https://ads.example.com/allowed/lib.js"))
        val m = e.match("https://ads.example.com/allowed/lib.js", "ads.example.com", "news.example", ResourceType.SCRIPT)
        assertNotNull(m.exception)
    }

    @Test
    fun thirdPartyOption() {
        val e = engine("||tracker.io^\$third-party")
        assertTrue(blocked(e, "https://tracker.io/t.js", page = "news.example"))
        assertFalse(blocked(e, "https://tracker.io/t.js", page = "tracker.io"))
        assertFalse(blocked(e, "https://cdn.tracker.io/t.js", page = "www.tracker.io"))
    }

    @Test
    fun typeOptions() {
        val e = engine("||cdn.example^\$script\n||img.example^\$~image")
        assertTrue(blocked(e, "https://cdn.example/a.js", type = ResourceType.SCRIPT))
        assertFalse(blocked(e, "https://cdn.example/a.png", type = ResourceType.IMAGE))
        assertFalse(blocked(e, "https://img.example/a.png", type = ResourceType.IMAGE))
        assertTrue(blocked(e, "https://img.example/a.js", type = ResourceType.SCRIPT))
    }

    @Test
    fun domainOption() {
        val e = engine("/sponsor.js\$domain=news.example|~safe.news.example")
        assertTrue(blocked(e, "https://cdn.x/sponsor.js", page = "www.news.example"))
        assertFalse(blocked(e, "https://cdn.x/sponsor.js", page = "safe.news.example"))
        assertFalse(blocked(e, "https://cdn.x/sponsor.js", page = "other.example"))
    }

    @Test
    fun documentsAreNeverBlockedWithoutDocumentOption() {
        val e = engine("||doubleclick.net^")
        assertFalse(blocked(e, "https://doubleclick.net/", type = ResourceType.DOCUMENT))
    }

    @Test
    fun regexRules() {
        val e = engine("/\\/ads?\\d+\\.js/")
        assertTrue(blocked(e, "https://x.com/ad12.js"))
        assertTrue(blocked(e, "https://x.com/ads3.js"))
        assertFalse(blocked(e, "https://x.com/add.js"))
    }

    @Test
    fun unsupportedRulesAreSkippedNotMisapplied() {
        val r = FilterParser.parse("||x.com^\$popup\n||y.com^\$redirect=noop.js\nexample.com#?#div:-abp-has(.ad)\n! comment\n[Adblock Plus 2.0]")
        assertEquals(0, r.network.size)
        assertEquals(0, r.cosmetic.size)
        assertEquals(3, r.skipped)
    }

    @Test
    fun cosmeticRules() {
        val e = engine("##.ad-banner\nyoutube.com###player-ads\nexample.com#@#.ad-banner\n~foo.com##.sponsored")
        val yt = e.hiddenSelectors("m.youtube.com")
        assertTrue(".ad-banner" in yt)
        assertTrue("#player-ads" in yt)
        assertTrue(".sponsored" in yt)
        val ex = e.hiddenSelectors("example.com")
        assertFalse(".ad-banner" in ex)
        assertFalse("#player-ads" in ex)
        assertFalse(".sponsored" in e.hiddenSelectors("www.foo.com"))
        assertTrue(e.cosmeticCss("m.youtube.com").contains("#player-ads{display:none!important}"))
    }

    @Test
    fun builtInListParsesAndBlocksKnownTrackers() {
        val file = listOf(
            File("src/main/assets/filters/aurora-builtin.txt"),
            File("app/src/main/assets/filters/aurora-builtin.txt"),
        ).first { it.exists() }
        val text = file.readText()
        val parsed = FilterParser.parse(text)
        assertTrue("built-in list should have rules", parsed.network.size > 50)
        assertEquals("every built-in rule should be understood", 0, parsed.skipped)
        val e = AdblockEngine.build(text)
        assertTrue(blocked(e, "https://www.google-analytics.com/analytics.js"))
        assertTrue(blocked(e, "https://securepubads.g.doubleclick.net/tag/js/gpt.js"))
        assertTrue(blocked(e, "https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js"))
        assertFalse(blocked(e, "https://www.wikipedia.org/static/app.js"))
        assertFalse(blocked(e, "https://accounts.google.com/gsi/client", type = ResourceType.SCRIPT))
    }

    @Test
    fun registrableDomain() {
        assertEquals("example.com", DomainUtil.registrableDomain("a.b.example.com"))
        assertEquals("bbc.co.uk", DomainUtil.registrableDomain("www.bbc.co.uk"))
        assertEquals("user.github.io", DomainUtil.registrableDomain("user.github.io"))
        assertEquals("127.0.0.1", DomainUtil.registrableDomain("127.0.0.1"))
    }

    @Test
    fun requestClassifier() {
        assertEquals(ResourceType.SCRIPT, RequestClassifier.classify("https://x/a.js?v=1", null, false))
        assertEquals(ResourceType.IMAGE, RequestClassifier.classify("https://x/p", "image/avif,image/webp,*/*", false))
        assertEquals(ResourceType.SUBDOCUMENT, RequestClassifier.classify("https://x/frame", "text/html,*/*", false))
        assertEquals(ResourceType.DOCUMENT, RequestClassifier.classify("https://x/", "text/html", true))
        assertNull(ResourceType.fromOption("popup"))
    }
}
