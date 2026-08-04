package android.view;
import android.content.Context;
import android.graphics.Canvas;
public class View {
  public static class MeasureSpec {
    public static int getSize(int m) { return m & 0xffffff; }
  }
  public View(Context c) {}
  public void invalidate() {}
  public boolean requestFocus() { return true; }
  public void setFocusable(boolean b) {}
  public void setFocusableInTouchMode(boolean b) {}
  public void setClickable(boolean b) {}
  public boolean post(Runnable r) { if (r!=null) r.run(); return true; }
  public boolean postDelayed(Runnable r, long d) { return true; }
  public Object getWindowToken() { return null; }
  public Context getContext() { return null; }
  public boolean onCheckIsTextEditor() { return false; }
  public android.view.inputmethod.InputConnection onCreateInputConnection(android.view.inputmethod.EditorInfo o) { return null; }
  protected void onDraw(Canvas c) {}
  protected void onMeasure(int w, int h) {}
  public void setMeasuredDimension(int w, int h) {}
  public int getWidth() { return 1080; }
  public int getHeight() { return 500; }
  public boolean onTouchEvent(MotionEvent e) { return false; }
}
