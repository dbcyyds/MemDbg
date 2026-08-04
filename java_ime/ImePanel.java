/**
 * 自绘输入条 + InputConnection，接入系统 IME（不经 TextView/EditText，避免 Settings NPE）
 * 文本变化实时回调 → 写文件 → 原生喂给 ImGui
 */
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

public class ImePanel extends View {
    public interface Listener {
        void onLive(String text);
        void onDone(String text);
        void onCancel();
    }

    private final String title;
    private final String mode;
    private final StringBuilder text = new StringBuilder();
    private final StringBuilder composing = new StringBuilder();
    private Listener listener;
    private float density = 3f;

    private final Paint pBg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pTitle = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pComp = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pBtn = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pBtnOk = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pBtnTxt = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float cancelL, cancelT, cancelR, cancelB;
    private float okL, okT, okR, okB;

    public ImePanel(Context context, String title, String initial, String mode) {
        super(context);
        this.title = title != null ? title : "输入到 ImGui";
        this.mode = mode != null ? mode : "text";
        if (initial != null) text.append(initial);
        try {
            density = context.getResources().getDisplayMetrics().density;
        } catch (Throwable ignored) {
        }
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);

        pBg.setColor(0xF012151C);
        pStroke.setStyle(Paint.Style.STROKE);
        pStroke.setStrokeWidth(dp(1.5f));
        pStroke.setColor(0xFF3AA0E0);
        pTitle.setColor(0xFF5EC8FF);
        pTitle.setTextSize(dp(15));
        pText.setColor(0xFFFFFFFF);
        pText.setTextSize(dp(20));
        pComp.setColor(0xFF8AD7FF);
        pComp.setTextSize(dp(18));
        pBtn.setColor(0xFF2A3344);
        pBtnOk.setColor(0xFF1E6B4A);
        pBtnTxt.setColor(0xFFFFFFFF);
        pBtnTxt.setTextSize(dp(15));
        pBtnTxt.setTextAlign(Paint.Align.CENTER);
    }

    public void setListener(Listener l) { this.listener = l; }

    public String getFullText() {
        return text.toString() + composing.toString();
    }

    private float dp(float v) { return v * density; }

    private void emitLive() {
        if (listener != null) listener.onLive(getFullText());
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        if ("number".equalsIgnoreCase(mode) || "num".equalsIgnoreCase(mode)) {
            outAttrs.inputType = InputType.TYPE_CLASS_NUMBER
                    | InputType.TYPE_NUMBER_FLAG_DECIMAL
                    | InputType.TYPE_NUMBER_FLAG_SIGNED;
        } else {
            // 文本 + 允许建议/中文输入
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;
        }
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        outAttrs.hintText = title;

        return new BaseInputConnection(this, true) {
            @Override
            public boolean commitText(CharSequence s, int newCursorPosition) {
                composing.setLength(0);
                if (s != null) text.append(s);
                invalidate();
                emitLive();
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence s, int newCursorPosition) {
                composing.setLength(0);
                if (s != null) composing.append(s);
                invalidate();
                emitLive();
                return true;
            }

            @Override
            public boolean finishComposingText() {
                if (composing.length() > 0) {
                    text.append(composing);
                    composing.setLength(0);
                    invalidate();
                    emitLive();
                }
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                if (composing.length() > 0) {
                    int n = Math.min(beforeLength, composing.length());
                    composing.delete(composing.length() - n, composing.length());
                } else if (beforeLength > 0 && text.length() > 0) {
                    int n = Math.min(beforeLength, text.length());
                    text.delete(text.length() - n, text.length());
                }
                invalidate();
                emitLive();
                return true;
            }

            @Override
            public CharSequence getTextBeforeCursor(int n, int flags) {
                String full = getFullText();
                int len = full.length();
                int start = Math.max(0, len - n);
                return full.substring(start, len);
            }

            @Override
            public CharSequence getTextAfterCursor(int n, int flags) {
                return "";
            }

            @Override
            public boolean performEditorAction(int actionCode) {
                if (actionCode == EditorInfo.IME_ACTION_DONE
                        || actionCode == EditorInfo.IME_ACTION_GO
                        || actionCode == EditorInfo.IME_ACTION_SEND
                        || actionCode == EditorInfo.IME_ACTION_NEXT) {
                    finishComposingText();
                    if (listener != null) listener.onDone(text.toString());
                    return true;
                }
                return false;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN) {
                    int kc = event.getKeyCode();
                    if (kc == KeyEvent.KEYCODE_DEL) {
                        deleteSurroundingText(1, 0);
                        return true;
                    }
                    if (kc == KeyEvent.KEYCODE_ENTER) {
                        finishComposingText();
                        if (listener != null) listener.onDone(text.toString());
                        return true;
                    }
                }
                return super.sendKeyEvent(event);
            }
        };
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = MeasureSpec.getSize(widthMeasureSpec);
        if (w <= 0) w = (int) dp(400);
        setMeasuredDimension(w, (int) dp(160));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float w = getWidth(), h = getHeight(), pad = dp(12);
        canvas.drawRect(0, 0, w, h, pBg);
        canvas.drawRect(1, 1, w - 1, h - 1, pStroke);
        canvas.drawText(title + " · 系统输入法 → ImGui", pad, pad + dp(14), pTitle);

        float fieldT = pad + dp(24);
        float fieldB = fieldT + dp(52);
        Paint fieldBg = new Paint(Paint.ANTI_ALIAS_FLAG);
        fieldBg.setColor(0xFF1E2430);
        canvas.drawRect(pad, fieldT, w - pad, fieldB, fieldBg);
        Paint fs = new Paint(Paint.ANTI_ALIAS_FLAG);
        fs.setStyle(Paint.Style.STROKE);
        fs.setColor(0xFF5EC8FF);
        fs.setStrokeWidth(dp(1));
        canvas.drawRect(pad, fieldT, w - pad, fieldB, fs);

        String solid = text.toString();
        String comp = composing.toString();
        String show = solid + comp;
        if (show.isEmpty()) show = "点这里，用系统键盘输入…";
        Paint tp = (text.length() + composing.length()) > 0 ? pText : pTitle;
        float maxW = w - pad * 2 - dp(16);
        while (show.length() > 1 && tp.measureText(show) > maxW)
            show = show.substring(1);
        canvas.drawText(show, pad + dp(8), fieldT + dp(34), tp);
        if (comp.length() > 0) {
            // underline composing roughly
            float cx = pad + dp(8) + pText.measureText(solid);
            float cw = pComp.measureText(comp);
            Paint ul = new Paint();
            ul.setColor(0xFF5EC8FF);
            ul.setStrokeWidth(dp(2));
            canvas.drawLine(cx, fieldB - dp(8), cx + cw, fieldB - dp(8), ul);
        }

        float btnH = dp(40), btnY = h - pad - btnH, gap = dp(8);
        float btnW = (w - pad * 2 - gap) / 2f;
        cancelL = pad; cancelT = btnY; cancelR = pad + btnW; cancelB = btnY + btnH;
        okL = cancelR + gap; okT = btnY; okR = okL + btnW; okB = btnY + btnH;
        canvas.drawRect(cancelL, cancelT, cancelR, cancelB, pBtn);
        canvas.drawRect(okL, okT, okR, okB, pBtnOk);
        canvas.drawText("取消", (cancelL + cancelR) / 2f, (cancelT + cancelB) / 2f + dp(5), pBtnTxt);
        canvas.drawText("完成", (okL + okR) / 2f, (okT + okB) / 2f + dp(5), pBtnTxt);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getAction() == MotionEvent.ACTION_UP) {
            float x = e.getX(), y = e.getY();
            if (x >= cancelL && x <= cancelR && y >= cancelT && y <= cancelB) {
                if (listener != null) listener.onCancel();
                return true;
            }
            if (x >= okL && x <= okR && y >= okT && y <= okB) {
                // finish composing first
                if (composing.length() > 0) {
                    text.append(composing);
                    composing.setLength(0);
                }
                if (listener != null) listener.onDone(text.toString());
                return true;
            }
            requestFocus();
            showIme();
            return true;
        }
        return super.onTouchEvent(e);
    }

    public void showIme() {
        try {
            requestFocus();
            Object imm = getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm instanceof InputMethodManager) {
                InputMethodManager im = (InputMethodManager) imm;
                im.restartInput(this);
                im.showSoftInput(this, InputMethodManager.SHOW_FORCED);
                try {
                    im.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
                } catch (Throwable ignored) {
                }
            }
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }
}
