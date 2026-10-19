package com.aurora.browser.ui

import android.os.Bundle
import androidx.core.view.isVisible
import com.aurora.browser.R
import com.aurora.browser.data.Prefs
import com.aurora.browser.search.SearchEngine
import com.aurora.browser.search.SearchEngines
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout

/** Pick the default engine; add/remove custom "%s" engines (desktop parity). */
class SearchEnginesActivity : ListPageActivity() {
    private lateinit var engines: SearchEngines
    private lateinit var adapter: RowAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        engines = SearchEngines(Prefs(this))
        adapter = RowAdapter(
            onClick = { row ->
                engines.setDefault((row.payload as SearchEngine).key)
                reload()
            },
            onAction = { row ->
                val e = row.payload as SearchEngine
                if (e.isCustom) {
                    engines.removeCustom(e.key)
                    reload()
                }
            },
        )
        list.adapter = adapter
        fab.setText(R.string.add_search_engine)
        fab.isVisible = true
        fab.setOnClickListener { addEngine() }
    }

    private fun addEngine() {
        val view = layoutInflater.inflate(R.layout.dialog_search_engine, null)
        val name = view.findViewById<TextInputEditText>(R.id.engineName)
        val template = view.findViewById<TextInputEditText>(R.id.engineTemplate)
        val templateLayout = view.findViewById<TextInputLayout>(R.id.engineTemplateLayout)
        template.setText("https://")
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.add_search_engine)
            .setView(view)
            .setPositiveButton(R.string.add, null)
            .setNegativeButton(R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val error = engines.addCustom(name.text?.toString().orEmpty(), template.text?.toString().orEmpty())
                if (error == null) {
                    dialog.dismiss()
                    reload()
                } else {
                    templateLayout.error = error
                }
            }
        }
        dialog.show()
    }

    override fun reload() {
        val current = engines.default().key
        adapter.submit(
            engines.all().mapIndexed { i, e ->
                Row(
                    i.toLong(),
                    e.name,
                    (if (e.isCustom) getString(R.string.custom) + " · " else "") + e.template,
                    if (e.key == current) R.drawable.ic_search else R.drawable.ic_globe,
                    actionIcon = if (e.isCustom) R.drawable.ic_delete else null,
                    payload = e,
                )
            },
        )
        title = getString(R.string.search_engines)
        toolbar.subtitle = engines.default().name
    }
}
