package com.aurora.browser.ui

import android.widget.TextView
import androidx.core.content.res.ResourcesCompat
import com.aurora.browser.R
import com.aurora.browser.core.Omnibox

/**
 * Font Awesome Free codepoints used across Aurora Mobile, so the mobile icon
 * language matches the desktop chrome (tinted outline glyphs, no flat grey).
 *
 * Font Awesome Free is licensed under CC BY 4.0 (icons) and SIL OFL 1.1
 * (fonts); see android/README.md for attribution.
 */
object FaIcons {
    /** FA solid glyphs. */
    const val HOUSE = 0xF015
    const val MAGNIFYING_GLASS = 0xF002
    const val SHIELD_HALVED = 0xF3ED
    const val SHIELD = 0xF132
    const val XMARK = 0xF00D
    const val ELLIPSIS_VERTICAL = 0xF142
    const val LOCK = 0xF023
    const val GLOBE = 0xF0AC
    const val USER_SECRET = 0xF21B
    const val CLOCK_ROTATE_LEFT = 0xF1DA
    const val BOOKMARK = 0xF02E
    const val DOWNLOAD = 0xF019
    const val GEAR = 0xF013
    const val CHEVRON_UP = 0xF077
    const val CHEVRON_DOWN = 0xF078
    const val ARROW_UP_RIGHT_FROM_SQUARE = 0xF35D

    /** FA brand glyphs. */
    const val GOOGLE = 0xF1A0
    const val YOUTUBE = 0xF167
    const val WIKIPEDIA_W = 0xF266
    const val FACEBOOK_F = 0xF39E
    const val X_TWITTER = 0xE61B
    const val INSTAGRAM = 0xF16D
    const val GITHUB = 0xF09B
    const val REDDIT_ALIEN = 0xF281
    const val AMAZON = 0xF270
    const val TIKTOK = 0xE07B
    const val LINKEDIN_IN = 0xF0E1
    const val NETFLIX = 0xF268
    const val TWITCH = 0xF1E8
    const val DISCORD = 0xF392
    const val STACK_OVERFLOW = 0xF16C
    const val SPOTIFY = 0xF1BC
    const val PINTEREST_P = 0xF231
    const val TUMBLR = 0xF173
    const val WHATSAPP = 0xF232
    const val TELEGRAM = 0xF2C6
    const val STEAM_SYMBOL = 0xF3F6
    const val DROPBOX = 0xF16B
    const val APPLE = 0xF179
    const val MICROSOFT = 0xF17A
    const val YAHOO = 0xF19E
    const val EBAY = 0xF4F4
    const val PAYPAL = 0xF1ED
    const val GITLAB = 0xF296
    const val BITBUCKET = 0xF171
    const val SOUNDCLOUD = 0xF1BE
    const val QUORA = 0xF2C4
    const val VK = 0xF189
    const val MEDIUM = 0xF23A
    const val BLOGGER_B = 0xF37D
    const val WORDPRESS_SIMPLE = 0xF411

    fun glyph(codepoint: Int): String = String(Character.toChars(codepoint))

    /** Renders a glyph into a plain TextView (home-page badges, rows...). */
    fun applyTo(view: TextView, codepoint: Int, brands: Boolean) {
        val fontRes = if (brands) R.font.fa_brands else R.font.fa_solid
        ResourcesCompat.getFont(view.context, fontRes)?.let { view.typeface = it }
        view.text = glyph(codepoint)
    }
}

/**
 * Maps a site URL to a Font Awesome brand glyph, like the desktop shortcut
 * badges — known sites get their brand mark, unknown ones a globe.
 */
object SiteBrand {
    data class Glyph(val codepoint: Int, val brands: Boolean)

    private val FALLBACK = Glyph(FaIcons.GLOBE, brands = false)

    private val byKey: Map<String, Int> = mapOf(
        "google" to FaIcons.GOOGLE,
        "youtube" to FaIcons.YOUTUBE,
        "wikipedia" to FaIcons.WIKIPEDIA_W,
        "facebook" to FaIcons.FACEBOOK_F,
        "instagram" to FaIcons.INSTAGRAM,
        "github" to FaIcons.GITHUB,
        "gitlab" to FaIcons.GITLAB,
        "reddit" to FaIcons.REDDIT_ALIEN,
        "amazon" to FaIcons.AMAZON,
        "tiktok" to FaIcons.TIKTOK,
        "linkedin" to FaIcons.LINKEDIN_IN,
        "netflix" to FaIcons.NETFLIX,
        "twitch" to FaIcons.TWITCH,
        "discord" to FaIcons.DISCORD,
        "stackoverflow" to FaIcons.STACK_OVERFLOW,
        "stackexchange" to FaIcons.STACK_OVERFLOW,
        "spotify" to FaIcons.SPOTIFY,
        "pinterest" to FaIcons.PINTEREST_P,
        "tumblr" to FaIcons.TUMBLR,
        "whatsapp" to FaIcons.WHATSAPP,
        "telegram" to FaIcons.TELEGRAM,
        "steam" to FaIcons.STEAM_SYMBOL,
        "steampowered" to FaIcons.STEAM_SYMBOL,
        "dropbox" to FaIcons.DROPBOX,
        "apple" to FaIcons.APPLE,
        "icloud" to FaIcons.APPLE,
        "microsoft" to FaIcons.MICROSOFT,
        "office" to FaIcons.MICROSOFT,
        "live" to FaIcons.MICROSOFT,
        "yahoo" to FaIcons.YAHOO,
        "ebay" to FaIcons.EBAY,
        "paypal" to FaIcons.PAYPAL,
        "bitbucket" to FaIcons.BITBUCKET,
        "soundcloud" to FaIcons.SOUNDCLOUD,
        "quora" to FaIcons.QUORA,
        "vk" to FaIcons.VK,
        "medium" to FaIcons.MEDIUM,
        "blogger" to FaIcons.BLOGGER_B,
        "blogspot" to FaIcons.BLOGGER_B,
        "wordpress" to FaIcons.WORDPRESS_SIMPLE,
        "x.com" to FaIcons.X_TWITTER,
        "twitter" to FaIcons.X_TWITTER,
    )

    fun forUrl(url: String): Glyph {
        val host = Omnibox.hostOf(url)?.lowercase()?.removePrefix("www.") ?: return FALLBACK
        for ((key, codepoint) in byKey) {
            if (host == key || host.endsWith(".$key")) return Glyph(codepoint, brands = true)
        }
        return FALLBACK
    }
}
