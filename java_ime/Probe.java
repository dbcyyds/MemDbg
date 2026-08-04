public class Probe {
  public static void main(String[] a) throws Exception {
    String[] cls = {
      "android.view.SurfaceControl",
      "android.view.SurfaceControl$Builder",
      "android.view.SurfaceControl$Transaction",
      "android.view.SurfaceControlViewHost",
      "android.view.WindowlessWindowManager",
      "android.window.WindowlessWindowManager",
      "android.view.SurfaceControlViewHost$SurfacePackage",
    };
    for (String c : cls) {
      try {
        Class<?> cl = Class.forName(c);
        System.out.println("OK " + c);
        if (c.endsWith("Builder") || c.endsWith("Transaction") || c.endsWith("WindowlessWindowManager") || c.endsWith("SurfaceControlViewHost")) {
          for (java.lang.reflect.Method m : cl.getDeclaredMethods()) {
            String n = m.getName();
            if (n.contains("set") || n.contains("reparent") || n.contains("show") || n.contains("Layer") || n.contains("Parent") || n.contains("View") || n.contains("build") || n.contains("apply") || n.contains("Display"))
              System.out.println("  m " + m.toGenericString().replace("android.view.","").replace("android.window.",""));
          }
          for (java.lang.reflect.Constructor<?> co : cl.getDeclaredConstructors())
            System.out.println("  c " + co.toGenericString().replace("android.view.","").replace("android.content.","").replace("android.window.",""));
        }
      } catch (Throwable e) {
        System.out.println("NO " + c + " " + e);
      }
    }
    // SurfaceControl static methods
    try {
      Class<?> sc = Class.forName("android.view.SurfaceControl");
      for (java.lang.reflect.Method m : sc.getDeclaredMethods()) {
        if (java.lang.reflect.Modifier.isStatic(m.getModifiers())) {
          String n = m.getName();
          if (n.toLowerCase().contains("display") || n.toLowerCase().contains("physical") || n.toLowerCase().contains("built"))
            System.out.println("static " + m);
        }
      }
    } catch (Throwable e) {}
  }
}
