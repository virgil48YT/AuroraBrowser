package com.aurora.browser.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OmniboxTest {
    private val ddg = "https://duckduckgo.com/?q=%s"

    @Test
    fun urlsVersusSearches() {
        assertEquals(Omnibox.Input.Navigate("https://example.com"), Omnibox.classify("example.com"))
        assertEquals(Omnibox.Input.Navigate("https://example.com/path?q=1"), Omnibox.classify("  example.com/path?q=1 "))
        assertEquals(Omnibox.Input.Navigate("http://example.com"), Omnibox.classify("http://example.com"))
        assertEquals(Omnibox.Input.Navigate("http://localhost:8080"), Omnibox.classify("localhost:8080"))
        assertEquals(Omnibox.Input.Navigate("http://192.168.1.1"), Omnibox.classify("192.168.1.1"))
        assertEquals(Omnibox.Input.Navigate("https://example.com:8443/x"), Omnibox.classify("example.com:8443/x"))
        assertEquals(Omnibox.Input.Search("hello world"), Omnibox.classify("hello world"))
        assertEquals(Omnibox.Input.Search("kotlin"), Omnibox.classify("kotlin"))
        assertEquals(Omnibox.Input.Search("what is example.com"), Omnibox.classify("what is example.com"))
        assertEquals(Omnibox.Input.Search("1.5"), Omnibox.classify("1.5"))
        assertEquals(Omnibox.Input.Empty, Omnibox.classify("   "))
        assertTrue(Omnibox.classify("aurora://home") is Omnibox.Input.Navigate)
    }

    @Test
    fun resolveUsesTemplate() {
        assertEquals("https://duckduckgo.com/?q=a+b%26c", Omnibox.resolve("a b&c", ddg))
        assertEquals("https://search.example/?q=caf%C3%A9&src=aurora", Omnibox.resolve("café", "https://search.example/?q=%s&src=aurora"))
        assertEquals("https://example.com", Omnibox.resolve("http://example.com", ddg, httpsOnly = true))
        assertEquals("http://localhost:3000", Omnibox.resolve("localhost:3000", ddg, httpsOnly = true))
        assertNull(Omnibox.resolve("", ddg))
    }

    @Test
    fun templateValidation() {
        assertEquals(SearchTemplates.TemplateError.OK, SearchTemplates.validateTemplate("https://s.example.com/?q=%s"))
        assertEquals(SearchTemplates.TemplateError.NOT_HTTPS, SearchTemplates.validateTemplate("http://s.example.com/?q=%s"))
        assertEquals(SearchTemplates.TemplateError.MISSING_PLACEHOLDER, SearchTemplates.validateTemplate("https://s.example.com/?q="))
        assertEquals(SearchTemplates.TemplateError.MULTIPLE_PLACEHOLDERS, SearchTemplates.validateTemplate("https://s.example.com/?q=%s&r=%s"))
        assertEquals(SearchTemplates.TemplateError.PLACEHOLDER_IN_HOST, SearchTemplates.validateTemplate("https://%s.example.com/"))
        assertEquals(SearchTemplates.TemplateError.INVALID_CHARACTER, SearchTemplates.validateTemplate("https://s.example.com/?q= %s"))
        assertEquals(SearchTemplates.TemplateError.INVALID_HOST, SearchTemplates.validateTemplate("https://user@s.example.com/?q=%s"))
        assertEquals("https://s.example.com/", SearchTemplates.home("https://s.example.com/find?q=%s"))
    }

    @Test
    fun displayUrl() {
        assertEquals("example.com/a", Omnibox.displayUrl("https://www.example.com/a"))
        assertEquals("", Omnibox.displayUrl(Omnibox.HOME_URL))
    }
}
