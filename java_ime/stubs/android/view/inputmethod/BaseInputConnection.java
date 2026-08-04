package android.view.inputmethod;
import android.view.KeyEvent;
import android.view.View;
public class BaseInputConnection implements InputConnection {
  public BaseInputConnection(View v, boolean full) {}
  public boolean commitText(CharSequence t, int n) { return false; }
  public boolean setComposingText(CharSequence t, int n) { return false; }
  public boolean finishComposingText() { return false; }
  public boolean deleteSurroundingText(int a, int b) { return false; }
  public CharSequence getTextBeforeCursor(int n, int f) { return ""; }
  public CharSequence getTextAfterCursor(int n, int f) { return ""; }
  public boolean performEditorAction(int a) { return false; }
  public boolean sendKeyEvent(KeyEvent e) { return false; }
}
