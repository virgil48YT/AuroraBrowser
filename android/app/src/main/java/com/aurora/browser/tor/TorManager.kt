package com.aurora.browser.tor

import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import androidx.webkit.ProxyConfig
import androidx.webkit.ProxyController
import androidx.webkit.WebViewFeature
import com.aurora.browser.R
import com.aurora.browser.data.Prefs
import org.torproject.jni.TorService
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.Executor
import java.util.concurrent.Executors

/**
 * Built-in Tor, in the spirit of Brave's "Private window with Tor": the tor
 * daemon ships inside the APK (Guardian Project's tor-android, libtor.so for
 * every ABI) and runs in-process as [TorService] — no Orbot or other setup.
 *
 * While Tor mode is on, every WebView request in the app goes through tor's
 * SOCKS5 port via androidx.webkit [ProxyController] (a process-wide proxy
 * override; WebView cannot proxy per tab). SOCKS5 hands host names to tor,
 * so DNS is resolved by the exit relay and .onion addresses work.
 *
 * Fail-closed: the moment Tor mode is switched on, WebView is pointed at a
 * dead local proxy and [BrowserWebView] refuses every request until tor has
 * bootstrapped and the real proxy override has been confirmed. Nothing is
 * ever sent around Tor while it is "connecting".
 */
object TorManager {
    private const val TAG = "AuroraTor"

    /** Nothing listens on the discard port: requests fail instead of leaking. */
    private const val BLACKHOLE_PROXY = "socks5://127.0.0.1:9"
    private const val POLL_MS = 750L

    enum class State { OFF, STARTING, ON, ERROR }

    data class Status(val state: State, val progress: Int = 0, val summary: String = "")

    private val main = Handler(Looper.getMainLooper())
    private val mainExecutor = Executor { main.post(it) }
    private val worker = Executors.newSingleThreadExecutor()
    private val listeners = CopyOnWriteArraySet<(Status) -> Unit>()

    @Volatile var status: Status = Status(State.OFF)
        private set

    /** Tor mode requested (pref on). Read from WebView network threads. */
    @Volatile var enabled: Boolean = false
        private set

    /** True once traffic is actually flowing through tor. */
    val ready: Boolean get() = enabled && status.state == State.ON

    /** Local SOCKS5 port of the running tor, or -1. */
    @Volatile var socksPort: Int = -1
        private set

    private var appContext: Context? = null
    @Volatile private var service: TorService? = null
    private var bound = false
    private var receiverRegistered = false

    /** WebView must support process-wide proxy overrides (WebView 74+). */
    fun isSupported(): Boolean = try {
        WebViewFeature.isFeatureSupported(WebViewFeature.PROXY_OVERRIDE)
    } catch (t: Throwable) {
        false
    }

    /** Called from Application.onCreate: resume Tor mode if it was left on. */
    fun init(context: Context) {
        appContext = context.applicationContext
        if (Prefs(context).torEnabled) start(context)
    }

    fun addListener(l: (Status) -> Unit) { listeners.add(l) }
    fun removeListener(l: (Status) -> Unit) { listeners.remove(l) }

    /** Persists the choice and starts/stops tor accordingly. */
    fun setEnabled(context: Context, on: Boolean) {
        Prefs(context).torEnabled = on
        if (on) start(context) else stop(context)
    }

    fun start(context: Context) {
        val app = context.applicationContext
        appContext = app
        if (enabled) return
        if (!isSupported()) {
            Prefs(app).torEnabled = false
            publish(Status(State.ERROR, summary = app.getString(R.string.tor_unsupported)))
            return
        }
        enabled = true
        publish(Status(State.STARTING, 0, app.getString(R.string.tor_connecting)))
        // Fail closed before tor is even up.
        applyProxy(BLACKHOLE_PROXY) {}
        if (!receiverRegistered) {
            val filter = IntentFilter(TorService.ACTION_STATUS).apply { addAction(TorService.ACTION_ERROR) }
            LocalBroadcastManager.getInstance(app).registerReceiver(receiver, filter)
            receiverRegistered = true
        }
        bound = try {
            app.bindService(Intent(app, TorService::class.java), connection, Context.BIND_AUTO_CREATE)
        } catch (e: Exception) {
            Log.w(TAG, "bindService failed", e)
            false
        }
        if (!bound) {
            publish(Status(State.ERROR, summary = app.getString(R.string.tor_failed)))
            return
        }
        main.removeCallbacks(poll)
        main.postDelayed(poll, POLL_MS)
    }

    fun stop(context: Context) {
        val app = context.applicationContext
        if (!enabled && status.state == State.OFF) return
        enabled = false
        main.removeCallbacks(poll)
        if (receiverRegistered) {
            LocalBroadcastManager.getInstance(app).unregisterReceiver(receiver)
            receiverRegistered = false
        }
        if (bound) {
            // Unbinding the last client destroys TorService, which shuts tor down.
            try { app.unbindService(connection) } catch (_: Exception) {}
            bound = false
        }
        service = null
        socksPort = -1
        if (isSupported()) {
            try {
                ProxyController.getInstance().clearProxyOverride(mainExecutor) { publish(Status(State.OFF)) }
            } catch (e: Exception) {
                Log.w(TAG, "clearProxyOverride failed", e)
                publish(Status(State.OFF))
            }
        } else {
            publish(Status(State.OFF))
        }
    }

    /** Ask tor for fresh circuits (Brave/Tor Browser "New identity" for the circuit). */
    fun newIdentity(onDone: (Boolean) -> Unit) {
        val svc = service
        if (svc == null || !ready) {
            onDone(false)
            return
        }
        worker.execute {
            val ok = try {
                svc.torControlConnection?.signal("NEWNYM")
                true
            } catch (t: Throwable) {
                Log.w(TAG, "NEWNYM failed", t)
                false
            }
            main.post { onDone(ok) }
        }
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service = (binder as? TorService.LocalBinder)?.service
            main.post(poll)
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
            if (enabled) {
                // Process of tor died; stay fail-closed until it is back.
                applyProxy(BLACKHOLE_PROXY) {}
                appContext?.let { publish(Status(State.STARTING, 0, it.getString(R.string.tor_reconnecting))) }
            }
        }
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                TorService.ACTION_ERROR -> {
                    val msg = intent.getStringExtra(Intent.EXTRA_TEXT).orEmpty()
                    Log.w(TAG, "tor error: $msg")
                    if (enabled && status.state != State.ON) {
                        publish(Status(State.ERROR, status.progress, context.getString(R.string.tor_failed)))
                    }
                }
                TorService.ACTION_STATUS -> main.post(poll)
            }
        }
    }

    /** Polls bootstrap progress off the main thread until tor is usable. */
    private val poll: Runnable = object : Runnable {
        override fun run() {
            if (!enabled || status.state == State.ON) return
            val svc = service
            if (svc == null) {
                main.postDelayed(this, POLL_MS)
                return
            }
            worker.execute {
                val phase = try { svc.getInfo("status/bootstrap-phase") } catch (t: Throwable) { null }
                val progress = phase?.let { Regex("PROGRESS=(\\d+)").find(it)?.groupValues?.get(1)?.toIntOrNull() } ?: 0
                val summary = phase?.let { Regex("SUMMARY=\"([^\"]*)\"").find(it)?.groupValues?.get(1) }.orEmpty()
                val port = try { svc.socksPort } catch (t: Throwable) { -1 }
                main.post {
                    if (!enabled || status.state == State.ON) return@post
                    if (progress >= 100 && port > 0) {
                        goLive(port)
                    } else {
                        val label = appContext?.getString(R.string.tor_connecting_progress, progress, summary) ?: summary
                        publish(Status(State.STARTING, progress, label))
                        main.postDelayed(this, POLL_MS)
                    }
                }
            }
        }
    }

    private fun goLive(port: Int) {
        applyProxy("socks5://127.0.0.1:$port") {
            if (!enabled) return@applyProxy
            socksPort = port
            Log.i(TAG, "Tor ready on SOCKS port $port")
            publish(Status(State.ON, 100, appContext?.getString(R.string.tor_connected).orEmpty()))
        }
    }

    private fun applyProxy(rule: String, onApplied: () -> Unit) {
        fun build(r: String) = ProxyConfig.Builder().addProxyRule(r).build()
        try {
            ProxyController.getInstance().setProxyOverride(build(rule), mainExecutor, onApplied)
        } catch (e: IllegalArgumentException) {
            // Older WebView builds only spell SOCKS5 as "socks://".
            ProxyController.getInstance().setProxyOverride(build(rule.replace("socks5://", "socks://")), mainExecutor, onApplied)
        } catch (e: Exception) {
            Log.w(TAG, "setProxyOverride failed", e)
            appContext?.let { publish(Status(State.ERROR, summary = it.getString(R.string.tor_failed))) }
        }
    }

    private fun publish(s: Status) {
        status = s
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listeners.forEach { it(s) }
        } else {
            main.post { listeners.forEach { it(s) } }
        }
    }
}
