package android.view;
import android.content.Context;
import android.window.InputTransferToken;
public class SurfaceControlViewHost {
  public SurfaceControlViewHost(Context c, Display d, WindowlessWindowManager w, String s) {}
  public SurfaceControlViewHost(Context c, Display d, InputTransferToken t) {}
  public SurfaceControlViewHost(Context c, Display d, android.os.IBinder b) {}
  public void setView(View v, int w, int h) {}
  public void setView(View v, WindowManager.LayoutParams lp) {}
  public View getView() { return null; }
  public void release() {}
}
