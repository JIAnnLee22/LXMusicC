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
import androidx.media3.common.MediaMetadata
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService

/** C 持有队列/控制策略；此类只适配 Android 音频、会话与前台通知。 */
@UnstableApi
class PlaybackService : MediaSessionService() {
    private lateinit var output: ExoPlayer
    private var session: MediaSession? = null
    private val handler = Handler(Looper.getMainLooper())
    private var generation = 0L
    @Volatile private var closed = false
    private var failure: ByteArray? = null

    private external fun nativeReady(): Boolean
    private external fun nativeGone()
    private external fun nativeCommand(action: Int, position: Long)
    private external fun nativeState(generation: Long, state: Int, playing: Boolean,
        requested: Boolean, position: Long, duration: Long, error: ByteArray?)
    private external fun nativeCanSkip(delta: Int): Boolean

    override fun onCreate() {
        super.onCreate()
        try {
            output = ExoPlayer.Builder(this).build().apply {
                setAudioAttributes(AudioAttributes.Builder().setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(), true)
                setHandleAudioBecomingNoisy(true)
                setWakeMode(C.WAKE_MODE_NETWORK)
            }
            output.addListener(object : Player.Listener {
                override fun onEvents(player: Player, events: Player.Events) { report() }
                override fun onPlayerError(error: PlaybackException) {
                    Log.e("LXMusicC", "Playback: ${error.errorCodeName}")
                    failure = "播放失败，请检查网络后点击播放重试".toByteArray()
                    report()
                }
                override fun onPlaybackStateChanged(state: Int) {
                    if (state == Player.STATE_ENDED) nativeCommand(7, generation)
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
                Intent(this, SourceActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            session = MediaSession.Builder(this, Controls(output)).setSessionActivity(launch).build()
            // NativeActivity 直接启动，无 MediaController 绑定，需主动注册以启用通知/前台提升。
            addSession(session!!)
            if (!nativeReady()) { stopSelf(); return }
            handler.post(tick)
        } catch (e: Exception) {
            Log.e("LXMusicC", "Cannot initialize playback service", e)
            PlaybackStart.failed()
            stopSelf()
        }
    }

    // 系统通知、锁屏和耳机命令全部回到 C，再调用原始 output，避免 Kotlin 维护队列。
    private inner class Controls(player: Player) : ForwardingPlayer(player) {
        override fun play() { nativeCommand(2, 0) }
        override fun pause() { nativeCommand(3, 0) }
        override fun setPlayWhenReady(value: Boolean) { nativeCommand(if (value) 2 else 3, 0) }
        override fun stop() { nativeCommand(8, 0) }
        override fun seekTo(positionMs: Long) { nativeCommand(4, positionMs) }
        override fun seekTo(mediaItemIndex: Int, positionMs: Long) { nativeCommand(4, positionMs) }
        override fun seekToDefaultPosition() { nativeCommand(4, 0) }
        override fun seekToDefaultPosition(mediaItemIndex: Int) { nativeCommand(4, 0) }
        override fun seekBack() { nativeCommand(4, (output.currentPosition - 10_000).coerceAtLeast(0)) }
        override fun seekForward() { nativeCommand(4, output.currentPosition + 10_000) }
        override fun seekToNext() { nativeCommand(5, 0) }
        override fun seekToPrevious() { nativeCommand(6, 0) }
        override fun seekToNextMediaItem() { nativeCommand(5, 0) }
        override fun seekToPreviousMediaItem() { nativeCommand(6, 0) }
        override fun hasNextMediaItem() = nativeCanSkip(1)
        override fun hasPreviousMediaItem() = nativeCanSkip(-1)
        override fun getAvailableCommands(): Player.Commands = super.getAvailableCommands().buildUpon()
            .add(Player.COMMAND_SEEK_TO_NEXT).add(Player.COMMAND_SEEK_TO_PREVIOUS)
            .add(Player.COMMAND_SEEK_TO_NEXT_MEDIA_ITEM).add(Player.COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM)
            .remove(Player.COMMAND_CHANGE_MEDIA_ITEMS).remove(Player.COMMAND_SET_MEDIA_ITEM).build()
        override fun isCommandAvailable(command: Int) = availableCommands.contains(command)
    }

    /** 任何 JNI 线程调用；只排入主线程，不同步回调 C，避免锁重入。 */
    fun applyPlayback(action: Int, url: ByteArray?, title: ByteArray?, artist: ByteArray?,
        position: Long, token: Long, autoplay: Boolean): Boolean {
        if (closed) return false
        return handler.post {
            if (closed) return@post
            try {
                if (action == 1) {
                    generation = token
                    failure = null
                    output.setMediaItem(MediaItem.Builder().setUri(url!!.toString(Charsets.UTF_8))
                        .setMediaMetadata(MediaMetadata.Builder().setTitle(title!!.toString(Charsets.UTF_8))
                            .setArtist(artist!!.toString(Charsets.UTF_8)).build()).build(), position)
                    output.prepare()
                    output.playWhenReady = autoplay
                } else if (token == generation) {
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
        if (closed || !::output.isInitialized) return
        nativeState(generation, output.playbackState, output.isPlaying, output.playWhenReady,
            output.currentPosition.coerceAtLeast(0), output.duration.coerceAtLeast(0), failure)
    }
    private val tick = object : Runnable {
        override fun run() {
            if (!closed) {
                if (output.isPlaying) report()
                handler.postDelayed(this, 1000)
            }
        }
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session
    // 播放时移除最近任务不停止；暂停时允许系统回收，不创建无界空闲服务。
    override fun onTaskRemoved(rootIntent: Intent?) {
        if (!::output.isInitialized || !output.playWhenReady) stopSelf()
    }
    override fun onDestroy() {
        closed = true
        handler.removeCallbacksAndMessages(null)
        nativeGone()
        session?.release()
        if (::output.isInitialized) output.release()
        super.onDestroy()
    }

    companion object { init { System.loadLibrary("lxmusicc") } }
}

/** 启动只发生在用户前台操作中；不持有 Activity。媒体会话通知无需 POST_NOTIFICATIONS。 */
object PlaybackStart {
    private external fun nativeFailure()
    fun failed() { nativeFailure() }
}
