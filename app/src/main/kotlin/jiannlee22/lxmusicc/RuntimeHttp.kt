package jiannlee22.lxmusicc

import android.os.SystemClock
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URI
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.Locale
import java.util.zip.GZIPInputStream

/** 系统 HTTP 传输；LX 协议、脚本及歌曲解析全部在 C/JS 工作线程。 */
internal object RuntimeHttp {
    private const val LIMIT = 2 * 1024 * 1024

    fun execute(data: ByteArray, cancelled: () -> Boolean): ByteArray = try {
        if (data.size > LIMIT) throw IOException("HTTP 请求超过 2 MiB")
        val spec = JSONObject(String(data, Charsets.UTF_8))
        var target = URI(spec.getString("url")).toURL()
        var method = spec.optString("method", "GET").uppercase(Locale.ROOT)
        if (method !in listOf("GET", "POST", "PUT", "DELETE", "PATCH", "HEAD")) {
            throw IOException("不支持的 HTTP 方法")
        }
        var body = if (spec.isNull("body")) null else spec.getString("body").toByteArray(Charsets.UTF_8)
        val timeout = spec.optLong("timeout", 15000).coerceIn(100, 30000)
        val deadline = SystemClock.elapsedRealtime() + timeout
        var headers = spec.optJSONObject("headers") ?: JSONObject()
        var result: JSONObject? = null
        fun check() {
            if (cancelled()) throw IOException("音源请求已取消")
            if (SystemClock.elapsedRealtime() >= deadline) throw IOException("音源 HTTP 超时，请重试")
        }
        for (redirect in 0..5) {
            check()
            if (target.protocol !in listOf("http", "https") || target.userInfo != null || target.host.isNullOrEmpty()) {
                throw IOException("仅支持无凭据的 HTTP(S) 地址")
            }
            if (target.toString().length > 8192 || target.toString().any { it.code <= 32 || it.code >= 127 }) {
                throw IOException("HTTP 地址无效，请先百分号编码")
            }
            val connection = target.openConnection() as HttpURLConnection
            try {
                connection.connectTimeout = minOf(2000L, deadline - SystemClock.elapsedRealtime()).coerceAtLeast(1).toInt()
                connection.readTimeout = minOf(1000L, timeout).toInt()
                connection.instanceFollowRedirects = false
                connection.requestMethod = method
                connection.setRequestProperty("User-Agent", "LXMusicC/0.1")
                val keys = headers.keys()
                while (keys.hasNext()) {
                    val key = keys.next()
                    val value = headers.getString(key)
                    if (!key.matches(Regex("[!#$%&'*+.^_`|~0-9A-Za-z-]+")) || value.any { it == '\r' || it == '\n' || it == '\u0000' }) {
                        throw IOException("HTTP 请求头无效")
                    }
                    if (key.lowercase(Locale.ROOT) in listOf("host", "content-length", "connection", "transfer-encoding", "accept-encoding")) continue
                    connection.setRequestProperty(key, value)
                }
                connection.setRequestProperty("Accept-Encoding", "identity")
                if (body != null && method !in listOf("GET", "HEAD")) {
                    connection.doOutput = true
                    connection.setFixedLengthStreamingMode(body.size)
                    check()
                    connection.outputStream.use { output ->
                        var offset = 0
                        while (offset < body.size) {
                            check()
                            val count = minOf(8192, body.size - offset)
                            output.write(body, offset, count)
                            offset += count
                        }
                    }
                }
                check()
                val status = connection.responseCode
                check()
                if (status in listOf(301, 302, 303, 307, 308)) {
                    if (redirect == 5) throw IOException("HTTP 重定向过多")
                    val location = connection.getHeaderField("Location") ?: throw IOException("重定向缺少地址")
                    val next = target.toURI().resolve(location).toURL()
                    if (target.protocol == "https" && next.protocol != "https") throw IOException("拒绝 HTTPS 降级重定向")
                    if (status == 303 || ((status == 301 || status == 302) && method == "POST")) {
                        method = if (method == "HEAD") "HEAD" else "GET"
                        body = null
                    }
                    if (target.protocol != next.protocol || target.host != next.host || target.port != next.port) {
                        if (body != null) throw IOException("拒绝携带正文的跨站重定向")
                        headers = JSONObject() // 不把 Cookie/Authorization/自定义密钥发给另一站点。
                    }
                    target = next
                    continue
                }
                if (connection.contentLengthLong > LIMIT) throw IOException("HTTP 响应超过 2 MiB")
                val output = ByteArrayOutputStream()
                val stream = if (status >= 400) connection.errorStream else connection.inputStream
                if (stream != null) {
                    val decoded = if (connection.contentEncoding.equals("gzip", true)) GZIPInputStream(stream) else stream
                    decoded.use { input ->
                        val buffer = ByteArray(8192)
                        while (true) {
                            check()
                            val n = input.read(buffer)
                            if (n < 0) break
                            if (output.size() + n > LIMIT) throw IOException("HTTP 响应超过 2 MiB")
                            output.write(buffer, 0, n)
                        }
                    }
                }
                check()
                val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(output.toByteArray())).toString()
                val responseHeaders = JSONObject()
                for ((key, values) in connection.headerFields) {
                    if (key != null) responseHeaders.put(key.lowercase(Locale.ROOT), values.joinToString(", "))
                }
                result = JSONObject().put("statusCode", status).put("headers", responseHeaders).put("body", text)
                break
            } finally {
                connection.disconnect()
            }
        }
        (result ?: throw IOException("HTTP 请求失败")).toString().toByteArray(Charsets.UTF_8)
    } catch (e: Exception) {
        val message = when (e) {
            is java.net.UnknownHostException -> "无法连接音源服务器，请检查网络后重试"
            is java.net.SocketTimeoutException -> "音源 HTTP 超时，请重试"
            is javax.net.ssl.SSLException -> "HTTPS 证书或连接失败"
            is java.nio.charset.CharacterCodingException -> "暂不支持非 UTF-8 HTTP 响应"
            else -> if (e is IOException && e.message?.any { it.code > 127 } == true) e.message!! else "HTTP 请求失败，请检查音源与网络后重试"
        }
        JSONObject().put("error", message).toString().toByteArray(Charsets.UTF_8)
    }
}
