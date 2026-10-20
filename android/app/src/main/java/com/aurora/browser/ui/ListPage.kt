package com.aurora.browser.ui

import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.TextView
import androidx.annotation.DrawableRes
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.isVisible
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.aurora.browser.R
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton

/** One row in a list page. */
data class Row(
    val id: Long,
    val title: String,
    val subtitle: String,
    @DrawableRes val icon: Int,
    @DrawableRes val actionIcon: Int? = R.drawable.ic_delete,
    val payload: Any? = null,
    /** Page URL whose site favicon should replace [icon] once available. */
    val faviconUrl: String? = null,
)

class RowAdapter(
    private val onClick: (Row) -> Unit,
    private val onLongClick: ((Row) -> Unit)? = null,
    private val onAction: ((Row) -> Unit)? = null,
) : RecyclerView.Adapter<RowAdapter.Holder>() {
    private var rows: List<Row> = emptyList()

    fun submit(list: List<Row>) {
        rows = list
        @Suppress("NotifyDataSetChanged")
        notifyDataSetChanged()
    }

    class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val icon: ImageView = v.findViewById(R.id.rowIcon)
        val favicon: ImageView = v.findViewById(R.id.rowFavicon)
        val title: TextView = v.findViewById(R.id.rowTitle)
        val subtitle: TextView = v.findViewById(R.id.rowSubtitle)
        val action: ImageButton = v.findViewById(R.id.rowAction)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_list_row, parent, false))

    override fun getItemCount() = rows.size

    override fun onBindViewHolder(holder: Holder, position: Int) {
        val row = rows[position]
        holder.icon.setImageResource(row.icon)
        val faviconUrl = row.faviconUrl
        if (faviconUrl != null) {
            Favicons.load(holder.favicon, faviconUrl, fallback = holder.icon)
        } else {
            holder.favicon.setTag(R.id.favicon_key, null)
            holder.favicon.visibility = View.GONE
            holder.icon.visibility = View.VISIBLE
        }
        holder.title.text = row.title
        holder.subtitle.text = row.subtitle
        holder.subtitle.isVisible = row.subtitle.isNotEmpty()
        val actionIcon = row.actionIcon
        holder.action.isVisible = actionIcon != null && onAction != null
        if (actionIcon != null) holder.action.setImageResource(actionIcon)
        holder.action.setOnClickListener { onAction?.invoke(row) }
        holder.itemView.setOnClickListener { onClick(row) }
        holder.itemView.setOnLongClickListener {
            onLongClick?.invoke(row)
            onLongClick != null
        }
    }
}

/** Shared scaffold for History, Bookmarks, Downloads and Search engines. */
abstract class ListPageActivity : AppCompatActivity() {
    protected lateinit var toolbar: MaterialToolbar
    protected lateinit var list: RecyclerView
    protected lateinit var empty: TextView
    protected lateinit var search: EditText
    protected lateinit var fab: ExtendedFloatingActionButton

    protected open val searchable: Boolean = false
    protected var query: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(R.layout.activity_list)
        UiUtil.applySystemBarInsets(findViewById(R.id.pageRoot))
        toolbar = findViewById(R.id.pageToolbar)
        toolbar.title = title
        toolbar.setNavigationOnClickListener { finish() }
        list = findViewById(R.id.pageList)
        list.layoutManager = LinearLayoutManager(this)
        empty = findViewById(R.id.pageEmpty)
        fab = findViewById(R.id.pageFab)
        search = findViewById(R.id.pageSearch)
        search.isVisible = searchable
        search.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) {
                query = s?.toString().orEmpty()
                reload()
            }
        })
    }

    override fun onResume() {
        super.onResume()
        reload()
    }

    abstract fun reload()

    protected fun showEmpty(isEmpty: Boolean, text: Int) {
        empty.setText(text)
        empty.isVisible = isEmpty
    }
}
