package com.aurora.browser.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.aurora.browser.R
import com.aurora.browser.core.Omnibox
import com.aurora.browser.data.BrowserDb
import com.aurora.browser.search.SearchEngine

data class Suggestion(
    val kind: Kind,
    val title: String,
    val subtitle: String,
    /** What to load when tapped (a URL, or the raw query for SEARCH). */
    val target: String,
) {
    enum class Kind { SEARCH, URL, BOOKMARK, HISTORY }
}

/** Builds omnibox suggestions from local history and bookmarks only (nothing leaves the device). */
object SuggestionBuilder {
    fun build(query: String, engine: SearchEngine, db: BrowserDb?, includeLocal: Boolean): List<Suggestion> {
        val q = query.trim()
        if (q.isEmpty()) return emptyList()
        val out = ArrayList<Suggestion>()
        when (val input = Omnibox.classify(q)) {
            is Omnibox.Input.Navigate ->
                out.add(Suggestion(Suggestion.Kind.URL, Omnibox.displayUrl(input.url), input.url, input.url))
            else -> Unit
        }
        out.add(Suggestion(Suggestion.Kind.SEARCH, q, engine.name, q))
        if (includeLocal && db != null) {
            val seen = HashSet<String>()
            out.forEach { seen.add(it.target) }
            for (b in db.bookmarks(q).take(3)) {
                if (seen.add(b.url)) out.add(Suggestion(Suggestion.Kind.BOOKMARK, b.title.ifBlank { b.url }, b.url, b.url))
            }
            for (h in db.suggestHistory(q, 6)) {
                if (out.size >= 9) break
                if (seen.add(h.url)) out.add(Suggestion(Suggestion.Kind.HISTORY, h.title.ifBlank { h.url }, h.url, h.url))
            }
        }
        return out
    }
}

class SuggestionsAdapter(
    private val onPick: (Suggestion) -> Unit,
    private val onFill: (Suggestion) -> Unit,
) : RecyclerView.Adapter<SuggestionsAdapter.Holder>() {
    private var items: List<Suggestion> = emptyList()

    fun submit(list: List<Suggestion>) {
        items = list
        @Suppress("NotifyDataSetChanged")
        notifyDataSetChanged()
    }

    class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val icon: ImageView = v.findViewById(R.id.suggestionIcon)
        val title: TextView = v.findViewById(R.id.suggestionTitle)
        val url: TextView = v.findViewById(R.id.suggestionUrl)
        val fill: ImageButton = v.findViewById(R.id.suggestionFill)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder =
        Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_suggestion, parent, false))

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: Holder, position: Int) {
        val s = items[position]
        val ctx = holder.itemView.context
        holder.icon.setImageResource(
            when (s.kind) {
                Suggestion.Kind.SEARCH -> R.drawable.ic_search
                Suggestion.Kind.URL -> R.drawable.ic_globe
                Suggestion.Kind.BOOKMARK -> R.drawable.ic_bookmark
                Suggestion.Kind.HISTORY -> R.drawable.ic_history
            },
        )
        holder.title.text = s.title
        holder.url.text = when (s.kind) {
            Suggestion.Kind.SEARCH -> ctx.getString(R.string.search_with, s.subtitle)
            Suggestion.Kind.URL -> ctx.getString(R.string.go_to, s.subtitle)
            else -> Omnibox.displayUrl(s.subtitle)
        }
        holder.itemView.setOnClickListener { onPick(s) }
        holder.fill.setOnClickListener { onFill(s) }
    }
}
