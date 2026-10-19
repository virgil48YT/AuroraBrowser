package com.aurora.browser.ui

import android.content.res.ColorStateList
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.RecyclerView
import com.aurora.browser.R
import com.aurora.browser.tabs.Tab
import com.google.android.material.card.MaterialCardView

/** Chrome-style grid of tab cards: favicon + title + close, page thumbnail below. */
class TabSwitcherAdapter(
    private val onSelect: (Tab) -> Unit,
    private val onClose: (Tab) -> Unit,
) : RecyclerView.Adapter<TabSwitcherAdapter.Holder>() {

    var tabs: List<Tab> = emptyList()
        private set
    private var activeId: Long? = null

    fun submit(list: List<Tab>, active: Tab?) {
        tabs = list
        activeId = active?.id
        @Suppress("NotifyDataSetChanged")
        notifyDataSetChanged()
    }

    class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val card: MaterialCardView = v.findViewById(R.id.tabCard)
        val header: View = v.findViewById(R.id.tabCardHeader)
        val icon: ImageView = v.findViewById(R.id.tabCardIcon)
        val title: TextView = v.findViewById(R.id.tabCardTitle)
        val close: ImageButton = v.findViewById(R.id.tabCardClose)
        val thumb: ImageView = v.findViewById(R.id.tabCardThumb)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder =
        Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_tab_card, parent, false))

    override fun getItemCount(): Int = tabs.size

    override fun onBindViewHolder(holder: Holder, position: Int) {
        val tab = tabs[position]
        val ctx = holder.itemView.context
        val isActive = tab.id == activeId
        holder.title.text = tab.displayTitle
        val favicon = tab.favicon
        if (favicon != null) {
            holder.icon.setImageBitmap(favicon)
            holder.icon.imageTintList = null
        } else {
            holder.icon.setImageResource(if (tab.incognito) R.drawable.ic_incognito else R.drawable.ic_globe)
            holder.icon.imageTintList = ColorStateList.valueOf(ContextCompat.getColor(ctx, R.color.aurora_text_muted))
        }
        val thumb = tab.thumbnail
        when {
            thumb != null -> holder.thumb.setImageBitmap(thumb)
            tab.showingHome -> holder.thumb.setImageResource(R.drawable.aurora_bg)
            else -> holder.thumb.setImageDrawable(null)
        }
        holder.card.strokeColor = ContextCompat.getColor(ctx, if (isActive) R.color.aurora_accent else R.color.aurora_border)
        holder.card.strokeWidth = (ctx.resources.displayMetrics.density * if (isActive) 2.5f else 1f).toInt()
        holder.header.setBackgroundColor(
            ContextCompat.getColor(ctx, if (tab.incognito) R.color.incognito_surface else R.color.aurora_card),
        )
        holder.title.setTextColor(ContextCompat.getColor(ctx, if (tab.incognito) R.color.incognito_text else R.color.aurora_text))
        holder.itemView.setOnClickListener { onSelect(tab) }
        holder.close.setOnClickListener { onClose(tab) }
    }
}
