package com.aurora.browser.core

import java.net.URI

/**
 * Search engine URL templates — port of src/searchengines/SearchEngine.cpp.
 * A template is an https:// URL containing exactly one "%s", replaced by
 * the URL-encoded search terms. Pure Kotlin for unit testing.
 */
object SearchTemplates {
    const val PLACEHOLDER = "%s"
    const val MAX_NAME_LENGTH = 64
    const val MAX_TEMPLATE_LENGTH = 2048

    enum class TemplateError(val message: String) {
        OK(""),
        EMPTY("Enter a search URL."),
        TOO_LONG("The search URL is too long."),
        INVALID_CHARACTER("The search URL cannot contain spaces or control characters."),
        NOT_HTTPS("The search URL must start with https://"),
        INVALID_HOST("The search URL needs a valid host name."),
        PLACEHOLDER_IN_HOST("%s cannot be part of the host name."),
        MISSING_PLACEHOLDER("Put %s where the search terms go."),
        MULTIPLE_PLACEHOLDERS("Use %s only once."),
    }

    enum class NameError(val message: String) {
        OK(""),
        EMPTY("Enter a name."),
        TOO_LONG("The name is too long."),
        INVALID_CHARACTER("The name cannot contain control characters."),
    }

    fun validateName(name: String): NameError = when {
        name.isBlank() -> NameError.EMPTY
        name.length > MAX_NAME_LENGTH -> NameError.TOO_LONG
        name.any { it.isISOControl() } -> NameError.INVALID_CHARACTER
        else -> NameError.OK
    }

    fun validateTemplate(template: String): TemplateError {
        if (template.isEmpty()) return TemplateError.EMPTY
        if (template.length > MAX_TEMPLATE_LENGTH) return TemplateError.TOO_LONG
        if (template.any { it.isWhitespace() || it.isISOControl() }) return TemplateError.INVALID_CHARACTER
        if (!template.startsWith("https://", ignoreCase = true)) return TemplateError.NOT_HTTPS
        val rest = template.substring(8)
        val authority = rest.substringBefore('/').substringBefore('?').substringBefore('#')
        if (authority.contains(PLACEHOLDER)) return TemplateError.PLACEHOLDER_IN_HOST
        if (authority.isEmpty() || authority.contains('@')) return TemplateError.INVALID_HOST
        val host = authority.substringBefore(':')
        val port = authority.substringAfter(':', "")
        if (port.isNotEmpty() && !port.all { it.isDigit() }) return TemplateError.INVALID_HOST
        if (!Omnibox.looksLikeDomain(host.lowercase()) && host != "localhost") return TemplateError.INVALID_HOST
        val count = template.windowed(2).count { it == PLACEHOLDER }
        if (count == 0) return TemplateError.MISSING_PLACEHOLDER
        if (count > 1) return TemplateError.MULTIPLE_PLACEHOLDERS
        return TemplateError.OK
    }

    /** Percent-encode terms like the desktop build: unreserved kept, space -> '+'. */
    fun encodeTerms(terms: String): String {
        val sb = StringBuilder(terms.length * 3)
        for (b in terms.toByteArray(Charsets.UTF_8)) {
            val c = b.toInt() and 0xFF
            val ch = c.toChar()
            when {
                c < 0x80 && (ch.isLetterOrDigit() || ch == '-' || ch == '_' || ch == '.' || ch == '~') -> sb.append(ch)
                ch == ' ' -> sb.append('+')
                else -> sb.append('%').append(String.format("%02X", c))
            }
        }
        return sb.toString()
    }

    fun expand(template: String, terms: String): String {
        val encoded = encodeTerms(terms)
        val idx = template.indexOf(PLACEHOLDER)
        if (idx < 0) return template + encoded // legacy prefix template
        return template.substring(0, idx) + encoded + template.substring(idx + PLACEHOLDER.length)
    }

    /** "https://host/" of a template — the engine's home page. */
    fun home(template: String): String? = try {
        val uri = URI(template.replace(PLACEHOLDER, "x"))
        if (uri.scheme == null || uri.host == null) null
        else "${uri.scheme}://${uri.host}${if (uri.port > 0) ":" + uri.port else ""}/"
    } catch (_: Exception) {
        null
    }
}
