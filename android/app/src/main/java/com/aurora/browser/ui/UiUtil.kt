package com.aurora.browser.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.text.format.DateUtils
import android.view.View
import android.view.inputmethod.InputMethodManager
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import com.aurora.browser.core.Omnibox
import java.util.Locale

object UiUtil {
    /**
     * targetSdk 35 makes every window edge-to-edge; pad [view] by the system
     * bars (and the keyboard, so the omnibox/find bar stay visible).
     */
    fun applySystemBarInsets(view: View) {
        ViewCompat.setOnApplyWindowInsetsListener(view) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            val ime = insets.getInsets(WindowInsetsCompat.Type.ime())
            v.updatePadding(left = bars.left, top = bars.top, right = bars.right, bottom = maxOf(bars.bottom, ime.bottom))
            WindowInsetsCompat.CONSUMED
        }
    }

    /** First letter of the site name for the round letter tiles. */
    fun tileLetter(url: String, title: String): String {
        val host = Omnibox.hostOf(url)?.removePrefix("www.").orEmpty()
        val source = host.ifEmpty { title }
        return source.firstOrNull { it.isLetterOrDigit() }?.uppercaseChar()?.toString() ?: "•"
    }

    fun relativeTime(time: Long): CharSequence =
        DateUtils.getRelativeTimeSpanString(time, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS)

    fun compactCount(n: Long): String = when {
        n >= 1_000_000 -> String.format(Locale.US, "%.1fM", n / 1_000_000.0)
        n >= 10_000 -> String.format(Locale.US, "%dk", n / 1000)
        n >= 1_000 -> String.format(Locale.US, "%.1fk", n / 1000.0)
        else -> n.toString()
    }

    fun hideKeyboard(view: View) {
        val imm = view.context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(view.windowToken, 0)
    }

    fun showKeyboard(view: View) {
        val imm = view.context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT)
    }

    /** Ask MainActivity (singleTask) to open [url]. */
    fun openInBrowser(activity: Activity, url: String, newTab: Boolean = true, incognito: Boolean = false) {
        activity.startActivity(
            Intent(activity, MainActivity::class.java)
                .setAction(MainActivity.ACTION_OPEN_URL)
                .putExtra(MainActivity.EXTRA_URL, url)
                .putExtra(MainActivity.EXTRA_NEW_TAB, newTab)
                .putExtra(MainActivity.EXTRA_INCOGNITO, incognito)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
        )
    }
}
