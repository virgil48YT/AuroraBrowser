package com.aurora.browser.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.util.AttributeSet
import android.widget.FrameLayout
import androidx.core.content.ContextCompat
import com.aurora.browser.R

/**
 * The desktop home page's "glow frame" (src/ui/GlowFrame.cpp) for Android:
 * a dark translucent card with a cyan -> purple -> magenta gradient hairline
 * and a soft outer glow. The glow is faked with a few widening, fading
 * strokes so it stays hardware accelerated (BlurMaskFilter would force a
 * software layer).
 */
class GlowFrameLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : FrameLayout(context, attrs, defStyleAttr) {

    private val density = resources.displayMetrics.density
    var cornerRadius: Float = 20f * density
    private val glowSpread: Float
    private val strokeWidth: Float
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val rect = RectF()
    private val colors: IntArray
    private var glowAlpha: Int

    init {
        setWillNotDraw(false)
        val a = context.obtainStyledAttributes(attrs, R.styleable.GlowFrameLayout, defStyleAttr, 0)
        cornerRadius = a.getDimension(R.styleable.GlowFrameLayout_glowCornerRadius, cornerRadius)
        glowSpread = a.getDimension(R.styleable.GlowFrameLayout_glowSpread, 10f * density)
        strokeWidth = a.getDimension(R.styleable.GlowFrameLayout_glowStrokeWidth, 1.5f * density)
        fillPaint.color = a.getColor(R.styleable.GlowFrameLayout_glowFill, ContextCompat.getColor(context, R.color.home_card))
        glowAlpha = a.getInt(R.styleable.GlowFrameLayout_glowAlpha, 70)
        a.recycle()
        colors = intArrayOf(
            ContextCompat.getColor(context, R.color.home_cyan),
            ContextCompat.getColor(context, R.color.home_purple),
            ContextCompat.getColor(context, R.color.home_magenta),
        )
        strokePaint.strokeWidth = strokeWidth
        // Leave room for the glow so it is not clipped by our own bounds.
        val pad = glowSpread.toInt()
        setPadding(paddingLeft + pad, paddingTop + pad, paddingRight + pad, paddingBottom + pad)
        clipToPadding = true
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val shader = LinearGradient(0f, 0f, w.toFloat(), h.toFloat(), colors, null, Shader.TileMode.CLAMP)
        strokePaint.shader = shader
        glowPaint.shader = LinearGradient(0f, 0f, w.toFloat(), h.toFloat(), colors, null, Shader.TileMode.CLAMP)
    }

    override fun onDraw(canvas: Canvas) {
        val inset = glowSpread
        rect.set(inset, inset, width - inset, height - inset)
        // Outer glow: concentric strokes, widest and faintest first.
        val steps = 5
        for (i in steps downTo 1) {
            val spread = glowSpread * i / steps
            glowPaint.strokeWidth = spread * 2
            glowPaint.alpha = (glowAlpha / (i + 1.5f)).toInt()
            canvas.drawRoundRect(rect, cornerRadius + spread / 2, cornerRadius + spread / 2, glowPaint)
        }
        canvas.drawRoundRect(rect, cornerRadius, cornerRadius, fillPaint)
        canvas.drawRoundRect(rect, cornerRadius, cornerRadius, strokePaint)
        super.onDraw(canvas)
    }
}
