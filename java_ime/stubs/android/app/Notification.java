package android.app;
import android.content.Context;
public class Notification {
  public static final int PRIORITY_HIGH = 1;
  public static final String CATEGORY_MESSAGE = "msg";
  public static class Builder {
    public Builder(Context c) {}
    public Builder(Context c, String ch) {}
    public Builder setContentTitle(CharSequence t) { return this; }
    public Builder setContentText(CharSequence t) { return this; }
    public Builder setSmallIcon(int id) { return this; }
    public Builder setOngoing(boolean b) { return this; }
    public Builder setAutoCancel(boolean b) { return this; }
    public Builder setPriority(int p) { return this; }
    public Builder setCategory(String c) { return this; }
    public Builder addAction(Action a) { return this; }
    public Notification build() { return new Notification(); }
  }
  public static class Action {
    public static class Builder {
      public Builder(int icon, CharSequence title, PendingIntent intent) {}
      public Builder addRemoteInput(RemoteInput r) { return this; }
      public Action build() { return new Action(); }
    }
  }
}
