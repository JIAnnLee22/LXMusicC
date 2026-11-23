package jiannlee22.lxmusicc

import android.Manifest
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import android.window.BackEvent
import android.window.OnBackAnimationCallback
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher

/** 仅适配系统返回/配置/通知授权；路由、反馈文案和页面全部在 C。主线程使用。 */
class SystemBridge(private val activity: SourceActivity) {
    private external fun nativeSystem(fontScale: Float, notifications: Int)
    private external fun nativeBack(kind: Int, progress: Float)
    private var callback: OnBackInvokedCallback? = null
    private var registered = false
    private var denied = false

    fun publish() {
        val manager = activity.getSystemService(NotificationManager::class.java)
        nativeSystem(activity.resources.configuration.fontScale,
            if (manager.areNotificationsEnabled()) 1 else if (denied) 2 else 0)
    }
    fun request(action: Int, value: Int) {
        try { when (action) {
            0 -> setBack(value != 0)
            1 -> if (Build.VERSION.SDK_INT >= 33 && activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                activity.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), NOTIFICATIONS)
            } else openSettings()
            2 -> openSettings()
        } } catch (e: Exception) {
            android.util.Log.w("LXMusicC", "System request failed", e)
            nativeSystem(activity.resources.configuration.fontScale, 3)
        }
    }
    private fun openSettings() {
        activity.startActivity(Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
            .putExtra(Settings.EXTRA_APP_PACKAGE, activity.packageName))
    }
    fun permissionResult(request: Int, results: IntArray) {
        if (request == NOTIFICATIONS) {
            denied = results.isEmpty() || results[0] != PackageManager.PERMISSION_GRANTED
            publish()
        }
    }
    @Suppress("DEPRECATION")
    fun legacyBack() { nativeBack(0, 0f) }
    private fun setBack(enabled: Boolean) {
        if (Build.VERSION.SDK_INT < 33 || enabled == registered) return
        if (!enabled) {
            callback?.let { activity.onBackInvokedDispatcher.unregisterOnBackInvokedCallback(it) }
            registered = false
            return
        }
        val cb = if (Build.VERSION.SDK_INT >= 34) object : OnBackAnimationCallback {
            override fun onBackStarted(event: BackEvent) { nativeBack(1, 0f) }
            override fun onBackProgressed(event: BackEvent) { nativeBack(1, event.progress.coerceIn(0f, 1f)) }
            override fun onBackCancelled() { nativeBack(2, 0f) }
            override fun onBackInvoked() { nativeBack(0, 0f) }
        } else OnBackInvokedCallback { nativeBack(0, 0f) }
        callback = cb
        activity.onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, cb)
        registered = true
    }
    fun close() { setBack(false); callback = null }
    companion object { private const val NOTIFICATIONS = 1301 }
}
