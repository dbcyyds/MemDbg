package android.app;
import android.content.Context;
import android.content.Intent;
public class PendingIntent {
  public static final int FLAG_UPDATE_CURRENT = 0x8000000;
  public static final int FLAG_MUTABLE = 0x2000000;
  public static final int FLAG_IMMUTABLE = 0x4000000;
  public static PendingIntent getBroadcast(Context c, int r, Intent i, int f) { return new PendingIntent(); }
}
