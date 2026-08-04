/**
 * IconDump — 通过 ActivityThread + PackageManager 导出应用图标为 PNG
 *
 *   CLASSPATH=icon_dump.dex app_process64 /system/bin IconDump \
 *       <out_dir> [pkg1 pkg2 ...]
 *
 * 无包名参数时导出全部已安装应用（用户+系统）。
 * 成功写 <out_dir>/<package>.png
 * 日志 stderr；结束码 0。
 *
 * 纯反射，javac 不依赖 android.jar。
 */
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

public class IconDump {
    private static final int SIZE = 96; // 导出边长

    public static void main(String[] args) {
        String outDir = args.length > 0 ? args[0] : "/data/local/tmp/memdbg_icons";
        List<String> only = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i] != null && args[i].indexOf('.') > 0) only.add(args[i]);
        }
        try {
            new File(outDir).mkdirs();
            // 必须先 prepare Looper（与 SysIme 相同）
            Class<?> looperClz = Class.forName("android.os.Looper");
            try {
                looperClz.getMethod("prepareMainLooper").invoke(null);
            } catch (Throwable t) {
                try {
                    looperClz.getMethod("prepare").invoke(null);
                } catch (Throwable ignored) {
                }
            }
            // 准备系统 Context
            Class<?> atClz = Class.forName("android.app.ActivityThread");
            atClz.getMethod("systemMain").invoke(null);
            Object at = atClz.getMethod("currentActivityThread").invoke(null);
            Object baseCtx = atClz.getMethod("getSystemContext").invoke(at);
            Object ctx = pickContext(baseCtx);
            Object pm = ctx.getClass().getMethod("getPackageManager").invoke(ctx);

            List<String> pkgs = only.isEmpty() ? listPackages(pm) : only;
            int ok = 0, fail = 0, skip = 0;
            for (String pkg : pkgs) {
                File out = new File(outDir, pkg + ".png");
                if (out.isFile() && out.length() > 64) {
                    skip++;
                    continue; // 已有缓存
                }
                try {
                    if (dumpOne(pm, pkg, out)) ok++;
                    else fail++;
                } catch (Throwable t) {
                    fail++;
                    System.err.println("[IconDump] " + pkg + " " + t.getClass().getSimpleName()
                            + ": " + t.getMessage());
                }
            }
            System.out.println("[IconDump] ok=" + ok + " fail=" + fail + " skip=" + skip
                    + " dir=" + outDir);
            System.exit(0);
        } catch (Throwable e) {
            e.printStackTrace();
            System.exit(2);
        }
    }

    private static Object pickContext(Object base) {
        String[] pkgs = {
                "com.android.shell", "com.android.systemui",
                "com.android.settings", "android"
        };
        for (String pkg : pkgs) {
            try {
                Method m = base.getClass().getMethod(
                        "createPackageContext", String.class, int.class);
                // CONTEXT_INCLUDE_CODE=1 CONTEXT_IGNORE_SECURITY=2
                Object c = m.invoke(base, pkg, 1 | 2);
                if (c != null) {
                    System.err.println("[IconDump] ctx=" + pkg);
                    return c;
                }
            } catch (Throwable ignored) {
            }
        }
        return base;
    }

    @SuppressWarnings("unchecked")
    private static List<String> listPackages(Object pm) throws Exception {
        List<String> out = new ArrayList<>();
        // getInstalledApplications(0)
        Method getApps = null;
        for (Method m : pm.getClass().getMethods()) {
            if (m.getName().equals("getInstalledApplications")
                    && m.getParameterTypes().length == 1) {
                getApps = m;
                break;
            }
        }
        if (getApps == null) return out;
        Object list = getApps.invoke(pm, 0);
        if (!(list instanceof List)) return out;
        for (Object ai : (List<?>) list) {
            try {
                Object name = ai.getClass().getField("packageName").get(ai);
                if (name != null) out.add(name.toString());
            } catch (Throwable ignored) {
            }
        }
        return out;
    }

    private static boolean dumpOne(Object pm, String pkg, File out) throws Exception {
        // ApplicationInfo
        Method getAi = null;
        for (Method m : pm.getClass().getMethods()) {
            if (m.getName().equals("getApplicationInfo")
                    && m.getParameterTypes().length == 2
                    && m.getParameterTypes()[0] == String.class) {
                getAi = m;
                break;
            }
        }
        if (getAi == null) return false;
        Object ai = getAi.invoke(pm, pkg, 0);
        if (ai == null) return false;

        // Drawable icon = pm.getApplicationIcon(ai) 或 loadIcon
        Object drawable = null;
        try {
            drawable = pm.getClass()
                    .getMethod("getApplicationIcon",
                            Class.forName("android.content.pm.ApplicationInfo"))
                    .invoke(pm, ai);
        } catch (Throwable t1) {
            try {
                drawable = ai.getClass()
                        .getMethod("loadIcon",
                                Class.forName("android.content.pm.PackageManager"))
                        .invoke(ai, pm);
            } catch (Throwable t2) {
                return false;
            }
        }
        if (drawable == null) return false;

        // Bitmap.createBitmap(SIZE, SIZE, ARGB_8888) — 反射取枚举常量
        Class<?> bmpClz = Class.forName("android.graphics.Bitmap");
        Class<?> cfgClz = Class.forName("android.graphics.Bitmap$Config");
        Object argb = null;
        for (Object c : (Object[]) cfgClz.getMethod("values").invoke(null)) {
            if ("ARGB_8888".equals(c.toString()) || c.toString().endsWith("ARGB_8888")) {
                argb = c;
                break;
            }
        }
        if (argb == null) argb = ((Object[]) cfgClz.getMethod("values").invoke(null))[0];
        Object bitmap = bmpClz.getMethod("createBitmap", int.class, int.class, cfgClz)
                .invoke(null, SIZE, SIZE, argb);

        // Canvas + setBounds + draw
        Class<?> canvasClz = Class.forName("android.graphics.Canvas");
        Object canvas = canvasClz.getConstructor(bmpClz).newInstance(bitmap);
        drawable.getClass().getMethod("setBounds", int.class, int.class, int.class, int.class)
                .invoke(drawable, 0, 0, SIZE, SIZE);
        drawable.getClass().getMethod("draw", canvasClz).invoke(drawable, canvas);

        // compress PNG
        Class<?> fmtClz = Class.forName("android.graphics.Bitmap$CompressFormat");
        Object png = null;
        for (Object c : (Object[]) fmtClz.getMethod("values").invoke(null)) {
            if ("PNG".equals(c.toString()) || c.toString().endsWith("PNG")) {
                png = c;
                break;
            }
        }
        if (png == null) return false;
        File tmp = new File(out.getAbsolutePath() + ".tmp");
        FileOutputStream fos = new FileOutputStream(tmp);
        Boolean ok = (Boolean) bmpClz.getMethod("compress", fmtClz, int.class,
                java.io.OutputStream.class).invoke(bitmap, png, 90, fos);
        fos.close();
        try {
            bmpClz.getMethod("recycle").invoke(bitmap);
        } catch (Throwable ignored) {
        }
        if (ok == null || !ok.booleanValue() || tmp.length() < 32) {
            tmp.delete();
            return false;
        }
        if (out.exists()) out.delete();
        return tmp.renameTo(out);
    }
}
