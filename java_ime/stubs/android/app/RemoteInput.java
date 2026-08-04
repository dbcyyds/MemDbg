package android.app;
import android.content.Intent;
import android.os.Bundle;
public class RemoteInput {
  public static class Builder {
    public Builder(String resultKey) {}
    public Builder setLabel(CharSequence l) { return this; }
    public RemoteInput build() { return new RemoteInput(); }
  }
  public static Bundle getResultsFromIntent(Intent i) { return null; }
}
