package android.view.inputmethod;
import android.view.View;
public class InputMethodManager {
  public static final int SHOW_FORCED = 2;
  public boolean showSoftInput(View v, int f) { return true; }
  public void toggleSoftInput(int a, int b) {}
  public void restartInput(View v) {}
  public boolean hideSoftInputFromWindow(Object token, int f) { return true; }
}
