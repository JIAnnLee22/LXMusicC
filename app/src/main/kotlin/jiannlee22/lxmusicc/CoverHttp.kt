package jiannlee22.lxmusicc

import android.os.SystemClock
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URI

/**
 * 封面下载：只封装系统 HttpURLConnection，不做业务判断、不落盘、不解码图片。
 * 输入是 C 侧规则化后的 ASCII 百分号编码地址（1..2047 字节）。
 * 输出协议与其它桥一致：首字节 1 + 原始 JPEG/PNG 字节（成功），或 0 + UTF-8 可读错误（失败）。
 */
internal object CoverHttp {
    private const val LIMIT = 2 * 1024 * 1024
    private const val URL_BYTES = 2047
    private const val REDIRECTS = 5
    private const val TOTAL_MS = 20_000L

    fun execute(data: ByteArray, cancelled: () -> Boolean): ByteArray = try {
        if (data.isEmpty() || data.size > URL_BYTES) throw IOException("封面链接为空或超过 2047 字节")
        if (data.any { (it.toInt() and 0xff) <= 32 || (it.toInt() and 0xff) >= 127 }) {
            throw IOException("封面链接必须是百分号编码的 ASCII 地址")
        }
        var target = URI(String(data, Charsets.US_ASCII)).toURL()
        val deadline = SystemClock.elapsedRealtime() + TOTAL_MS
        var body: ByteArray? = null
        fun check() {
            if (cancelled()) throw IOException("封面下载已取消")
            if (SystemClock.elapsedRealtime() >= deadline) throw IOException("封面下载超时，请重试")
        }
        for (redirect in 0..REDIRECTS) {
            check()
            if (target.protocol != "https" && target.protocol != "http") throw IOException("封面仅支持 HTTP(S) 链接")
            if (target.userInfo != null || target.host.isNullOrEmpty()) throw IOException("封面链接无效")
            val connection = target.openConnection() as HttpURLConnection
            try {
                // 销毁时只置取消位，不在主线程关闭 socket；短超时尽快响应取消。
                connection.connectTimeout = 2_000
                connection.readTimeout = 1_000
                connection.instanceFollowRedirects = false
                connection.setRequestProperty("User-Agent", "LXMusicC/0.1")
                connection.setRequestProperty("Accept", "image/jpeg, image/png")
                connection.setRequestProperty("Accept-Encoding", "identity")
                check()
                val status = connection.responseCode
                check()
                if (status in listOf(301, 302, 303, 307, 308)) {
                    if (redirect == REDIRECTS) throw IOException("封面链接重定向过多")
                    val location = connection.getHeaderField("Location") ?: throw IOException("封面重定向缺少地址")
                    val next = target.toURI().resolve(location).toURL()
                    if (target.protocol == "https" && next.protocol != "https") {
                        throw IOException("拒绝 HTTPS 降级重定向")
                    }
                    target = next
                    continue
                }
                // 封面不做错误页猜测：只有 200 才是图片，其余状态一律失败。
                if (status != HttpURLConnection.HTTP_OK) throw IOException("封面下载失败：HTTP $status")
                if (connection.contentLengthLong > LIMIT) throw IOException("封面超过 2 MiB")
                val output = ByteArrayOutputStream()
                connection.inputStream.use { input ->
                    val buffer = ByteArray(8192)
                    while (true) {
                        check()
                        val n = input.read(buffer)
                        if (n < 0) break
                        if (output.size() + n > LIMIT) throw IOException("封面超过 2 MiB")
                        output.write(buffer, 0, n)
                    }
                }
                check()
                if (output.size() == 0) throw IOException("封面内容为空")
                body = output.toByteArray()
                break
            } finally {
                connection.disconnect()
            }
        }
        byteArrayOf(1) + (body ?: throw IOException("封面下载失败，请重试"))
    } catch (e: Exception) {
        val message = when (e) {
            is java.net.UnknownHostException -> "无法连接封面服务器，请检查网络后重试"
            is java.net.SocketTimeoutException -> "封面下载超时，请重试"
            is javax.net.ssl.SSLException -> "封面 HTTPS 证书或连接失败"
            else -> if (e is IOException && e.message?.any { it.code > 127 } == true) {
                e.message!!
            } else "封面下载失败，请检查链接和网络后重试"
        }
        byteArrayOf(0) + message.toByteArray(Charsets.UTF_8)
    }
}
