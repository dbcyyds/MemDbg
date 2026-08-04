package android.view.inputmethod;
import android.view.KeyEvent;
public interface InputConnection {
  boolean commitText(CharSequence t, int n);
  boolean setComposingText(CharSequence t, int n);
  boolean finishComposingText();
  boolean deleteSurroundingText(int a, int b);
  CharSequence getTextBeforeCursor(int n, int f);
  CharSequence getTextAfterCursor(int n, int f);
  boolean performEditorAction(int a);
  boolean sendKeyEvent(KeyEvent e);
}
