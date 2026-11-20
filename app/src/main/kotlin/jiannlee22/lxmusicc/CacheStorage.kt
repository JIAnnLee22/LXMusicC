package jiannlee22.lxmusicc

import android.Manifest
import android.content.ContentResolver
import android.content.ContentValues
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.SystemClock
import android.provider.MediaStore
import android.webkit.MimeTypeMap
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URI
import java.net.URL
import java.net.UnknownHostException
import java.util.Locale
import javax.net.ssl.SSLException

/**
 * 公共目录缓存桥：C 工作线程同步调用，网络与磁盘 IO 全在该线程完成，
 * 只有存储权限请求投递到主线程（不等待用户操作）。返回首字节 1=成功，0=UTF-8 中文错误。
 *
 * 缓存地址一律是 content:// URI（legacy 也经 MediaStore 登记），播放侧不接受 file://。
 * 错误文案区分「缓存文件不存在」（丢失，可重下）与「暂时无法访问 / 没有权限」（暂不可用，保留索引）。
 */
internal object CacheStorage {
    private const val CHUNK = 32 * 1024
    private const val CONNECT_TIMEOUT_MS = 15_000
    private const val READ_TIMEOUT_MS = 10_000
    private const val MAX_REDIRECTS = 5
    private const val ROOT = "LXMusicC"
    private const val PERMISSION_CODE = 0x2106
    private const val PERMISSION_COOLDOWN_MS = 10_000L
    private const val STALE_PART_MS = 10 * 60_000L
    private const val HARD_LIMIT = 2L shl 30
    private const val MISSING = "缓存文件不存在"
    private const val DENIED = "没有权限访问缓存文件，请授权后重试"
    private const val UNAVAILABLE = "缓存文件暂时无法访问，请稍后重试"

    @Volatile private var permissionAskedAt = 0L

    fun execute(activity: SourceActivity, action: Int, url: ByteArray?, folder: ByteArray?,
                filename: ByteArray?, uri: ByteArray?, maxBytes: Long,
                cancelled: () -> Boolean): ByteArray = try {
        when (action) {
            1 -> download(activity, text(url), text(folder), text(filename), maxBytes, cancelled)
            2 -> probe(activity, text(uri))
            3 -> remove(activity, text(uri))
            4 -> cleanup(activity)
            else -> fail("不支持的缓存操作")
        }
    } catch (e: Exception) {
        fail(reason(e))
    }

    private fun reason(e: Exception): String {
        val message = e.message
        if (message != null && message.any { it.code > 127 }) return message
        return when (e) {
            is UnknownHostException -> "无法连接服务器，请检查网络后重试"
            is SocketTimeoutException -> "下载超时，请重试"
            is SSLException -> "HTTPS 连接失败，请检查网络后重试"
            else -> "缓存操作失败，请重试"
        }
    }

    // ---- action 1：下载到 Music/LXMusicC/<folder>/<filename> ----

    private fun download(activity: SourceActivity, url: String, folder: String, filename: String,
                         maxBytes: Long, cancelled: () -> Boolean): ByteArray {
        if (cancelled()) abort("下载已取消")
        if (url.isEmpty()) abort("下载地址为空")
        if (filename.isEmpty()) abort("缓存文件名无效")
        checkSegment(filename)
        if (folder.isNotEmpty()) checkSegment(folder)
        if (maxBytes <= 0) abort("缓存上限无效")
        val limit = minOf(maxBytes, HARD_LIMIT)
        val mime = mimeOf(filename)
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            downloadScoped(activity, url, folder, filename, mime, limit, cancelled)
        else
            downloadLegacy(activity, url, folder, filename, mime, limit, cancelled)
    }

    /** API29+：先建 IS_PENDING 条目再写入，成功后发布；任何失败都删掉 pending 半成品。 */
    private fun downloadScoped(activity: SourceActivity, url: String, folder: String, filename: String,
                               mime: String, limit: Long, cancelled: () -> Boolean): ByteArray {
        val resolver = activity.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, filename)
            put(MediaStore.MediaColumns.MIME_TYPE, mime)
            put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath(folder))
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = runCatching { resolver.insert(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, values) }.getOrNull()
            ?: abort("无法在 Music 目录创建缓存文件")
        try {
            val written = resolver.openOutputStream(uri, "w")?.use { output ->
                fetch(url, limit, cancelled) { input -> copy(input, output, limit, cancelled) }
            } ?: abort("无法写入 Music 缓存文件")
            val done = ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }
            if (resolver.update(uri, done, null, null) <= 0) abort("缓存文件登记失败，请重试")
            return ok("$written\n$uri")
        } catch (e: Exception) {
            try { resolver.delete(uri, null, null) } catch (_: Exception) { }
            throw e
        }
    }

    /** API26-28：按需请求写外部存储权限，先写 .part 再改名，最后登记到 MediaStore。 */
    private fun downloadLegacy(activity: SourceActivity, url: String, folder: String, filename: String,
                               mime: String, limit: Long, cancelled: () -> Boolean): ByteArray {
        if (!hasStoragePermission(activity)) {
            requestStoragePermission(activity)
            abort("需要存储权限才能保存到 Music，请在弹出的提示中授权后重试")
        }
        val music = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MUSIC)
        val dir = File(music, if (folder.isEmpty()) ROOT else "$ROOT/$folder")
        if (!dir.isDirectory && !dir.mkdirs() && !dir.isDirectory) abort("无法创建缓存目录")
        val target = uniqueFile(dir, filename)
        val part = File(dir, "${target.name}.part")
        val written = try {
            part.outputStream().use { output ->
                fetch(url, limit, cancelled) { input -> copy(input, output, limit, cancelled) }
            }
        } catch (e: Exception) {
            part.delete()
            throw e
        }
        if (!part.renameTo(target)) {
            part.delete()
            abort("缓存文件写入失败，请重试")
        }
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DATA, target.absolutePath)
            put(MediaStore.MediaColumns.DISPLAY_NAME, target.name)
            put(MediaStore.MediaColumns.TITLE, target.nameWithoutExtension)
            put(MediaStore.MediaColumns.MIME_TYPE, mime)
            put(MediaStore.MediaColumns.SIZE, written)
            put(MediaStore.MediaColumns.DATE_MODIFIED, System.currentTimeMillis() / 1000)
        }
        val uri = runCatching {
            activity.contentResolver.insert(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, values)
        }.getOrNull() ?: run {
            // legacy 失败清理对应真实文件，不能留下没索引的孤儿文件。
            target.delete()
            abort("无法登记到系统媒体库，请重试")
        }
        return ok("$written\n$uri")
    }

    /** 同名文件已存在时附数字，避免覆盖用户已有的同名文件。 */
    private fun uniqueFile(dir: File, name: String): File {
        if (!File(dir, name).exists()) return File(dir, name)
        val base = name.substringBeforeLast('.', name)
        val ext = if (name.contains('.')) "." + name.substringAfterLast('.') else ""
        for (index in 1..999) {
            val candidate = File(dir, "$base ($index)$ext")
            if (!candidate.exists()) return candidate
        }
        abort("同名缓存文件过多，无法保存")
    }

    // ---- HTTP：流式下载，校验状态码、重定向、类型与长度 ----

    private fun fetch(url: String, limit: Long, cancelled: () -> Boolean,
                      consume: (InputStream) -> Long): Long {
        var target = runCatching { URI(url).toURL() }.getOrElse { abort("下载地址无效") }
        for (hop in 0..MAX_REDIRECTS) {
            if (cancelled()) abort("下载已取消")
            val scheme = target.protocol.lowercase(Locale.ROOT)
            if (scheme != "http" && scheme != "https") abort("仅支持 HTTP(S) 下载")
            if (target.userInfo != null) abort("下载地址不能包含账号凭据")
            if (target.host.isNullOrEmpty()) abort("下载地址无效")
            val connection = runCatching { target.openConnection() as HttpURLConnection }
                .getOrElse { abort("下载地址无效") }
            var redirect: URL? = null
            try {
                connection.connectTimeout = CONNECT_TIMEOUT_MS
                connection.readTimeout = READ_TIMEOUT_MS
                connection.instanceFollowRedirects = false
                connection.requestMethod = "GET"
                connection.setRequestProperty("User-Agent", "LXMusicC/0.1")
                connection.setRequestProperty("Accept-Encoding", "identity")
                val status = connection.responseCode
                if (cancelled()) abort("下载已取消")
                if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                    if (hop == MAX_REDIRECTS) abort("重定向次数过多")
                    val location = connection.getHeaderField("Location") ?: abort("重定向缺少地址")
                    val next = runCatching { target.toURI().resolve(location).toURL() }
                        .getOrElse { abort("重定向地址无效") }
                    if (scheme == "https" && next.protocol.lowercase(Locale.ROOT) != "https") {
                        abort("拒绝 HTTPS 降级重定向")
                    }
                    redirect = next
                } else {
                    if (status != HttpURLConnection.HTTP_OK) abort("下载失败：HTTP $status")
                    val type = connection.contentType?.lowercase(Locale.ROOT) ?: ""
                    if (type.contains("text/html") || type.contains("text/xml") ||
                        type.contains("application/xhtml") || type.contains("application/json")) {
                        abort("服务器返回的不是音频文件")
                    }
                    val declared = connection.contentLengthLong
                    if (declared > limit) abort("文件超过缓存上限")
                    val written = connection.inputStream.use { input -> consume(input) }
                    if (cancelled()) abort("下载已取消")
                    if (written <= 0) abort("下载内容为空")
                    if (declared >= 0 && written != declared) abort("下载不完整，请重试")
                    return written
                }
            } catch (e: SocketTimeoutException) {
                if (cancelled()) abort("下载已取消")
                throw e
            } finally {
                connection.disconnect()
            }
            target = redirect ?: abort("重定向次数过多")
        }
        abort("重定向次数过多")
    }

    private fun copy(input: InputStream, output: OutputStream, limit: Long,
                     cancelled: () -> Boolean): Long {
        val buffer = ByteArray(CHUNK)
        var total = 0L
        val deadline = SystemClock.elapsedRealtime() + 10 * 60_000L
        var first = true
        while (true) {
            if (cancelled()) abort("下载已取消")
            if (SystemClock.elapsedRealtime() > deadline) abort("下载超时，请重试")
            val count = input.read(buffer)
            if (count < 0) break
            if (count == 0) continue
            if (first) {
                first = false
                if (looksLikeMarkup(buffer, count)) abort("服务器返回网页而非音频")
            }
            total += count
            if (total > limit) abort("文件超过缓存上限")
            output.write(buffer, 0, count)
        }
        output.flush()
        return total
    }

    /** 音源常把错误页当音频返回；按首块内容兜底识别 HTML/XML/JSON。 */
    private fun looksLikeMarkup(buffer: ByteArray, length: Int): Boolean {
        val head = String(buffer, 0, minOf(length, 64), Charsets.ISO_8859_1)
            .trimStart().lowercase(Locale.ROOT)
        return head.startsWith("<!doctype") || head.startsWith("<html") ||
            head.startsWith("<?xml") || head.startsWith("{")
    }

    // ---- action 2：探测缓存 URI ----

    private fun probe(activity: SourceActivity, text: String): ByteArray {
        if (text.isEmpty()) abort("缓存地址无效")
        val uri = Uri.parse(text)
        if (uri.scheme?.lowercase(Locale.ROOT) != "content") abort("缓存地址无效")
        val resolver = activity.contentResolver
        // 始终打开文件：MediaStore 的 SIZE 可能在文件已被外部删除时仍保留旧值。
        val actual = try {
            resolver.openFileDescriptor(uri, "r")?.use { descriptor -> descriptor.statSize } ?: 0L
        } catch (_: FileNotFoundException) {
            abort(MISSING)
        } catch (_: SecurityException) {
            abort(DENIED)
        } catch (_: Exception) {
            abort(UNAVAILABLE)
        }
        if (actual <= 0) abort(MISSING)
        return ok("$actual\n$uri")
    }

    // ---- action 3：删除缓存（不存在也算成功） ----

    private fun remove(activity: SourceActivity, text: String): ByteArray {
        if (text.isEmpty()) abort("缓存地址无效")
        val uri = Uri.parse(text)
        if (uri.scheme?.lowercase(Locale.ROOT) != "content") abort("缓存地址无效")
        val resolver = activity.contentResolver
        // legacy 设备删行不一定连带删文件，先记下真实路径再兜底删除。
        val realPath = if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q && hasStoragePermission(activity))
            dataPath(resolver, uri) else null
        try {
            resolver.delete(uri, null, null)
        } catch (_: SecurityException) {
            abort("没有权限删除缓存文件，请在系统文件管理器中删除")
        } catch (_: Exception) {
            abort("缓存文件暂时无法删除，请稍后重试")
        }
        if (realPath != null && File(realPath).exists() && !File(realPath).delete())
            abort("缓存文件暂时无法删除，请稍后重试")
        return ok("已删除")
    }

    private fun dataPath(resolver: ContentResolver, uri: Uri): String? = try {
        resolver.query(uri, arrayOf(MediaStore.MediaColumns.DATA), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getString(0) else null
        }
    } catch (_: Exception) {
        null
    }

    // ---- action 4：清理本应用 LXMusicC 下遗留的 pending 条目与半成品 ----

    private fun cleanup(activity: SourceActivity): ByteArray {
        var removed = 0
        // MediaStore 只暴露本应用名下的 pending 条目，正好是要清的目标。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val path = MediaStore.MediaColumns.RELATIVE_PATH
            val selection = "${MediaStore.MediaColumns.IS_PENDING}=1 AND " +
                "${MediaStore.MediaColumns.OWNER_PACKAGE_NAME}=? AND ($path LIKE ? OR $path=?)"
            try {
                removed += activity.contentResolver.delete(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI,
                    selection, arrayOf(activity.packageName, "${relativePath("")}/%", "${relativePath("")}/"))
            } catch (_: Exception) { }
        }
        // 兜底：只清理超过 10 分钟的 .part，避免误删正在进行的下载。
        try {
            val root = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MUSIC), ROOT)
            val permitted = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q || hasStoragePermission(activity)
            val deadline = System.currentTimeMillis() - STALE_PART_MS
            if (permitted && root.isDirectory) {
                for (file in root.walkTopDown()) {
                    if (!file.isFile || !file.name.endsWith(".part")) continue
                    if (file.lastModified() < deadline && file.delete()) removed++
                }
            }
        } catch (_: Exception) { }
        return ok("清理 $removed 项")
    }

    // ---- 权限与路径工具 ----

    private fun hasStoragePermission(activity: SourceActivity): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q ||
            activity.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED

    /** 只投递请求，不阻塞等待；用户授权后由 C 侧重试本次操作。 */
    private fun requestStoragePermission(activity: SourceActivity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) return
        val now = SystemClock.elapsedRealtime()
        val last = permissionAskedAt
        if (last != 0L && now - last < PERMISSION_COOLDOWN_MS) return
        permissionAskedAt = now
        try {
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
                try {
                    activity.requestPermissions(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE), PERMISSION_CODE)
                } catch (_: Exception) { }
            }
        } catch (_: Exception) { }
    }

    private fun relativePath(folder: String): String =
        "${Environment.DIRECTORY_MUSIC}/$ROOT" + if (folder.isEmpty()) "" else "/$folder"

    private fun mimeOf(filename: String): String {
        val ext = filename.substringAfterLast('.', "").lowercase(Locale.ROOT)
        val mapped = if (ext.isEmpty() || ext.length > 8) null
            else MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext)
        if (mapped != null && mapped.startsWith("audio/")) return mapped
        return if (ext == "m4a" || ext == "mp4" || ext == "aac") "audio/mp4" else "audio/mpeg"
    }

    /** folder/filename 已由 C 清理；这里只做纵深防御，防止越界路径与非法字符。 */
    private fun checkSegment(name: String) {
        if (name.length > 180) abort("缓存名称过长")
        if (name == "." || name == "..") abort("缓存名称无效")
        for (c in name) if (c == '/' || c == '\\' || c.code < 32 || c in "\":*?<>|") abort("缓存名称包含非法字符")
    }

    private fun text(bytes: ByteArray?): String =
        if (bytes == null) "" else String(bytes, Charsets.UTF_8)

    private fun ok(body: String): ByteArray =
        byteArrayOf(1) + body.toByteArray(Charsets.UTF_8)

    private fun fail(message: String): ByteArray =
        byteArrayOf(0) + message.toByteArray(Charsets.UTF_8)

    private fun abort(message: String): Nothing = throw IOException(message)
}
