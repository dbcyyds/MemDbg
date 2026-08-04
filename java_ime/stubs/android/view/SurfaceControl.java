package android.view;
import android.graphics.Rect;
import android.os.IBinder;
public class SurfaceControl {
  public SurfaceControl() {}
  public void release() {}
  public static class Builder {
    public Builder() {}
    public Builder setName(String n) { return this; }
    public Builder setContainerLayer() { return this; }
    public Builder setCallsite(String s) { return this; }
    public Builder setHidden(boolean h) { return this; }
    public Builder setBufferSize(int w, int h) { return this; }
    public Builder setFormat(int f) { return this; }
    public Builder setParent(SurfaceControl p) { return this; }
    public SurfaceControl build() { return new SurfaceControl(); }
  }
  public static class Transaction {
    public Transaction() {}
    public Transaction setLayer(SurfaceControl sc, int z) { return this; }
    public Transaction setPosition(SurfaceControl sc, float x, float y) { return this; }
    public Transaction setLayerStack(SurfaceControl sc, int s) { return this; }
    public Transaction setCrop(SurfaceControl sc, Rect r) { return this; }
    public Transaction show(SurfaceControl sc) { return this; }
    public Transaction reparent(SurfaceControl sc, SurfaceControl p) { return this; }
    public void apply() {}
  }
}
