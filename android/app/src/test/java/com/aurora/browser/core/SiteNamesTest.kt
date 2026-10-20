package com.aurora.browser.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SiteNamesTest {
    @Test
    fun namesComeFromTheDomainNotThePageTitle() {
        assertEquals("DuckDuckGo", SiteNames.forUrl("https://duckduckgo.com/?q=googke&ia=web"))
        assertEquals("GitHub", SiteNames.forUrl("https://github.com/virgil48YT/AuroraBrowser"))
        assertEquals("Example", SiteNames.forUrl("https://www.example.com/a/b"))
        assertEquals("Wikipedia", SiteNames.forUrl("https://en.m.wikipedia.org/wiki/Tor"))
        assertEquals("BBC", SiteNames.forUrl("https://news.bbc.co.uk/"))
        assertEquals("Example", SiteNames.forUrl("https://shop.example.com.au/"))
        assertEquals("192.168.1.1", SiteNames.forUrl("http://192.168.1.1:8080/"))
        assertEquals("localhost", SiteNames.forUrl("http://localhost:3000/"))
        assertNull(SiteNames.forUrl("about:blank"))
    }

    @Test
    fun shortcutLabelsPreferShortHandPickedTitles() {
        assertEquals("DuckDuckGo", SiteNames.shortcutLabel("https://duckduckgo.com/?q=googke", "googke at DuckDuckGo"))
        assertEquals("GitHub", SiteNames.shortcutLabel("https://github.com/x/y", "x/y: A very long repository description"))
        assertEquals("Work mail", SiteNames.shortcutLabel("https://mail.example.com/", "Work mail"))
        assertEquals("Example", SiteNames.shortcutLabel("https://example.com/", ""))
    }

    @Test
    fun siteRootDropsPathAndQuery() {
        assertEquals("https://duckduckgo.com/", SiteNames.siteRoot("https://duckduckgo.com/?q=googke&ia=web"))
        assertEquals("http://localhost:8080/", SiteNames.siteRoot("http://localhost:8080/x?y"))
        assertEquals("https://www.example.com/", SiteNames.siteRoot("https://www.example.com/a#b"))
        assertNull(SiteNames.siteRoot("about:blank"))
        assertNull(SiteNames.siteRoot("file:///sdcard/a.html"))
    }

    @Test
    fun normalizeHostStripsMobileAndWwwPrefixes() {
        assertEquals("example.com", SiteNames.normalizeHost("WWW.Example.com"))
        assertEquals("wikipedia.org", SiteNames.registrableDomain("en.m.wikipedia.org"))
        assertEquals("m.com", SiteNames.normalizeHost("m.com"))
    }
}
