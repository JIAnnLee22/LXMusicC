package jiannlee22.lxmusicc

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.LibraryResult
import androidx.media3.session.MediaLibraryService.LibraryParams
import androidx.media3.session.MediaLibraryService.MediaLibrarySession
import androidx.media3.session.MediaSession
import androidx.media3.session.SessionCommand
import androidx.media3.session.SessionError
import com.google.common.collect.ImmutableList
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.SettableFuture
import org.json.JSONObject
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** 只适配Media3协议与C快照；目录、身份、选曲和持久化均在C。 */
@UnstableApi
class MediaCatalog(private val context: Context) : MediaLibrarySession.Callback {
    private val worker = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var closed = false
    @Volatile private var ready = false
    private val refreshing = AtomicBoolean(false)
    // 仅worker访问；所有订阅合计最多128个，通知在主线程发出。
    private val subscriptions = mutableMapOf<MediaSession.ControllerInfo, MutableSet<String>>()
    private val versions = mutableMapOf<String, String>()
    private external fun nativeBoot(directory: ByteArray): Boolean
    private external fun nativeQuery(action: Int, id: ByteArray, page: Int, size: Int, position: Long): ByteArray?

    fun boot(done: (Boolean) -> Unit) {
        worker.execute {
            ready = try { nativeBoot(context.filesDir.absolutePath.toByteArray()) } catch (_: Exception) { false }
            main.post { if (!closed) done(ready) }
        }
    }
    fun dispatch(command: () -> Unit) {
        if (!closed) worker.execute { main.post { if (!closed && ready) command() } }
    }
    fun close() { closed = true; worker.shutdown() }
    fun refresh(session: MediaLibrarySession) {
        if (closed || !refreshing.compareAndSet(false, true)) return
        worker.execute {
            try {
                if (!ready || closed) return@execute
                val parents = subscriptions.values.flatten().toSet()
                versions.keys.retainAll(parents)
                for (id in parents) {
                    val data = try { query(0, id, size = 1) } catch (_: Exception) { null }
                    val signature = data?.toString() ?: "removed"
                    if (versions.put(id, signature) != signature) {
                        val count = data?.optInt("total", 0) ?: 0
                        main.post { if (!closed) session.notifyChildrenChanged(id, count, null) }
                    }
                }
            } finally { refreshing.set(false) }
        }
    }
    override fun onDisconnected(session: MediaSession, controller: MediaSession.ControllerInfo) {
        if (!closed) worker.execute { subscriptions.remove(controller) }
    }
    private fun query(action: Int, id: String, page: Int = 0, size: Int = 100, position: Long = 0): JSONObject {
        check(ready && !closed) { "媒体库尚未就绪" }
        val bytes = nativeQuery(action, id.toByteArray(), page, size, position)
            ?: throw IllegalArgumentException("媒体条目不存在或已过期，请刷新列表")
        return JSONObject(bytes.toString(Charsets.UTF_8))
    }
    private fun <T> async(block: () -> T): ListenableFuture<T> {
        val future = SettableFuture.create<T>()
        if (closed) { future.setException(IllegalStateException("媒体服务已关闭")); return future }
        worker.execute {
            try { future.set(block()) } catch (e: Exception) { future.setException(e) }
        }
        return future
    }
    override fun onConnect(session: MediaSession, controller: MediaSession.ControllerInfo): MediaSession.ConnectionResult {
        // 开放标准浏览/控制，不开放任意URL注入、队列编辑或自定义命令。
        val commands = MediaSession.ConnectionResult.DEFAULT_SESSION_AND_LIBRARY_COMMANDS.buildUpon()
            .remove(SessionCommand.COMMAND_CODE_LIBRARY_SEARCH)
            .remove(SessionCommand.COMMAND_CODE_LIBRARY_GET_SEARCH_RESULT).build()
        return MediaSession.ConnectionResult.AcceptedResultBuilder(session)
            .setAvailableSessionCommands(commands).build()
    }
    override fun onGetLibraryRoot(session: MediaLibrarySession, browser: MediaSession.ControllerInfo,
        params: LibraryParams?): ListenableFuture<LibraryResult<MediaItem>> = async {
        LibraryResult.ofItem(items(query(1, "root")).single(), params)
    }
    override fun onGetItem(session: MediaLibrarySession, browser: MediaSession.ControllerInfo,
        mediaId: String): ListenableFuture<LibraryResult<MediaItem>> = async {
        try { LibraryResult.ofItem(items(query(1, mediaId)).single(), null) }
        catch (_: IllegalArgumentException) { LibraryResult.ofError(SessionError.ERROR_BAD_VALUE) }
    }
    override fun onGetChildren(session: MediaLibrarySession, browser: MediaSession.ControllerInfo,
        parentId: String, page: Int, pageSize: Int, params: LibraryParams?):
        ListenableFuture<LibraryResult<ImmutableList<MediaItem>>> = async {
        // MediaBrowserCompat无分页订阅会以MAX_VALUE请求；桥内仍每次最多100项。
        if (page == 0 && pageSize == Int.MAX_VALUE) {
            val rows = mutableListOf<MediaItem>()
            var next = 0
            do {
                val data = query(0, parentId, next++, 100)
                rows.addAll(items(data))
            } while (rows.size < data.getInt("total") && next < 6)
            LibraryResult.ofItemList(rows, params)
        } else if (page < 0 || pageSize !in 1..100) LibraryResult.ofError(SessionError.ERROR_BAD_VALUE)
        else try { LibraryResult.ofItemList(items(query(0, parentId, page, pageSize)), params) }
        catch (_: IllegalArgumentException) { LibraryResult.ofError(SessionError.ERROR_BAD_VALUE) }
    }
    override fun onSubscribe(session: MediaLibrarySession, browser: MediaSession.ControllerInfo,
        parentId: String, params: LibraryParams?): ListenableFuture<LibraryResult<Void>> = async {
        try {
            require(subscriptions.values.sumOf { it.size } < 128) { "媒体订阅过多" }
            val count = query(0, parentId, size = 1).getInt("total")
            subscriptions.getOrPut(browser) { mutableSetOf() }.add(parentId)
            main.post { if (!closed) session.notifyChildrenChanged(browser, parentId, count, params) }
            LibraryResult.ofVoid(params)
        } catch (_: IllegalArgumentException) { LibraryResult.ofError(SessionError.ERROR_BAD_VALUE) }
    }
    override fun onUnsubscribe(session: MediaLibrarySession, browser: MediaSession.ControllerInfo,
        parentId: String): ListenableFuture<LibraryResult<Void>> = async {
        subscriptions[browser]?.remove(parentId)
        LibraryResult.ofVoid()
    }
    override fun onSetMediaItems(session: MediaSession, controller: MediaSession.ControllerInfo,
        mediaItems: List<MediaItem>, startIndex: Int, startPositionMs: Long):
        ListenableFuture<MediaSession.MediaItemsWithStartPosition> = async {
        require(mediaItems.size == 1 && (startIndex == 0 || startIndex == C.INDEX_UNSET)) {
            "请用可浏览条目的mediaId选曲，不支持外部队列编辑"
        }
        val data = query(2, mediaItems.single().mediaId, position = startPositionMs.coerceAtLeast(0))
        MediaSession.MediaItemsWithStartPosition(items(data), data.getInt("index"), data.getLong("position"))
    }
    override fun onPlaybackResumption(session: MediaSession, controller: MediaSession.ControllerInfo):
        ListenableFuture<MediaSession.MediaItemsWithStartPosition> = async {
        val data = query(3, "queue")
        MediaSession.MediaItemsWithStartPosition(items(data), data.getInt("index"), data.getLong("position"))
    }
    companion object {
        fun items(data: JSONObject): List<MediaItem> {
            val array = data.getJSONArray("items")
            return (0 until array.length()).map { index ->
                val row = array.getJSONObject(index)
                val folder = row.optInt("folder") == 1
                val metadata = MediaMetadata.Builder().setTitle(row.getString("title"))
                    .setArtist(row.optString("artist")).setAlbumTitle(row.optString("album"))
                    .setIsBrowsable(folder).setIsPlayable(!folder)
                    .setMediaType(if (folder) MediaMetadata.MEDIA_TYPE_FOLDER_MIXED else MediaMetadata.MEDIA_TYPE_MUSIC)
                if (!folder) metadata.setDurationMs(row.optLong("duration"))
                MediaItem.Builder().setMediaId(row.getString("id")).setMediaMetadata(metadata.build())
                    .apply { if (row.has("url")) setUri(Uri.parse(row.getString("url"))) }.build()
            }
        }
    }
}
