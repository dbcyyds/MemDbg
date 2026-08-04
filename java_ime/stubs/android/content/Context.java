package android.content;
public class Context {
  public static final String DISPLAY_SERVICE = "display";
  public static final String INPUT_METHOD_SERVICE = "input_method";
  public static final String WINDOW_SERVICE = "window";
  public static final String NOTIFICATION_SERVICE = "notification";
  public static final int CONTEXT_INCLUDE_CODE = 1;
  public static final int CONTEXT_IGNORE_SECURITY = 2;
  public Object getSystemService(String n) { return null; }
  public Context createPackageContext(String p, int f) throws Exception { return null; }
  public void setTheme(int id) {}
  public String getPackageName() { return "android"; }
  public android.content.res.Resources getResources() { return new android.content.res.Resources(); }
}
