package android.view.inputmethod;
public class EditorInfo {
  public int inputType, imeOptions;
  public CharSequence hintText;
  public static final int IME_ACTION_DONE = 6;
  public static final int IME_ACTION_GO = 2;
  public static final int IME_ACTION_SEARCH = 3;
  public static final int IME_ACTION_SEND = 4;
  public static final int IME_ACTION_NEXT = 5;
  public static final int IME_FLAG_NO_FULLSCREEN = 0x2000000;
}
