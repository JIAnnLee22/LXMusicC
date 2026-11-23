package jiannlee22.lxmusicc

import android.content.Context
import android.graphics.Color
import android.graphics.Rect
import android.os.Build
import android.text.Editable
import android.text.InputFilter
import android.text.InputType
import android.text.TextWatcher
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout

/** 唯一的输入目标：1dp 透明 EditText；不包含页面或提交业务。仅主线程访问。 */
class ImeBridge(private val activity: SourceActivity) {
    private val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
    private var session = 0
    private var editorKind = 1
    private var muted = true
    private var closed = false
    private var visible = false
    private var lastBottom = -1
    private var lastReference = -1
    private var previousFocus: View? = null
    private external fun nativeIme(session: Int, data: ByteArray, start: Int, end: Int,
                                   composingStart: Int, composingEnd: Int, event: Int)
    private external fun nativeImeInsets(bottom: Int, referenceHeight: Int)

    private val editor = object : EditText(activity) {
        override fun onSelectionChanged(start: Int, end: Int) {
            super.onSelectionChanged(start, end)
            if (!muted) publish()
        }
        override fun onKeyPreIme(keyCode: Int, event: KeyEvent): Boolean {
            if (keyCode == KeyEvent.KEYCODE_BACK && session != 0) {
                if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) hide(true)
                return true
            }
            return super.onKeyPreIme(keyCode, event)
        }
        override fun onCreateInputConnection(info: EditorInfo): InputConnection? {
            val base = super.onCreateInputConnection(info) ?: return null
            // 旋转重建连接时也明确写入，不能只依赖 TextView 缓存的 imeOptions。
            info.imeOptions = (if (editorKind == 1) EditorInfo.IME_ACTION_SEARCH else EditorInfo.IME_ACTION_DONE) or
                EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
            return object : InputConnectionWrapper(base, false) {
                override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean =
                    super.setComposingText(text, newCursorPosition).also { publish() }
                override fun setComposingRegion(start: Int, end: Int): Boolean =
                    super.setComposingRegion(start, end).also { publish() }
                override fun finishComposingText(): Boolean = super.finishComposingText().also { publish() }
                override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean =
                    super.commitText(text, newCursorPosition).also { publish() }
                override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean =
                    super.deleteSurroundingText(beforeLength, afterLength).also { publish() }
                override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean =
                    super.deleteSurroundingTextInCodePoints(beforeLength, afterLength).also { publish() }
            }
        }
    }
    private val layoutListener = android.view.ViewTreeObserver.OnGlobalLayoutListener { reportInsets() }

    init {
        editor.apply {
            setSingleLine(true)
            setBackgroundColor(Color.TRANSPARENT)
            setTextColor(Color.TRANSPARENT)
            setHintTextColor(Color.TRANSPARENT)
            alpha = 0f
            isCursorVisible = false
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            isSaveEnabled = false
            setPadding(0, 0, 0, 0)
            setOnEditorActionListener { _, _, _ -> submit(); true }
            setOnKeyListener { _, key, event ->
                if (key == KeyEvent.KEYCODE_ENTER || key == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                    if (event.action == KeyEvent.ACTION_UP) submit()
                    true
                } else false
            }
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
                override fun afterTextChanged(s: Editable?) { if (!muted) publish() }
            })
        }
        val dp = (activity.resources.displayMetrics.density + 0.5f).toInt().coerceAtLeast(1)
        activity.addContentView(editor, FrameLayout.LayoutParams(dp, dp))
        editor.setOnApplyWindowInsetsListener { _, insets -> reportInsets(insets); insets }
        activity.window.decorView.viewTreeObserver.addOnGlobalLayoutListener(layoutListener)
        editor.requestApplyInsets()
    }

    private fun byteOffset(text: String, index: Int): Int =
        text.substring(0, index.coerceIn(0, text.length)).toByteArray(Charsets.UTF_8).size
    private fun charOffset(bytes: ByteArray, offset: Int): Int =
        String(bytes, 0, offset.coerceIn(0, bytes.size), Charsets.UTF_8).length

    private fun publish(event: Int = 0) {
        if (muted || closed || session == 0) return
        val value = editor.text ?: return
        val text = value.toString()
        val cs = BaseInputConnection.getComposingSpanStart(value)
        val ce = BaseInputConnection.getComposingSpanEnd(value)
        nativeIme(session, text.toByteArray(Charsets.UTF_8), byteOffset(text, editor.selectionStart),
            byteOffset(text, editor.selectionEnd), if (cs < 0) -1 else byteOffset(text, cs),
            if (ce < 0) -1 else byteOffset(text, ce), event)
    }
    private fun submit() {
        // 组合期间的 Enter 只结束本次组合，不把尚未确认的拼音当作搜索/名称提交。
        if (BaseInputConnection.getComposingSpanStart(editor.text) >= 0) {
            BaseInputConnection.removeComposingSpans(editor.text)
            imm.restartInput(editor)
            publish()
        } else publish(1)
    }

    // action: 0=隐藏，1=新会话/显示，2=C 文本或选区更新，3=系统粘贴。
    fun request(action: Int, token: Int, data: ByteArray, start: Int, end: Int, kind: Int) {
        if (closed) return
        try {
            if (action == 0) { hide(false); return }
            if (action != 1 && token != session) return
            if (action == 1) {
                if (session == 0) previousFocus = activity.window.decorView.findFocus()
                session = token
                editorKind = kind
                muted = true
                editor.inputType = if (kind == 3) InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
                    else InputType.TYPE_CLASS_TEXT
                editor.imeOptions = (if (kind == 1) EditorInfo.IME_ACTION_SEARCH else EditorInfo.IME_ACTION_DONE) or
                    EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
                val limit = if (kind == 3) 2047 else 255
                editor.filters = arrayOf(InputFilter { source, s, e, dest, dstart, dend ->
                    val result = dest.subSequence(0, dstart).toString() + source.subSequence(s, e) +
                        dest.subSequence(dend, dest.length)
                    if (result.toByteArray(Charsets.UTF_8).size > limit || result.contains('\u0000') ||
                        result.contains('\n') || result.contains('\r')) {
                        nativeIme(session, "输入过长或包含换行，未插入".toByteArray(Charsets.UTF_8), 0, 0, -1, -1, 3)
                        dest.subSequence(dstart, dend)
                    } else null
                })
            }
            if (action == 1 || action == 2) {
                muted = true
                val text = String(data, Charsets.UTF_8)
                if (editor.text.toString() != text) editor.setText(text)
                editor.setSelection(charOffset(data, start), charOffset(data, end))
                muted = false
                if (action == 1) {
                    editor.requestFocus()
                    imm.restartInput(editor)
                    editor.post { if (!closed && session == token && editor.hasFocus()) {
                        if (!imm.showSoftInput(editor, InputMethodManager.SHOW_IMPLICIT)) editor.postDelayed({
                            if (!closed && session == token && editor.hasFocus())
                                imm.showSoftInput(editor, InputMethodManager.SHOW_IMPLICIT)
                        }, 150)
                    } }
                } else imm.updateSelection(editor, editor.selectionStart, editor.selectionEnd,
                    BaseInputConnection.getComposingSpanStart(editor.text), BaseInputConnection.getComposingSpanEnd(editor.text))
                publish()
            } else if (action == 3) {
                editor.onTextContextMenuItem(android.R.id.paste)
                publish()
            }
        } catch (e: Exception) {
            muted = false
            android.util.Log.w("LXMusicC", "IME request failed", e)
            nativeIme(token, "系统输入失败，请重新点击输入框".toByteArray(Charsets.UTF_8), 0, 0, -1, -1, 4)
            hide(false)
        }
    }

    fun hide(notify: Boolean) {
        if (notify) publish(2)
        muted = true
        session = 0
        imm.hideSoftInputFromWindow(editor.windowToken, 0)
        editor.clearFocus()
        previousFocus?.requestFocus()
        previousFocus = null
    }
    private fun reportInsets(insets: WindowInsets? = editor.rootWindowInsets) {
        if (closed) return
        val bottom = if (Build.VERSION.SDK_INT >= 30 && insets != null) {
            insets.getInsets(WindowInsets.Type.ime()).bottom
        } else {
            val frame = Rect()
            activity.window.decorView.getWindowVisibleDisplayFrame(frame)
            val root = activity.window.decorView
            val location = IntArray(2)
            root.getLocationOnScreen(location)
            val height = (root.height + location[1] - frame.bottom).coerceAtLeast(0)
            @Suppress("DEPRECATION")
            val systemBottom = insets?.systemWindowInsetBottom ?: 0
            @Suppress("DEPRECATION")
            val stableBottom = insets?.stableInsetBottom ?: 0
            // resize 的 surface 已由框架缩小；frame 不再缩小时以 insets 为后备。
            maxOf(height, if (systemBottom > stableBottom) systemBottom else 0)
        }
        val reference = activity.window.decorView.height
        if (bottom != lastBottom || reference != lastReference) {
            lastBottom = bottom; lastReference = reference; nativeImeInsets(bottom, reference)
        }
        val shown = if (Build.VERSION.SDK_INT >= 30 && insets != null)
            insets.isVisible(WindowInsets.Type.ime()) else bottom > 0
        if (visible && !shown && session != 0) {
            val token = session
            // 旋转/分屏时可能先收到旧窗口的零 inset，不能立即销毁新输入连接。
            editor.postDelayed({
                val stillShown = if (Build.VERSION.SDK_INT >= 30)
                    editor.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true else lastBottom > 0
                if (!closed && session == token && !stillShown) hide(true)
            }, 300)
        }
        visible = shown
    }
    fun close() {
        hide(false)
        closed = true
        activity.window.decorView.viewTreeObserver.removeOnGlobalLayoutListener(layoutListener)
        editor.setOnApplyWindowInsetsListener(null)
        (editor.parent as? android.view.ViewGroup)?.removeView(editor)
    }
}
