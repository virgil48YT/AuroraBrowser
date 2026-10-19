package com.aurora.browser.ui

import android.content.Context
import android.util.AttributeSet
import androidx.appcompat.widget.AppCompatTextView
import androidx.core.content.res.ResourcesCompat
import com.aurora.browser.R

/**
 * A TextView that renders a single Font Awesome glyph. Used for every chrome
 * icon so the mobile app's iconography matches the desktop build instead of
 * flat grey Material vectors. The glyph can be set from XML via app:faGlyph
 * / app:faBrands or from code with [setFa].
 */
class FaIconView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : AppCompatTextView(context, attrs, defStyleAttr) {

    init {
        context.obtainStyledAttributes(attrs, R.styleable.FaIconView).apply {
            val glyph = getInt(R.styleable.FaIconView_faGlyph, 0)
            val brands = getBoolean(R.styleable.FaIconView_faBrands, false)
            if (glyph != 0) setFa(glyph, brands)
            recycle()
        }
        includeFontPadding = false
    }

    /** Sets the glyph text and the matching Font Awesome typeface. */
    fun setFa(codepoint: Int, brands: Boolean = false) {
        val fontRes = if (brands) R.font.fa_brands else R.font.fa_solid
        val typeface = ResourcesCompat.getFont(context, fontRes)
        if (typeface != null) setTypeface(typeface)
        text = FaIcons.glyph(codepoint)
    }
}
