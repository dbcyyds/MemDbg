package android.app;
import android.media.AudioAttributes;
import android.net.Uri;
public class NotificationChannel {
  public NotificationChannel(String id, CharSequence name, int importance) {}
  public void setDescription(String d) {}
  public void setSound(Uri s, AudioAttributes a) {}
  public void enableVibration(boolean b) {}
}
