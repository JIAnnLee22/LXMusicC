package jiannlee22.lxmusicc

import android.app.NativeActivity
import android.content.ClipboardManager
import android.os.SystemClock
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URI

/** 仅系统剪贴板和 HTTP；没有 View、解析或音源业务状态。 */
class SourceActivity : NativeActivity() {
    @Volatile private var closing = false
    private external fun nativePaste(data: ByteArray?, error: String?, search: Boolean)
    private external fun nativeSearchCancelled(token: Long): Boolean
    private external fun nativeCoverCancelled(token: Long): Boolean

    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        volumeControlStream = android.media.AudioManager.STREAM_MUSIC
    }

    /** 二进制传输仅在 C 封面线程执行；解码和缓存策略留在 C。 */
    fun requestCover(data: ByteArray, token: Long): ByteArray =
        CoverHttp.execute(data) { closing || nativeCoverCancelled(token) }

    override fun onDestroy() {
        closing = true
        super.onDestroy()
    }

    fun cancelSourceFetch() { closing = true }

    fun requestPlaybackStart(): Boolean {
        if (closing) return false
        runOnUiThread {
            if (closing || isFinishing || isDestroyed) { PlaybackStart.failed(); return@runOnUiThread }
            try {
                // 用户前台选曲时启动，Media3 在准备/播放时负责提升为前台服务。
                startService(android.content.Intent(this, PlaybackService::class.java))
            } catch (_: Exception) { PlaybackStart.failed() }
        }
        return true
    }

    /** C 工作线程调用；标准 UTF-8 JSON 字节，不使用 JNI modified UTF-8。 */
    fun requestRuntime(data: ByteArray): ByteArray = RuntimeHttp.execute(data) { closing }

    /**
     * 公共目录缓存桥：C 工作线程同步调用，IO 全部在本线程完成。
     * 首字节 1 + UTF-8 正文（成功）或 0 + UTF-8 中文错误；cancelled 复用 closing。
     */
    fun requestCache(action: Int, url: ByteArray?, folder: ByteArray?, filename: ByteArray?,
                     uri: ByteArray?, maxBytes: Long): ByteArray =
        CacheStorage.execute(this, action, url, folder, filename, uri, maxBytes) { closing }

    fun requestRuntimeSearch(data: ByteArray, token: Long): ByteArray =
        RuntimeHttp.execute(data) { closing || nativeSearchCancelled(token) }

    fun requestSourcePaste(): Boolean = requestPaste(false)
    fun requestSearchPaste(): Boolean = requestPaste(true)

    private fun requestPaste(search: Boolean): Boolean {
        if (closing) return false
        runOnUiThread {
            if (closing || isFinishing || isDestroyed) return@runOnUiThread
            try {
                val clipboard = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
                val clip = clipboard.primaryClip
                // 不调用 coerceToText：它可能在主线程读取 ContentProvider 文件。
                val text = if (clip != null && clip.itemCount > 0) clip.getItemAt(0).text else null
                if (text == null || text.isEmpty()) {
                    nativePaste(null, "剪贴板没有文本，请先复制内容", search)
                } else if (text.length > if (search) 255 else LIMIT) {
                    nativePaste(null, if (search) "关键词超过 255 字节" else "剪贴板内容超过 2 MiB", search)
                } else {
                    val bytes = text.toString().toByteArray(Charsets.UTF_8)
                    if (bytes.size > if (search) 255 else LIMIT)
                        nativePaste(null, if (search) "关键词超过 255 字节" else "剪贴板内容超过 2 MiB", search)
                    else nativePaste(bytes, null, search)
                }
            } catch (_: Exception) {
                nativePaste(null, "读取剪贴板失败，请回到应用前台后重试", search)
            }
        }
        return true
    }

    /** 由 C 工作线程同步调用；首字节 1=原始 UTF-8 正文，0=UTF-8 错误。 */
    fun fetchSource(url: String): ByteArray {
        return try {
            var target = URI(url).toURL()
            val deadline = SystemClock.elapsedRealtime() + 30_000
            var response: ByteArray? = null
            for (redirect in 0..5) {
                if (closing) throw IOException("下载已取消")
                if (target.protocol != "https" && target.protocol != "http") throw IOException("仅支持 HTTP(S) 链接")
                if (target.userInfo != null || target.host.isNullOrEmpty()) throw IOException("音源链接无效")
                val connection = target.openConnection() as HttpURLConnection
                try {
                    // 销毁时只置取消位，不在主线程关闭 socket；短超时尽快响应取消。
                    // Android 系统 DNS 解析不保证遵守 socket 的超时。
                    connection.connectTimeout = 2_000
                    connection.readTimeout = 1_000
                    connection.instanceFollowRedirects = false
                    connection.setRequestProperty("User-Agent", "LXMusicC/0.1")
                    connection.setRequestProperty("Accept-Encoding", "identity")
                    val status = connection.responseCode
                    if (closing) throw IOException("下载已取消")
                    if (status in listOf(301, 302, 303, 307, 308)) {
                        if (redirect == 5) throw IOException("链接重定向过多")
                        val location = connection.getHeaderField("Location") ?: throw IOException("重定向缺少地址")
                        val next = target.toURI().resolve(location).toURL()
                        if (target.protocol == "https" && next.protocol != "https") throw IOException("拒绝 HTTPS 降级重定向")
                        target = next
                    } else {
                        if (status !in 200..299) throw IOException("下载失败：HTTP $status")
                        if (connection.contentLengthLong > LIMIT) throw IOException("音源内容超过 2 MiB")
                        val output = ByteArrayOutputStream()
                        output.write(1)
                        connection.inputStream.use { input ->
                            val buffer = ByteArray(8192)
                            while (true) {
                                if (closing) throw IOException("下载已取消")
                                if (SystemClock.elapsedRealtime() > deadline) throw IOException("下载超时，请重试")
                                val n = input.read(buffer)
                                if (n < 0) break
                                if (output.size() - 1 + n > LIMIT) throw IOException("音源内容超过 2 MiB")
                                output.write(buffer, 0, n)
                            }
                        }
                        response = output.toByteArray()
                        break
                    }
                } finally {
                    connection.disconnect()
                }
                if (SystemClock.elapsedRealtime() > deadline) throw IOException("下载超时，请重试")
            }
            response ?: throw IOException("下载失败，请重试")
        } catch (e: Exception) {
            val message = when (e) {
                is java.net.UnknownHostException -> "无法连接音源服务器，请检查网络后重试"
                is java.net.SocketTimeoutException -> "下载超时，请重试"
                is javax.net.ssl.SSLException -> "HTTPS 证书或连接失败"
                else -> if (e is IOException && e.message?.any { it.code > 127 } == true) e.message!! else "下载失败，请检查链接和网络后重试"
            }
            byteArrayOf(0) + message.toByteArray(Charsets.UTF_8)
        }
    }

    private companion object {
        const val LIMIT = 2 * 1024 * 1024
        // NativeActivity 的 dlopen 不会登记 JVM native 方法解析的 ClassLoader 库表。
        init { System.loadLibrary("lxmusicc") }
    }
}
