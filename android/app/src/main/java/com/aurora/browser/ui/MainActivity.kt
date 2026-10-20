package com.aurora.browser.ui

import android.annotation.SuppressLint
import android.app.Activity
import android.app.SearchManager
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.MutableContextWrapper
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Message
import android.text.Editable
import android.text.TextWatcher
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.webkit.CookieManager
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebView
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.ItemTouchHelper
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout
import com.aurora.browser.R
import com.aurora.browser.adblock.AdblockService
import com.aurora.browser.core.Omnibox
import com.aurora.browser.data.BrowserDb
import com.aurora.browser.data.Prefs
import com.aurora.browser.search.SearchEngines
import com.aurora.browser.tabs.BrowserHost
import com.aurora.browser.tabs.BrowserState
import com.aurora.browser.tabs.BrowserWebView
import com.aurora.browser.tabs.IncognitoProfile
import com.aurora.browser.tabs.Tab
import com.aurora.browser.tor.TorManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.tabs.TabLayout

class MainActivity : AppCompatActivity(), BrowserHost, HomeController.Callbacks {

    companion object {
        const val ACTION_OPEN_URL = "com.aurora.browser.OPEN_URL"
        const val EXTRA_URL = "url"
        const val EXTRA_NEW_TAB = "new_tab"
        const val EXTRA_INCOGNITO = "incognito"
    }

    override val activity: Activity get() = this

    private lateinit var prefs: Prefs
    private lateinit var db: BrowserDb
    private lateinit var adblock: AdblockService
    private lateinit var engines: SearchEngines
    private val tabs get() = BrowserState.tabs
    private val handler = Handler(Looper.getMainLooper())

    // Views
    private lateinit var root: FrameLayout
    private lateinit var browserColumn: LinearLayout
    private lateinit var toolbar: LinearLayout
    private lateinit var omniboxFrame: View
    private lateinit var omnibox: EditText
    private lateinit var siteIcon: FaIconView
    private lateinit var btnClearOmnibox: FaIconView
    private lateinit var btnHome: FaIconView
    private lateinit var btnMenu: FaIconView
    private lateinit var shieldIcon: FaIconView
    private lateinit var shieldBadge: TextView
    private lateinit var tabCount: TextView
    private lateinit var tabIcon: FaIconView
    private lateinit var torBadge: ImageView
    private lateinit var progress: LinearProgressIndicator
    private lateinit var swipeRefresh: SwipeRefreshLayout
    private lateinit var webHolder: FrameLayout
    private lateinit var homeView: View
    private lateinit var suggestions: RecyclerView
    private lateinit var findBar: View
    private lateinit var findInput: EditText
    private lateinit var findCount: TextView
    private lateinit var switcher: View
    private lateinit var switcherModes: TabLayout
    private lateinit var switcherGrid: RecyclerView
    private lateinit var switcherEmpty: View
    private lateinit var fullscreenHolder: FrameLayout

    private lateinit var home: HomeController
    private lateinit var suggestionsAdapter: SuggestionsAdapter
    private lateinit var switcherAdapter: TabSwitcherAdapter
    private var switcherIncognito = false
    private var ignoreOmniboxChanges = false

    private var customView: View? = null
    private var customViewCallback: WebChromeClient.CustomViewCallback? = null
    private var fileCallback: ValueCallback<Array<Uri>>? = null
    private lateinit var fileChooser: ActivityResultLauncher<Intent>

    // --- Lifecycle ---------------------------------------------------------------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(R.layout.activity_main)
        prefs = Prefs(this)
        db = BrowserDb.get(this)
        adblock = AdblockService.get(this)
        engines = SearchEngines(prefs)
        BrowserState.host = this

        fileChooser = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            fileCallback?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(result.resultCode, result.data))
            fileCallback = null
        }

        bindViews()
        switcher.isVisible = false
        setupInsets()
        setupToolbar()
        setupFindBar()
        setupSwitcher()
        home = HomeController(homeView, db, adblock, this)
        onBackPressedDispatcher.addCallback(this, backCallback)
        TorManager.addListener(torListener)

        if (tabs.tabs.isNotEmpty()) {
            // Activity was recreated (e.g. theme change): re-home the live WebViews.
            tabs.tabs.forEach { t -> (t.webView?.context as? MutableContextWrapper)?.baseContext = this }
            render()
        } else {
            val restored = if (prefs.restoreTabs) tabs.restore(prefs.sessionJson) else null
            activateTab(restored ?: tabs.create(incognito = false))
        }
        if (savedInstanceState == null) handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        BrowserState.host = this
        tabs.active?.webView?.onResume()
        if (tabs.active?.showingHome == true) {
            home.bind(tabs.active!!.incognito)
            home.onShown()
        }
        updateToolbar()
    }

    override fun onPause() {
        super.onPause()
        tabs.active?.webView?.onPause()
        home.onHidden()
        saveSession()
        adblock.flushStats()
        CookieManager.getInstance().flush()
    }

    override fun onDestroy() {
        super.onDestroy()
        TorManager.removeListener(torListener)
        if (BrowserState.host === this) BrowserState.host = null
        if (isFinishing && !isChangingConfigurations) {
            val hadIncognito = tabs.incognitoTabs().isNotEmpty()
            for (t in tabs.tabs.toList()) {
                destroyWebView(t)
                tabs.remove(t)
            }
            tabs.setActive(null)
            if (hadIncognito) IncognitoProfile.wipe()
        } else {
            // Detach so the WebViews are not tied to the dying view hierarchy.
            // Only touch views this instance owns: a recreated activity may
            // already have adopted them.
            tabs.tabs.forEach { t -> if (t.webView?.parent === webHolder) webHolder.removeView(t.webView) }
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updateSwitcherSpan()
    }

    private fun saveSession() {
        prefs.sessionJson = if (prefs.restoreTabs) tabs.serialize() else ""
    }

    // --- View setup --------------------------------------------------------------

    private fun bindViews() {
        root = findViewById(R.id.root)
        browserColumn = findViewById(R.id.browserColumn)
        toolbar = findViewById(R.id.toolbar)
        omniboxFrame = findViewById(R.id.omniboxFrame)
        omnibox = findViewById(R.id.omnibox)
        siteIcon = findViewById(R.id.siteIcon)
        btnClearOmnibox = findViewById(R.id.btnClearOmnibox)
        btnHome = findViewById(R.id.btnHome)
        btnMenu = findViewById(R.id.btnMenu)
        shieldIcon = findViewById(R.id.shieldIcon)
        shieldBadge = findViewById(R.id.shieldBadge)
        tabCount = findViewById(R.id.tabCount)
        tabIcon = findViewById(R.id.tabIcon)
        torBadge = findViewById(R.id.torBadge)
        progress = findViewById(R.id.progress)
        swipeRefresh = findViewById(R.id.swipeRefresh)
        webHolder = findViewById(R.id.webHolder)
        homeView = findViewById(R.id.homeView)
        suggestions = findViewById(R.id.suggestions)
        findBar = findViewById(R.id.findBar)
        findInput = findViewById(R.id.findInput)
        findCount = findViewById(R.id.findCount)
        switcher = findViewById(R.id.tabSwitcher)
        switcherModes = findViewById(R.id.switcherModes)
        switcherGrid = findViewById(R.id.switcherGrid)
        switcherEmpty = findViewById(R.id.switcherEmpty)
        fullscreenHolder = findViewById(R.id.fullscreenHolder)
    }

    private fun setupInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            val ime = insets.getInsets(WindowInsetsCompat.Type.ime())
            val bottom = maxOf(bars.bottom, ime.bottom)
            browserColumn.updatePadding(left = bars.left, top = bars.top, right = bars.right, bottom = bottom)
            switcher.updatePadding(left = bars.left, top = bars.top, right = bars.right, bottom = bars.bottom)
            WindowInsetsCompat.CONSUMED
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupToolbar() {
        btnHome.setOnClickListener { goHome() }
        btnMenu.setOnClickListener { showMenu() }
        findViewById<View>(R.id.btnTabs).apply {
            setOnClickListener { showSwitcher() }
            setOnLongClickListener {
                activateTab(tabs.create(tabs.active?.incognito ?: false, afterActive = true))
                Toast.makeText(this@MainActivity, R.string.new_tab_opened, Toast.LENGTH_SHORT).show()
                true
            }
        }
        findViewById<View>(R.id.btnShield).setOnClickListener { showShields() }
        btnClearOmnibox.setOnClickListener { omnibox.setText("") }
        torBadge.setOnClickListener { showTorStatus() }

        swipeRefresh.setColorSchemeColors(
            ContextCompat.getColor(this, R.color.home_cyan),
            ContextCompat.getColor(this, R.color.home_purple),
            ContextCompat.getColor(this, R.color.home_magenta),
        )
        swipeRefresh.setOnRefreshListener {
            tabs.active?.webView?.reload()
            handler.postDelayed({ swipeRefresh.isRefreshing = false }, 600)
        }
        // Only pull-to-refresh when the page itself is scrolled to the top.
        swipeRefresh.setOnChildScrollUpCallback { _, _ ->
            val wv = tabs.active?.webView
            wv == null || wv.scrollY > 0 || wv.canScrollVertically(-1)
        }

        suggestionsAdapter = SuggestionsAdapter(
            onPick = { s ->
                if (s.kind == Suggestion.Kind.SEARCH) {
                    loadInActive(com.aurora.browser.core.SearchTemplates.expand(engines.default().template, s.target))
                } else {
                    loadInActive(s.target)
                }
                finishEditing()
            },
            onFill = { s ->
                omnibox.setText(s.target)
                omnibox.setSelection(omnibox.text.length)
            },
        )
        suggestions.layoutManager = LinearLayoutManager(this)
        suggestions.adapter = suggestionsAdapter

        omnibox.setOnFocusChangeListener { _, focused ->
            if (focused) {
                val tab = tabs.active
                ignoreOmniboxChanges = true
                omnibox.setText(if (tab == null || tab.showingHome) "" else tab.url)
                ignoreOmniboxChanges = false
                omnibox.selectAll()
                btnClearOmnibox.isVisible = omnibox.text.isNotEmpty()
                UiUtil.showKeyboard(omnibox)
            } else {
                suggestions.isVisible = false
                btnClearOmnibox.isVisible = false
                updateToolbar()
            }
        }
        omnibox.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) {
                if (ignoreOmniboxChanges || !omnibox.hasFocus()) return
                btnClearOmnibox.isVisible = !s.isNullOrEmpty()
                updateSuggestions(s?.toString().orEmpty())
            }
        })
        omnibox.setOnEditorActionListener { _, actionId, event ->
            val isEnter = event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN
            if (actionId == EditorInfo.IME_ACTION_GO || isEnter) {
                submitOmnibox(omnibox.text.toString())
                true
            } else {
                false
            }
        }
    }

    private fun updateSuggestions(text: String) {
        val tab = tabs.active
        val list = SuggestionBuilder.build(
            text, engines.default(),
            // Incognito never reads from (or writes to) local history.
            if (tab?.incognito == true) null else db,
            prefs.showSuggestions,
        )
        suggestionsAdapter.submit(list)
        suggestions.isVisible = list.isNotEmpty()
    }

    private fun submitOmnibox(text: String) {
        val url = Omnibox.resolve(text, engines.default().template, prefs.httpsOnly) ?: return
        loadInActive(url)
        finishEditing()
    }

    private fun finishEditing() {
        suggestions.isVisible = false
        omnibox.clearFocus()
        UiUtil.hideKeyboard(omnibox)
        tabs.active?.webView?.requestFocus()
    }

    private fun setupFindBar() {
        findInput.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) {
                tabs.active?.webView?.findAllAsync(s?.toString().orEmpty())
            }
        })
        findInput.setOnEditorActionListener { _, _, _ ->
            tabs.active?.webView?.findNext(true)
            true
        }
        findViewById<View>(R.id.findNext).setOnClickListener { tabs.active?.webView?.findNext(true) }
        findViewById<View>(R.id.findPrev).setOnClickListener { tabs.active?.webView?.findNext(false) }
        findViewById<View>(R.id.findClose).setOnClickListener { closeFind() }
    }

    private fun openFind() {
        val wv = tabs.active?.webView ?: return
        findBar.isVisible = true
        findCount.text = ""
        wv.setFindListener { active, count, _ ->
            findCount.text = if (count == 0) "0/0" else "${active + 1}/$count"
        }
        findInput.setText("")
        findInput.requestFocus()
        UiUtil.showKeyboard(findInput)
    }

    private fun closeFind() {
        findBar.isVisible = false
        tabs.active?.webView?.clearMatches()
        UiUtil.hideKeyboard(findInput)
    }

    private fun setupSwitcher() {
        switcherAdapter = TabSwitcherAdapter(
            onSelect = { tab ->
                activateTab(tab)
                hideSwitcher()
            },
            onClose = { tab ->
                closeTab(tab)
                refreshSwitcher()
            },
        )
        switcherGrid.adapter = switcherAdapter
        updateSwitcherSpan()
        ItemTouchHelper(object : ItemTouchHelper.SimpleCallback(0, ItemTouchHelper.LEFT or ItemTouchHelper.RIGHT) {
            override fun onMove(rv: RecyclerView, vh: RecyclerView.ViewHolder, target: RecyclerView.ViewHolder) = false
            override fun onSwiped(vh: RecyclerView.ViewHolder, direction: Int) {
                val tab = switcherAdapter.tabs.getOrNull(vh.bindingAdapterPosition) ?: return
                closeTab(tab)
                refreshSwitcher()
            }
        }).attachToRecyclerView(switcherGrid)

        switcherModes.addTab(switcherModes.newTab().setText(R.string.tabs))
        switcherModes.addTab(switcherModes.newTab().setText(R.string.incognito).setIcon(R.drawable.ic_incognito))
        switcherModes.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
            override fun onTabSelected(t: TabLayout.Tab) {
                switcherIncognito = t.position == 1
                refreshSwitcher()
            }

            override fun onTabUnselected(t: TabLayout.Tab) = Unit
            override fun onTabReselected(t: TabLayout.Tab) = Unit
        })
        val newTab = { activateTab(tabs.create(switcherIncognito, afterActive = false)); hideSwitcher() }
        findViewById<View>(R.id.switcherNewTab).setOnClickListener { newTab() }
        findViewById<ExtendedFloatingActionButton>(R.id.switcherFab).setOnClickListener { newTab() }
        findViewById<View>(R.id.switcherMenu).setOnClickListener { v ->
            val popup = PopupMenu(this, v)
            popup.menuInflater.inflate(R.menu.switcher_menu, popup.menu)
            popup.menu.findItem(R.id.action_close_incognito).isVisible = tabs.incognitoTabs().isNotEmpty()
            popup.setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    R.id.action_new_incognito -> {
                        activateTab(tabs.create(true))
                        hideSwitcher()
                    }
                    R.id.action_reopen_closed -> {
                        val closed = tabs.popClosed()
                        if (closed != null) {
                            val t = tabs.create(false, closed.first, closed.second)
                            activateTab(t)
                            hideSwitcher()
                        }
                    }
                    R.id.action_close_all -> {
                        tabs.tabs.filter { it.incognito == switcherIncognito }.forEach { closeTab(it, activateNext = false) }
                        ensureActiveTab()
                        refreshSwitcher()
                    }
                    R.id.action_close_incognito -> {
                        tabs.incognitoTabs().forEach { closeTab(it, activateNext = false) }
                        ensureActiveTab()
                        refreshSwitcher()
                    }
                }
                true
            }
            popup.show()
        }
    }

    private fun updateSwitcherSpan() {
        val widthDp = resources.configuration.screenWidthDp
        val span = (widthDp / 180).coerceIn(2, 5)
        val lm = switcherGrid.layoutManager as? GridLayoutManager
        if (lm == null) switcherGrid.layoutManager = GridLayoutManager(this, span) else lm.spanCount = span
    }

    // --- Tabs --------------------------------------------------------------------

    private fun activateTab(tab: Tab) {
        val previous = tabs.active
        if (previous != null && previous !== tab) {
            captureThumbnail(previous)
            previous.webView?.onPause()
        }
        tabs.setActive(tab)
        if (findBar.isVisible) closeFind()
        render()
    }

    private fun ensureActiveTab() {
        val active = tabs.active
        if (active == null || active !in tabs.tabs) {
            activateTab(tabs.tabs.lastOrNull { it.incognito == switcherIncognito } ?: tabs.tabs.lastOrNull() ?: tabs.create(false))
        }
    }

    /** Shows the active tab's content (start page or WebView). */
    private fun render() {
        val tab = tabs.active ?: return
        if (tab.showingHome) {
            tab.webView?.let {
                it.onPause()
                (it.parent as? ViewGroup)?.removeView(it)
            }
            swipeRefresh.isVisible = false
            homeView.isVisible = true
            home.bind(tab.incognito)
            home.onShown()
        } else {
            home.onHidden()
            homeView.isVisible = false
            swipeRefresh.isVisible = true
            val wv = tab.webView ?: createWebView(tab).also { it.loadUrl(tab.url) }
            // WebViews are swapped inside webHolder; SwipeRefreshLayout keeps a
            // single stable child (it caches its target view).
            if (wv.parent !== webHolder) {
                (wv.parent as? ViewGroup)?.removeView(wv)
                webHolder.removeAllViews()
                webHolder.addView(wv, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            }
            wv.onResume()
        }
        updateToolbar()
    }

    private fun createWebView(tab: Tab): WebView {
        val wv = BrowserWebView.create(this, tab)
        tab.webView = wv
        return wv
    }

    private fun destroyWebView(tab: Tab) {
        val wv = tab.webView ?: return
        tab.webView = null
        try {
            wv.stopLoading()
            (wv.parent as? ViewGroup)?.removeView(wv)
            wv.webChromeClient = null
            wv.destroy()
        } catch (_: Exception) {
        }
    }

    /** Load [url] in the active tab ([Omnibox.HOME_URL] shows the start page). */
    private fun loadInActive(url: String) {
        val tab = tabs.active ?: tabs.create(false).also { tabs.setActive(it) }
        loadInTab(tab, url)
    }

    private fun loadInTab(tab: Tab, url: String) {
        if (url == Omnibox.HOME_URL) {
            showHome(tab, detour = false)
            return
        }
        tab.homeDetour = false
        tab.url = url
        tab.pageHost = Omnibox.hostOf(url).orEmpty()
        val wv = tab.webView
        if (wv == null) {
            createWebView(tab).loadUrl(url)
        } else {
            wv.loadUrl(url)
        }
        if (tab === tabs.active) render()
    }

    private fun showHome(tab: Tab, detour: Boolean) {
        tab.webView?.stopLoading()
        tab.homeDetour = detour && tab.webView?.url != null
        tab.url = Omnibox.HOME_URL
        tab.title = ""
        tab.favicon = null
        tab.blockedOnPage.set(0)
        tab.isLoading = false
        tab.progress = 100
        if (tab === tabs.active) render()
    }

    private fun goHome() {
        val tab = tabs.active ?: return
        if (!tab.showingHome) showHome(tab, detour = true)
    }

    private fun openInNewTab(url: String, incognito: Boolean, background: Boolean = false) {
        val tab = tabs.create(incognito, afterActive = true)
        if (background) {
            tab.url = url
            tab.title = Omnibox.displayUrl(url)
            updateToolbar()
        } else {
            activateTab(tab)
            loadInTab(tab, url)
        }
    }

    private fun captureThumbnail(tab: Tab) {
        if (tab.showingHome) {
            tab.thumbnail = null
            return
        }
        val wv = tab.webView ?: return
        if (wv.width == 0 || wv.height == 0 || wv.parent == null) return
        try {
            val scale = 0.4f
            val h = minOf(wv.height, (wv.width * 1.4f).toInt())
            val bmp = Bitmap.createBitmap((wv.width * scale).toInt(), (h * scale).toInt(), Bitmap.Config.RGB_565)
            val c = Canvas(bmp)
            c.scale(scale, scale)
            c.translate(-wv.scrollX.toFloat(), -wv.scrollY.toFloat())
            wv.draw(c)
            tab.thumbnail = bmp
        } catch (_: Throwable) {
            // Thumbnails are cosmetic; never let them crash the browser.
        }
    }

    override fun closeTab(tab: Tab) = closeTab(tab, activateNext = true)

    private fun closeTab(tab: Tab, activateNext: Boolean) {
        val wasActive = tab === tabs.active
        val next = tabs.remove(tab)
        destroyWebView(tab)
        tab.thumbnail = null
        if (tab.incognito && tabs.incognitoTabs().isEmpty()) {
            // Last incognito tab: erase the incognito profile once teardown settles.
            handler.postDelayed({ if (tabs.incognitoTabs().isEmpty()) IncognitoProfile.wipe() }, 750)
        }
        if (wasActive) {
            tabs.setActive(null)
            if (activateNext) activateTab(next ?: tabs.create(false))
        } else {
            updateToolbar()
        }
    }

    // --- Toolbar state -------------------------------------------------------------

    private fun updateToolbar() {
        val tab = tabs.active ?: return
        val incognito = tab.incognito
        val surface = ContextCompat.getColor(this, if (incognito) R.color.incognito_surface else R.color.chrome_bg)
        val textColor = ContextCompat.getColor(this, if (incognito) R.color.incognito_text else R.color.chrome_text)
        val iconColor = if (incognito) textColor else textColor
        toolbar.setBackgroundColor(surface)
        root.setBackgroundColor(surface)
        omniboxFrame.backgroundTintList =
            if (incognito) ColorStateList.valueOf(ContextCompat.getColor(this, R.color.incognito_omnibox)) else null
        omnibox.setTextColor(textColor)
        omnibox.setHint(
            when {
                TorManager.enabled -> R.string.tor_hint
                incognito -> R.string.omnibox_hint_incognito
                else -> R.string.omnibox_hint
            },
        )
        updateTorBadge()
        btnHome.setTextColor(iconColor)
        btnMenu.setTextColor(iconColor)
        btnClearOmnibox.setTextColor(iconColor)
        // Outline and glyph follow the chrome colour; the count badge is the
        // same colour with the toolbar colour as its text (an inverted pill).
        tabIcon.setTextColor(iconColor)
        (tabIcon.background?.mutate() as? GradientDrawable)
            ?.setStroke((2 * resources.displayMetrics.density).toInt().coerceAtLeast(1), iconColor)
        (tabCount.background?.mutate() as? GradientDrawable)?.setColor(iconColor)
        tabCount.setTextColor(surface)
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
        WindowInsetsControllerCompat(window, root).apply {
            // The chrome is always the deep-navy aurora toolbar, so status-bar
            // icons are always light, like the desktop build.
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = !night
        }

        if (!omnibox.hasFocus()) {
            ignoreOmniboxChanges = true
            omnibox.setText(if (tab.showingHome) "" else Omnibox.displayUrl(tab.url))
            ignoreOmniboxChanges = false
        }
        siteIcon.setFa(
            when {
                tab.showingHome && incognito -> FaIcons.USER_SECRET
                tab.showingHome -> FaIcons.MAGNIFYING_GLASS
                tab.url.startsWith("https://") -> FaIcons.LOCK
                else -> FaIcons.GLOBE
            },
        )
        siteIcon.setTextColor(iconColor)

        val sameMode = tabs.tabs.count { it.incognito == incognito }
        tabCount.text = if (sameMode > 99) "99+" else sameMode.toString()
        findViewById<View>(R.id.btnTabs).contentDescription =
            resources.getQuantityString(R.plurals.tab_switcher_count, sameMode, sameMode)

        progress.isVisible = tab.isLoading && tab.progress < 100 && !tab.showingHome
        progress.setProgressCompat(tab.progress, true)
        updateShield()
    }

    private fun updateShield() {
        val tab = tabs.active ?: return
        val web = !tab.showingHome && Omnibox.isWebUrl(tab.url)
        val up = web && adblock.shieldsUp(tab.pageHost)
        shieldIcon.setFa(if (up || !web) FaIcons.SHIELD_HALVED else FaIcons.SHIELD)
        val color = when {
            up -> R.color.chrome_cyan
            web -> R.color.chrome_shield_off
            else -> if (tab.incognito) R.color.incognito_text else R.color.chrome_text
        }
        shieldIcon.setTextColor(ContextCompat.getColor(this, color))
        val count = tab.blockedOnPage.get()
        shieldBadge.isVisible = up && count > 0
        shieldBadge.text = if (count > 99) "99+" else count.toString()
    }

    // --- BrowserHost -------------------------------------------------------------

    override fun onTabStateChanged(tab: Tab) {
        if (tab !== tabs.active) return
        if (tab.showingHome != homeView.isVisible) render() else updateToolbar()
        if (!tab.incognito && !TorManager.enabled && !tab.showingHome && tab.title.isNotBlank()) db.updateTitle(tab.url, tab.title)
    }

    override fun onTabProgress(tab: Tab) {
        if (tab !== tabs.active) return
        progress.isVisible = tab.progress < 100 && !tab.showingHome
        progress.setProgressCompat(tab.progress, true)
    }

    override fun onTabBlockedCountChanged(tab: Tab) {
        handler.post { if (tab === tabs.active) updateShield() }
    }

    override fun onPageFinished(tab: Tab, url: String) {
        if (tab === tabs.active) updateToolbar()
        // Like Brave's Tor windows, nothing browsed over Tor is written to history.
        if (!tab.incognito && !TorManager.enabled && prefs.saveHistory && Omnibox.isWebUrl(url)) {
            db.recordVisit(url, tab.title)
        }
    }

    override fun openPopupTab(opener: Tab, resultMsg: Message): Boolean {
        val tab = tabs.create(opener.incognito, url = "about:blank", openerId = opener.id, afterActive = true)
        val wv = createWebView(tab)
        val transport = resultMsg.obj as? WebView.WebViewTransport ?: return false
        transport.webView = wv
        resultMsg.sendToTarget()
        activateTab(tab)
        return true
    }

    override fun showCustomView(view: View, callback: WebChromeClient.CustomViewCallback) {
        if (customView != null) {
            callback.onCustomViewHidden()
            return
        }
        customView = view
        customViewCallback = callback
        fullscreenHolder.addView(view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        fullscreenHolder.isVisible = true
        WindowInsetsControllerCompat(window, root).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    override fun hideCustomView() {
        val view = customView ?: return
        fullscreenHolder.removeView(view)
        fullscreenHolder.isVisible = false
        customView = null
        WindowInsetsControllerCompat(window, root).show(WindowInsetsCompat.Type.systemBars())
        customViewCallback?.onCustomViewHidden()
        customViewCallback = null
    }

    override fun showFileChooser(callback: ValueCallback<Array<Uri>>, params: WebChromeClient.FileChooserParams): Boolean {
        fileCallback?.onReceiveValue(null)
        fileCallback = callback
        return try {
            fileChooser.launch(params.createIntent())
            true
        } catch (_: ActivityNotFoundException) {
            fileCallback = null
            callback.onReceiveValue(null)
            false
        }
    }

    override fun startDownload(tab: Tab, url: String, userAgent: String, contentDisposition: String, mimeType: String) {
        if (TorManager.enabled && !url.startsWith("data:", ignoreCase = true)) {
            // DownloadManager runs in another process and ignores the WebView
            // proxy, so a download would leave Tor. Ask first.
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.tor_download_title)
                .setMessage(R.string.tor_download_message)
                .setPositiveButton(R.string.tor_download_anyway) { _, _ ->
                    doStartDownload(tab, url, userAgent, contentDisposition, mimeType)
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
            return
        }
        doStartDownload(tab, url, userAgent, contentDisposition, mimeType)
    }

    private fun doStartDownload(tab: Tab, url: String, userAgent: String, contentDisposition: String, mimeType: String) {
        val cookies = if (tab.incognito) IncognitoProfile.cookieManager() else CookieManager.getInstance()
        Downloads.start(this, url, userAgent, contentDisposition, mimeType, cookies)
        // A download opened via target=_blank leaves an empty popup tab behind; close it.
        val wv = tab.webView
        if (tab.openerId != null && (wv == null || wv.url == null || wv.url == "about:blank" || !wv.canGoBack())) {
            handler.post { if (tab in tabs.tabs) closeTab(tab) }
        }
    }

    override fun onRenderProcessGone(tab: Tab) {
        destroyWebView(tab)
        if (tab === tabs.active) {
            Toast.makeText(this, R.string.page_load_failed, Toast.LENGTH_SHORT).show()
            render() // recreates the WebView and reloads tab.url
        }
    }

    // --- Home callbacks ----------------------------------------------------------

    override fun onHomeSearchTapped() {
        omnibox.requestFocus()
    }

    override fun onHomeOpen(url: String) = loadInActive(url)

    override fun onHomeAction(action: HomeController.Action) {
        when (action) {
            HomeController.Action.INCOGNITO -> activateTab(tabs.create(true, afterActive = true))
            HomeController.Action.HISTORY -> startActivity(Intent(this, HistoryActivity::class.java))
            HomeController.Action.BOOKMARKS -> startActivity(Intent(this, BookmarksActivity::class.java))
            HomeController.Action.DOWNLOADS -> startActivity(Intent(this, DownloadsActivity::class.java))
            HomeController.Action.SETTINGS -> startActivity(Intent(this, SettingsActivity::class.java))
        }
    }

    // --- Tab switcher ------------------------------------------------------------

    private fun showSwitcher() {
        tabs.active?.let { captureThumbnail(it) }
        finishEditing()
        switcherIncognito = tabs.active?.incognito ?: false
        switcherModes.getTabAt(if (switcherIncognito) 1 else 0)?.select()
        refreshSwitcher()
        switcher.isVisible = true
    }

    private fun hideSwitcher() {
        ensureActiveTab()
        switcher.isVisible = false
        render()
    }

    private fun refreshSwitcher() {
        val list = tabs.tabs.filter { it.incognito == switcherIncognito }
        switcherAdapter.submit(list, tabs.active)
        switcherEmpty.isVisible = list.isEmpty()
        switcherModes.getTabAt(0)?.text = getString(R.string.tabs_count, tabs.normalTabs().size)
        switcherModes.getTabAt(1)?.text = getString(R.string.incognito_count, tabs.incognitoTabs().size)
        val active = tabs.active
        val idx = list.indexOf(active)
        if (idx >= 0) switcherGrid.scrollToPosition(idx)
    }

    // --- Menu & dialogs ----------------------------------------------------------

    private fun showMenu() {
        val tab = tabs.active ?: return
        val wv = tab.webView
        val web = !tab.showingHome && wv != null
        val popup = PopupMenu(this, btnMenu)
        popup.menuInflater.inflate(R.menu.browser_menu, popup.menu)
        val m = popup.menu
        m.findItem(R.id.action_back).isEnabled = (web && wv!!.canGoBack()) || (tab.showingHome && tab.homeDetour)
        m.findItem(R.id.action_forward).isEnabled = web && wv!!.canGoForward()
        m.findItem(R.id.action_reload).isEnabled = web
        m.findItem(R.id.action_reload).setTitle(if (tab.isLoading) R.string.stop else R.string.reload)
        val bookmarked = web && db.isBookmarked(tab.url)
        m.findItem(R.id.action_bookmark).isVisible = web
        m.findItem(R.id.action_bookmark).setTitle(if (bookmarked) R.string.remove_bookmark else R.string.bookmark_page)
        m.findItem(R.id.action_find).isVisible = web
        m.findItem(R.id.action_share).isVisible = web
        m.findItem(R.id.action_desktop).isChecked = tab.desktopMode
        popup.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_back -> backCallback.handleOnBackPressed()
                R.id.action_forward -> wv?.goForward()
                R.id.action_reload -> if (tab.isLoading) wv?.stopLoading() else wv?.reload()
                R.id.action_new_tab -> activateTab(tabs.create(false, afterActive = true))
                R.id.action_new_incognito -> activateTab(tabs.create(true, afterActive = true))
                R.id.action_new_tor_tab -> openTorTab()
                R.id.action_bookmark -> toggleBookmark(tab)
                R.id.action_bookmarks -> startActivity(Intent(this, BookmarksActivity::class.java))
                R.id.action_history -> startActivity(Intent(this, HistoryActivity::class.java))
                R.id.action_downloads -> startActivity(Intent(this, DownloadsActivity::class.java))
                R.id.action_find -> openFind()
                R.id.action_share -> share(tab)
                R.id.action_desktop -> {
                    tab.desktopMode = !tab.desktopMode
                    wv?.let {
                        BrowserWebView.applyUserAgent(this, it.settings, tab.desktopMode)
                        it.reload()
                    }
                }
                R.id.action_settings -> startActivity(Intent(this, SettingsActivity::class.java))
            }
            true
        }
        popup.show()
    }

    private fun toggleBookmark(tab: Tab) {
        if (db.isBookmarked(tab.url)) {
            db.removeBookmark(tab.url)
            Toast.makeText(this, R.string.bookmark_removed, Toast.LENGTH_SHORT).show()
        } else {
            db.addBookmark(tab.url, tab.title.ifBlank { Omnibox.displayUrl(tab.url) })
            Toast.makeText(this, R.string.bookmark_added, Toast.LENGTH_SHORT).show()
        }
    }

    private fun share(tab: Tab) {
        val send = Intent(Intent.ACTION_SEND).setType("text/plain")
            .putExtra(Intent.EXTRA_TEXT, tab.url)
            .putExtra(Intent.EXTRA_SUBJECT, tab.title)
        try {
            startActivity(Intent.createChooser(send, getString(R.string.share)))
        } catch (_: ActivityNotFoundException) {
            val cm = getSystemService(ClipboardManager::class.java)
            cm.setPrimaryClip(ClipData.newPlainText("url", tab.url))
            Toast.makeText(this, R.string.link_copied, Toast.LENGTH_SHORT).show()
        }
    }

    private fun showShields() {
        val tab = tabs.active ?: return
        // Settle the button's press ripple now, so it does not linger behind
        // the dialog's scrim as a grey blob over the toolbar.
        findViewById<View>(R.id.btnShield).apply {
            isPressed = false
            jumpDrawablesToCurrentState()
        }
        val web = !tab.showingHome && Omnibox.isWebUrl(tab.url) && tab.pageHost.isNotEmpty()
        val view = layoutInflater.inflate(R.layout.dialog_shields, null)
        val site = view.findViewById<TextView>(R.id.shieldsSite)
        val state = view.findViewById<TextView>(R.id.shieldsState)
        val toggle = view.findViewById<MaterialSwitch>(R.id.shieldsSwitch)
        val count = view.findViewById<TextView>(R.id.shieldsCount)
        val total = view.findViewById<TextView>(R.id.shieldsTotal)
        val icon = view.findViewById<FaIconView>(R.id.shieldsIcon)
        total.text = getString(R.string.blocked_total, UiUtil.compactCount(adblock.totalBlocked()))
        fun refresh() {
            val up = adblock.shieldsUp(tab.pageHost)
            state.setText(if (up) R.string.shields_up else R.string.shields_down)
            icon.setFa(if (up) FaIcons.SHIELD_HALVED else FaIcons.SHIELD)
            icon.setTextColor(
                ContextCompat.getColor(this, if (up) R.color.chrome_cyan else R.color.aurora_shield_off),
            )
        }
        when {
            !adblock.enabled -> {
                site.text = getString(R.string.shields)
                toggle.isVisible = false
                count.setText(R.string.shields_off_globally)
                state.setText(R.string.shields_down)
            }
            !web -> {
                site.text = getString(R.string.shields)
                toggle.isVisible = false
                count.setText(R.string.shields_not_web)
                state.setText(R.string.shields_up)
            }
            else -> {
                site.text = getString(R.string.shields_for, adblock.siteKey(tab.pageHost))
                toggle.isChecked = adblock.shieldsUp(tab.pageHost)
                count.text = getString(R.string.shields_blocked_page, tab.blockedOnPage.get())
                refresh()
                toggle.setOnCheckedChangeListener { _, checked ->
                    adblock.setShields(tab.pageHost, checked)
                    refresh()
                    updateShield()
                    tab.webView?.reload()
                }
            }
        }
        MaterialAlertDialogBuilder(this)
            .setView(view)
            .setPositiveButton(R.string.close, null)
            .show()
    }

    // --- Tor --------------------------------------------------------------------

    private val torListener: (TorManager.Status) -> Unit = { status ->
        if (!isFinishing && !isDestroyed) {
            updateToolbar()
            if (status.state == TorManager.State.ON) reloadTabsWaitingForTor()
            if (status.state == TorManager.State.OFF) reloadTabsWaitingForTor()
            if (tabs.active?.showingHome == true) home.bind(tabs.active!!.incognito)
        }
    }

    private fun reloadTabsWaitingForTor() {
        for (t in tabs.tabs) {
            if (!t.waitingForTor) continue
            t.waitingForTor = false
            if (!t.showingHome) t.webView?.reload()
        }
    }

    private fun updateTorBadge() {
        val status = TorManager.status
        torBadge.isVisible = TorManager.enabled || status.state == TorManager.State.ERROR
        val color = when (status.state) {
            TorManager.State.ON -> R.color.home_purple
            TorManager.State.ERROR -> R.color.chrome_magenta
            else -> R.color.chrome_muted
        }
        torBadge.imageTintList = ColorStateList.valueOf(ContextCompat.getColor(this, color))
        torBadge.alpha = if (status.state == TorManager.State.STARTING) 0.6f else 1f
    }

    /** Brave-style "New private tab with Tor": Tor mode on + an incognito tab. */
    private fun openTorTab() {
        if (!TorManager.enabled) {
            if (!TorManager.isSupported()) {
                Toast.makeText(this, R.string.tor_unsupported, Toast.LENGTH_LONG).show()
                return
            }
            TorManager.setEnabled(this, true)
        }
        activateTab(tabs.create(true, afterActive = true))
    }

    private fun showTorStatus() {
        val status = TorManager.status
        val message = when (status.state) {
            TorManager.State.OFF -> getString(R.string.tor_off)
            else -> status.summary.ifBlank { getString(R.string.tor_connecting) }
        }
        val builder = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.tor)
            .setMessage(message)
            .setNegativeButton(R.string.close, null)
        if (TorManager.enabled) {
            builder.setPositiveButton(R.string.tor_turn_off) { _, _ -> TorManager.setEnabled(this, false) }
            if (TorManager.ready) {
                builder.setNeutralButton(R.string.pref_tor_new_identity) { _, _ ->
                    TorManager.newIdentity { ok ->
                        Toast.makeText(
                            this,
                            if (ok) R.string.tor_new_identity_done else R.string.tor_new_identity_failed,
                            Toast.LENGTH_SHORT,
                        ).show()
                    }
                }
            }
        } else {
            builder.setPositiveButton(R.string.tor_turn_on) { _, _ -> TorManager.setEnabled(this, true) }
        }
        builder.show()
    }

    // --- Back navigation ---------------------------------------------------------

    private val backCallback = object : OnBackPressedCallback(true) {
        override fun handleOnBackPressed() {
            val tab = tabs.active
            when {
                customView != null -> hideCustomView()
                switcher.isVisible -> hideSwitcher()
                findBar.isVisible -> closeFind()
                omnibox.hasFocus() -> finishEditing()
                tab == null -> finish()
                !tab.showingHome && tab.webView?.canGoBack() == true -> tab.webView?.goBack()
                !tab.showingHome && tab.openerId != null && tabs.find(tab.openerId) != null -> closeTab(tab)
                !tab.showingHome -> showHome(tab, detour = false)
                tab.homeDetour && tab.webView?.url != null -> {
                    tab.homeDetour = false
                    tab.url = tab.webView?.url ?: Omnibox.HOME_URL
                    tab.pageHost = Omnibox.hostOf(tab.url).orEmpty()
                    tab.title = tab.webView?.title.orEmpty()
                    render()
                }
                tab.incognito -> closeTab(tab)
                else -> moveTaskToBack(true)
            }
        }
    }

    // --- Intents -------------------------------------------------------------------

    private fun handleIntent(intent: Intent?) {
        intent ?: return
        when (intent.action) {
            Intent.ACTION_VIEW -> {
                val url = intent.dataString ?: return
                if (Omnibox.isWebUrl(url)) openInNewTab(url, incognito = false)
            }
            ACTION_OPEN_URL -> {
                val url = intent.getStringExtra(EXTRA_URL) ?: return
                val incognito = intent.getBooleanExtra(EXTRA_INCOGNITO, false)
                val active = tabs.active
                val newTab = intent.getBooleanExtra(EXTRA_NEW_TAB, true)
                if (switcher.isVisible) switcher.isVisible = false
                if (active != null && active.incognito == incognito && (active.showingHome || !newTab)) {
                    loadInTab(active, url)
                } else {
                    openInNewTab(url, incognito)
                }
            }
            Intent.ACTION_WEB_SEARCH -> {
                val q = intent.getStringExtra(SearchManager.QUERY) ?: return
                Omnibox.resolve(q, engines.default().template, prefs.httpsOnly)?.let { openInNewTab(it, false) }
            }
            Intent.ACTION_SEND -> {
                val text = intent.getStringExtra(Intent.EXTRA_TEXT) ?: return
                val urlInText = Regex("https?://\\S+").find(text)?.value
                val target = urlInText ?: Omnibox.resolve(text, engines.default().template, prefs.httpsOnly) ?: return
                openInNewTab(target, false)
            }
        }
        setIntent(Intent(this, MainActivity::class.java))
    }
}
