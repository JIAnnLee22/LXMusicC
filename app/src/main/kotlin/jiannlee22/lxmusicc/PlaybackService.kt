package jiannlee22.lxmusicc

import android.app.PendingIntent
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.ForwardingPlayer
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import androidx.media3.session.MediaLibraryService
import androidx.media3.session.MediaLibraryService.MediaLibrarySession
import org.json.JSONObject

/** C 持有队列/控制策略；此类只适配 Android 音频、会话与前台通知。 */
@UnstableApi
class PlaybackService : MediaLibraryService() {
    private lateinit var output: ExoPlayer
    private var session: MediaLibrarySession? = null
    private lateinit var catalog: MediaCatalog
    private val handler = Handler(Looper.getMainLooper())
    private var generation = 0L
    @Volatile private var closed = false
    private var failure: ByteArray? = null
    private var updating = false
    private var endedGeneration = -1L
    private var registered = false

    private external fun nativeReady(): Boolean
    private external fun nativeGone()
    private external fun nativeCommand(action: Int, position: Long)
    private external fun nativeState(generation: Long, state: Int, playing: Boolean,
        requested: Boolean, position: Long, duration: Long, error: ByteArray?)
    private external fun nativeCanSkip(delta: Int): Boolean
    private external fun nativeSeek(id: ByteArray, position: Long)

    override fun onCreate() {
        super.onCreate()
        try {
            output = ExoPlayer.Builder(this).build().apply {
                setAudioAttributes(AudioAttributes.Builder().setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(), true)
                setHandleAudioBecomingNoisy(true)
                setWakeMode(C.WAKE_MODE_NETWORK)
                pauseAtEndOfMediaItems = true
            }
            output.addListener(object : Player.Listener {
                override fun onEvents(player: Player, events: Player.Events) { report() }
                override fun onPlayerError(error: PlaybackException) {
                    Log.e("LXMusicC", "Playback: ${error.errorCodeName}")
                    failure = "播放失败，请检查网络后点击播放重试".toByteArray()
                    report()
                }
                override fun onPlaybackStateChanged(state: Int) {
                    if (state == Player.STATE_ENDED) ended()
                }
                override fun onPlayWhenReadyChanged(value: Boolean, reason: Int) {
                    if (!value && reason == Player.PLAY_WHEN_READY_CHANGE_REASON_END_OF_MEDIA_ITEM) ended()
                }
            })
            setListener(object : MediaSessionService.Listener {
                override fun onForegroundServiceStartNotAllowedException() {
                    output.pause()
                    failure = "后台播放受限，请回到前台点击播放重试".toByteArray()
                    report()
                }
            })
            val launch = PendingIntent.getActivity(this, 0,
                Intent(this, SourceActivity::class.java).setAction(OPEN_PLAYER)
                    .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            catalog = MediaCatalog(applicationContext)
            session = MediaLibrarySession.Builder(this, Controls(output), catalog).setSessionActivity(launch).build()
            // NativeActivity 直接启动，无 MediaController 绑定，需主动注册以启用通知/前台提升。
            addSession(session!!)
            catalog.boot { ok ->
                if (!ok || !nativeReady()) { PlaybackStart.failed(); stopSelf() }
                else { registered = true; handler.post(tick) }
            }
        } catch (e: Exception) {
            Log.e("LXMusicC", "Cannot initialize playback service", e)
            PlaybackStart.failed()
            stopSelf()
        }
    }

    // 系统通知、锁屏和耳机命令全部回到 C，再调用原始 output，避免 Kotlin 维护队列。
    private inner class Controls(player: Player) : ForwardingPlayer(player) {
        private fun command(action: Int, position: Long = 0) { catalog.dispatch { nativeCommand(action, position) } }
        override fun play() { command(2) }
        override fun pause() { command(3) }
        override fun setPlayWhenReady(value: Boolean) { command(if (value) 2 else 3) }
        override fun stop() { command(8) }
        override fun seekTo(positionMs: Long) { command(4, positionMs) }
        override fun seekTo(mediaItemIndex: Int, positionMs: Long) {
            if (mediaItemIndex !in 0 until output.mediaItemCount) return
            val id = output.getMediaItemAt(mediaItemIndex).mediaId.toByteArray()
            catalog.dispatch { nativeSeek(id, if (positionMs == C.TIME_UNSET) 0 else positionMs) }
        }
        override fun seekToDefaultPosition() { command(4) }
        override fun seekToDefaultPosition(mediaItemIndex: Int) { seekTo(mediaItemIndex, 0) }
        override fun seekBack() { command(4, (output.currentPosition - 10_000).coerceAtLeast(0)) }
        override fun seekForward() { command(4, output.currentPosition + 10_000) }
        override fun seekToNext() { command(5) }
        override fun seekToPrevious() { command(6) }
        override fun seekToNextMediaItem() { command(5) }
        override fun seekToPreviousMediaItem() { command(6) }
        // Callback已在C完成规范化并异步下发镜像，禁止框架绕过C再改一次队列。
        override fun setMediaItem(item: MediaItem) {}
        override fun setMediaItem(item: MediaItem, positionMs: Long) {}
        override fun setMediaItem(item: MediaItem, resetPosition: Boolean) {}
        override fun setMediaItems(items: List<MediaItem>) {}
        override fun setMediaItems(items: List<MediaItem>, resetPosition: Boolean) {}
        override fun setMediaItems(items: List<MediaItem>, index: Int, positionMs: Long) {}
        override fun hasNextMediaItem() = nativeCanSkip(1)
        override fun hasPreviousMediaItem() = nativeCanSkip(-1)
        override fun getAvailableCommands(): Player.Commands = super.getAvailableCommands().buildUpon()
            .add(Player.COMMAND_SEEK_TO_NEXT).add(Player.COMMAND_SEEK_TO_PREVIOUS)
            .add(Player.COMMAND_SEEK_TO_NEXT_MEDIA_ITEM).add(Player.COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM)
            .add(Player.COMMAND_SET_MEDIA_ITEM).add(Player.COMMAND_SEEK_TO_MEDIA_ITEM)
            .remove(Player.COMMAND_CHANGE_MEDIA_ITEMS).remove(Player.COMMAND_SET_REPEAT_MODE)
            .remove(Player.COMMAND_SET_SHUFFLE_MODE).remove(Player.COMMAND_SET_SPEED_AND_PITCH).build()
        override fun isCommandAvailable(command: Int) = availableCommands.contains(command)
    }

    private fun ended() {
        if (!updating && registered && endedGeneration != generation) {
            endedGeneration = generation
            nativeCommand(7, generation)
        }
    }
    /** C队列的系统时间线镜像；Exo不负责循环/随机/自动下一首。 */
    fun applyQueue(data: ByteArray, token: Long, autoplay: Boolean): Boolean {
        if (closed) return false
        return handler.post {
            if (closed) return@post
            try {
                val snapshot = JSONObject(data.toString(Charsets.UTF_8))
                val items = MediaCatalog.items(snapshot)
                updating = true
                generation = token; failure = null
                output.setMediaItems(items, snapshot.getInt("index"), snapshot.getLong("position"))
                output.prepare()
                output.playWhenReady = autoplay
            } catch (e: Exception) {
                Log.e("LXMusicC", "Cannot apply media queue", e)
                failure = "队列加载失败，请重新选曲".toByteArray()
            } finally { updating = false; report() }
        }
    }
    /** 任何 JNI 线程调用；只排入主线程，不同步回调 C，避免锁重入。 */
    fun applyPlayback(action: Int, url: ByteArray?, title: ByteArray?, artist: ByteArray?,
        position: Long, token: Long, autoplay: Boolean): Boolean {
        if (closed) return false
        return handler.post {
            if (closed) return@post
            try {
                if (token == generation) {
                    when (action) {
                        2 -> {
                            failure = null
                            if (output.playbackState == Player.STATE_IDLE) output.prepare()
                            output.play()
                        }
                        3 -> output.pause()
                        4 -> output.seekTo(position)
                        8 -> { output.stop(); output.playWhenReady = false }
                    }
                }
                report()
            } catch (e: Exception) {
                Log.e("LXMusicC", "Playback command failed", e)
                failure = "播放失败，请检查地址或网络后重试".toByteArray()
                report()
            }
        }
    }

    private fun report() {
        if (closed || updating || !registered || !::output.isInitialized) return
        nativeState(generation, output.playbackState, output.isPlaying, output.playWhenReady,
            output.currentPosition.coerceAtLeast(0), output.duration.coerceAtLeast(0), failure)
    }
    private val tick = object : Runnable {
        override fun run() {
            if (!closed) {
                if (output.isPlaying) report()
                session?.let { catalog.refresh(it) }
                handler.postDelayed(this, 1000)
            }
        }
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaLibrarySession? = session
    // 播放时移除最近任务不停止；暂停时允许系统回收，不创建无界空闲服务。
    override fun onTaskRemoved(rootIntent: Intent?) {
        if (!::output.isInitialized || !output.playWhenReady) stopSelf()
    }
    override fun onDestroy() {
        closed = true
        handler.removeCallbacksAndMessages(null)
        if (::catalog.isInitialized) catalog.close()
        if (registered) nativeGone()
        registered = false
        session?.release()
        if (::output.isInitialized) output.release()
        super.onDestroy()
    }

    companion object {
        const val OPEN_PLAYER = "jiannlee22.lxmusicc.OPEN_PLAYER"
        init { System.loadLibrary("lxmusicc") }
    }
}

/** 启动只发生在用户前台操作中；不持有 Activity。媒体会话通知无需 POST_NOTIFICATIONS。 */
object PlaybackStart {
    private external fun nativeFailure()
    fun failed() { nativeFailure() }
}
