package android.os;
public class Looper {
  public static void prepareMainLooper() {}
  public static Looper getMainLooper() { return new Looper(); }
  public static void loop() {}
}
