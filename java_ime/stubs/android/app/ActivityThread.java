package android.app;
import android.content.Context;
public class ActivityThread {
  public static ActivityThread systemMain() { return new ActivityThread(); }
  public static ActivityThread currentActivityThread() { return new ActivityThread(); }
  public Context getSystemContext() { return null; }
}
