package android.view;
public class ViewGroup extends View {
  public ViewGroup(android.content.Context c) { super(c); }
  public static class LayoutParams {
    public int width, height;
    public LayoutParams() {}
    public LayoutParams(int w, int h) { width=w; height=h; }
  }
}
