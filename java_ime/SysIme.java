/**
 * SysIme — app_process + SurfaceControl/WindowlessWindowManager
 * 系统输入法直接打到悬浮输入条，实时写 live 文件供 ImGui 同步。
 *
 *   app_process64 -Djava.class.path=sys_ime.dex /system/bin SysIme \
 *       <out_path> <title> <initial> [text|number]
 *
 * 文件:
 *   out_path.live  — 实时文本（含拼音 composing）
 *   out_path       — 完成时最终文本
 *   out_path.done  — 完成标记
 *   out_path.cancel
 */
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Display;
import android.view.SurfaceControl;
import android.view.SurfaceControlViewHost;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowlessWindowManager;
import android.view.inputmethod.InputMethodManager;
import android.window.InputTransferToken;

import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

public class SysIme implements ImePanel.Listener {
    private static String outPath;
    private static volatile boolean finished = false;

    private Context ctx;
    private SurfaceControl rootSc;
    private SurfaceControlViewHost host;
    private ImePanel panel;
    private int panelW, panelH, posY;

    public static void main(String[] args) {
        System.out.println("[SysIme] start");
        try {
            outPath = args.length > 0 ? args[0] : "/data/local/tmp/memdbg_ime_out";
            final String title = args.length > 1 ? args[1] : "输入到 ImGui";
            final String initial = args.length > 2 ? args[2] : "";
            final String mode = args.length > 3 ? args[3] : "text";

            Looper.prepareMainLooper();
            ensureTypeface();

            Class<?> atClz = Class.forName("android.app.ActivityThread");
            atClz.getMethod("systemMain").invoke(null);
            Object at = atClz.getMethod("currentActivityThread").invoke(null);
            Context base = (Context) atClz.getMethod("getSystemContext").invoke(at);
            Context ctx = pickContext(base);
            ctx = applyTheme(ctx);

            final SysIme app = new SysIme();
            app.ctx = ctx;
            new Handler(Looper.getMainLooper()).post(new Runnable() {
                @Override
                public void run() {
                    try {
                        app.show(title, initial, mode);
                    } catch (Throwable e) {
                        app.fail(e);
                    }
                }
            });
            Looper.loop();
        } catch (Throwable e) {
            e.printStackTrace();
            try {
                writeBytes(outPath + ".err", e.toString().getBytes(StandardCharsets.UTF_8));
                writeBytes(outPath + ".done", "err".getBytes(StandardCharsets.UTF_8));
            } catch (Throwable ignored) {
            }
            System.exit(2);
        }
    }

    private static Context pickContext(Context base) {
        for (String pkg : new String[]{
                "com.android.shell", "com.android.systemui",
                "com.android.settings", "com.termux", "android"}) {
            try {
                Context c = base.createPackageContext(pkg,
                        Context.CONTEXT_INCLUDE_CODE | Context.CONTEXT_IGNORE_SECURITY);
                System.out.println("[SysIme] ctx=" + pkg);
                return c;
            } catch (Throwable t) {
                System.out.println("[SysIme] pkg " + pkg + " " + t.getClass().getSimpleName());
            }
        }
        return base;
    }

    private void show(String title, String initial, String mode) throws Exception {
        DisplayManager dm = (DisplayManager) ctx.getSystemService(Context.DISPLAY_SERVICE);
        Display display = dm.getDisplay(Display.DEFAULT_DISPLAY);
        float density = ctx.getResources().getDisplayMetrics().density;
        int screenW = ctx.getResources().getDisplayMetrics().widthPixels;
        int screenH = ctx.getResources().getDisplayMetrics().heightPixels;
        panelW = screenW;
        panelH = (int) (160 * density);
        posY = screenH - panelH - (int) (24 * density);
        if (posY < 0) posY = 0;

        // 1) 根 SurfaceControl（容器层，不经 WMS openSession）
        SurfaceControl.Builder b = new SurfaceControl.Builder();
        b.setName("MemDbgSysIme");
        b.setContainerLayer();
        b.setCallsite("SysIme");
        b.setHidden(false);
        rootSc = b.build();

        SurfaceControl.Transaction t = new SurfaceControl.Transaction();
        t.setLayer(rootSc, 0x7ffffffe);
        t.setPosition(rootSc, 0f, (float) posY);
        try {
            t.setLayerStack(rootSc, 0);
        } catch (Throwable ignored) {
        }
        try {
            // 裁剪到面板大小
            t.setCrop(rootSc, new Rect(0, 0, panelW, panelH));
        } catch (Throwable ignored) {
        }
        try {
            // 允许接收输入（部分版本）
            Method m = SurfaceControl.Transaction.class.getMethod(
                    "setTrustedOverlay", SurfaceControl.class, boolean.class);
            m.invoke(t, rootSc, true);
        } catch (Throwable ignored) {
        }
        t.show(rootSc);
        t.apply();
        System.out.println("[SysIme] root SurfaceControl shown " + panelW + "x" + panelH
                + " y=" + posY);

        // 2) WindowlessWindowManager + SurfaceControlViewHost
        Configuration config = ctx.getResources().getConfiguration();
        InputTransferToken token = new InputTransferToken();
        WindowlessWindowManager wwm =
                new WindowlessWindowManager(config, rootSc, token);
        host = new SurfaceControlViewHost(ctx, display, wwm, "MemDbgIme");

        panel = new ImePanel(ctx, title, initial, mode);
        panel.setListener(this);

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                panelW, panelH,
                WindowManager.LayoutParams.TYPE_APPLICATION,
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                        | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                PixelFormat.TRANSLUCENT);
        lp.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;

        try {
            host.setView(panel, lp);
        } catch (Throwable e) {
            System.out.println("[SysIme] setView(lp) fail " + e + ", try wh");
            host.setView(panel, panelW, panelH);
        }
        System.out.println("[SysIme] view attached");

        // 初始 live
        onLive(initial != null ? initial : "");

        final InputTransferToken focusToken = token;
        panel.post(new Runnable() {
            @Override
            public void run() {
                panel.requestFocus();
                panel.showIme();
                new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        panel.requestFocus();
                        panel.showIme();
                        tryFocus(focusToken);
                    }
                }, 250);
            }
        });

        // 超时
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                onCancel();
            }
        }, 5 * 60 * 1000L);
    }

    private void tryFocus(InputTransferToken token) {
        try {
            SurfaceControl.Transaction t = new SurfaceControl.Transaction();
            IBinder binder = token.getToken();
            Method m = SurfaceControl.Transaction.class.getMethod(
                    "setFocusedWindow", IBinder.class, String.class, int.class);
            m.invoke(t, binder, "MemDbgIme", Display.DEFAULT_DISPLAY);
            t.apply();
            System.out.println("[SysIme] setFocusedWindow ok");
        } catch (Throwable e) {
            System.out.println("[SysIme] setFocusedWindow: " + e);
        }
    }

    @Override
    public void onLive(String text) {
        try {
            writeBytes(outPath + ".live",
                    (text != null ? text : "").getBytes(StandardCharsets.UTF_8));
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void onDone(String text) {
        finish(true, text != null ? text : "");
    }

    @Override
    public void onCancel() {
        finish(false, "");
    }

    private synchronized void finish(boolean ok, String text) {
        if (finished) return;
        finished = true;
        try {
            if (ok) {
                writeBytes(outPath, text.getBytes(StandardCharsets.UTF_8));
                writeBytes(outPath + ".live", text.getBytes(StandardCharsets.UTF_8));
            } else {
                writeBytes(outPath + ".cancel", "1".getBytes(StandardCharsets.UTF_8));
            }
            writeBytes(outPath + ".done", "ok".getBytes(StandardCharsets.UTF_8));
            System.out.println("[SysIme] finish ok=" + ok + " text=" + text);
        } catch (Throwable e) {
            e.printStackTrace();
        }
        try {
            if (panel != null) {
                InputMethodManager imm = (InputMethodManager)
                        ctx.getSystemService(Context.INPUT_METHOD_SERVICE);
                imm.hideSoftInputFromWindow(panel.getWindowToken(), 0);
            }
        } catch (Throwable ignored) {
        }
        try {
            if (host != null) host.release();
        } catch (Throwable ignored) {
        }
        try {
            if (rootSc != null) {
                SurfaceControl.Transaction t = new SurfaceControl.Transaction();
                t.reparent(rootSc, null);
                t.apply();
                rootSc.release();
            }
        } catch (Throwable ignored) {
        }
        System.exit(ok ? 0 : 1);
    }

    private void fail(Throwable e) {
        try {
            writeBytes(outPath + ".err",
                    (e != null ? e.toString() : "err").getBytes(StandardCharsets.UTF_8));
            writeBytes(outPath + ".done", "err".getBytes(StandardCharsets.UTF_8));
        } catch (Throwable ignored) {
        }
        if (e != null) e.printStackTrace();
        System.exit(3);
    }

    private static void writeBytes(String path, byte[] data) throws Exception {
        FileOutputStream fos = new FileOutputStream(path);
        fos.write(data);
        fos.close();
    }

    private static Context applyTheme(Context base) {
        try {
            int themeId = android.R.style.Theme_DeviceDefault;
            try {
                base.setTheme(themeId);
            } catch (Throwable ignored) {
            }
            Class<?> ctw = Class.forName("android.view.ContextThemeWrapper");
            return (Context) ctw.getConstructor(Context.class, int.class)
                    .newInstance(base, themeId);
        } catch (Throwable e) {
            return base;
        }
    }

    private static void ensureTypeface() {
        try {
            Class<?> tf = Class.forName("android.graphics.Typeface");
            try {
                Method load = tf.getDeclaredMethod("loadPreinstalledSystemFontMap");
                load.setAccessible(true);
                load.invoke(null);
            } catch (Throwable ignored) {
            }
            Object face = null;
            for (String f : new String[]{
                    "/system/fonts/Roboto-Regular.ttf",
                    "/system/fonts/DroidSans.ttf",
                    "/system/fonts/ZUKChinese.ttf"}) {
                try {
                    if (new java.io.File(f).exists()) {
                        face = tf.getMethod("createFromFile", String.class).invoke(null, f);
                        if (face != null) break;
                    }
                } catch (Throwable ignored) {
                }
            }
            if (face == null) return;
            for (String name : new String[]{
                    "DEFAULT", "DEFAULT_BOLD", "SANS_SERIF", "SERIF", "MONOSPACE"}) {
                try {
                    Field field = tf.getDeclaredField(name);
                    field.setAccessible(true);
                    if (field.get(null) == null) field.set(null, face);
                } catch (Throwable ignored) {
                }
            }
            System.out.println("[SysIme] typeface ready");
        } catch (Throwable e) {
            System.out.println("[SysIme] typeface " + e);
        }
    }
}
