package android.view;
public interface WindowManager {
  class LayoutParams extends ViewGroup.LayoutParams {
    public static final int TYPE_APPLICATION = 2;
    public static final int FLAG_NOT_TOUCH_MODAL = 0x20;
    public static final int FLAG_LAYOUT_NO_LIMITS = 0x200;
    public static final int FLAG_HARDWARE_ACCELERATED = 0x1000000;
    public static final int FLAG_KEEP_SCREEN_ON = 0x80;
    public static final int FLAG_LAYOUT_IN_SCREEN = 0x100;
    public static final int SOFT_INPUT_STATE_ALWAYS_VISIBLE = 5;
    public static final int SOFT_INPUT_ADJUST_RESIZE = 0x10;
    public static final int TYPE_SYSTEM_ALERT = 2003;
    public int type, flags, gravity, softInputMode, format, y;
    public LayoutParams() { super(-1,-2); }
    public LayoutParams(int w, int h, int type, int flags, int format) {
      super(w,h); this.type=type; this.flags=flags; this.format=format;
    }
  }
}
