public class Probe2 {
  public static void main(String[] a) throws Exception {
    for (String c : new String[]{
      "android.window.InputTransferToken",
      "android.view.SurfaceControl",
      "android.hardware.display.DisplayManagerGlobal",
      "android.view.SurfaceSession",
    }) {
      try {
        Class<?> cl = Class.forName(c);
        System.out.println("OK " + c);
        for (java.lang.reflect.Constructor<?> co : cl.getDeclaredConstructors())
          System.out.println("  c " + co);
        for (java.lang.reflect.Method m : cl.getDeclaredMethods()) {
          String n = m.getName();
          if (n.contains("Display") || n.contains("Token") || n.contains("Physical") || n.equals("getInstance"))
            System.out.println("  m " + m);
        }
      } catch (Throwable e) { System.out.println("NO " + c + " " + e); }
    }
    // try create SC and show
    try {
      Class<?> builderClz = Class.forName("android.view.SurfaceControl$Builder");
      Object b = builderClz.getConstructor().newInstance();
      b = builderClz.getMethod("setName", String.class).invoke(b, "ProbeSC");
      b = builderClz.getMethod("setContainerLayer").invoke(b);
      b = builderClz.getMethod("setCallsite", String.class).invoke(b, "probe");
      Object sc = builderClz.getMethod("build").invoke(b);
      System.out.println("SC built " + sc);
      Class<?> tClz = Class.forName("android.view.SurfaceControl$Transaction");
      Object t = tClz.getConstructor().newInstance();
      tClz.getMethod("setLayer", Class.forName("android.view.SurfaceControl"), int.class).invoke(t, sc, 0x7ffffffe);
      tClz.getMethod("setPosition", Class.forName("android.view.SurfaceControl"), float.class, float.class).invoke(t, sc, 0f, 500f);
      // setLayerStack 0
      try {
        tClz.getMethod("setLayerStack", Class.forName("android.view.SurfaceControl"), int.class).invoke(t, sc, 0);
        System.out.println("setLayerStack ok");
      } catch (Throwable e) { System.out.println("setLayerStack " + e); }
      tClz.getMethod("show", Class.forName("android.view.SurfaceControl")).invoke(t, sc);
      tClz.getMethod("apply").invoke(t);
      System.out.println("transaction apply ok");
      Thread.sleep(500);
      // release
      try { sc.getClass().getMethod("release").invoke(sc); } catch (Throwable ignored) {}
    } catch (Throwable e) {
      e.printStackTrace();
    }
  }
}
