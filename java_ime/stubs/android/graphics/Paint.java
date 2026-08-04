package android.graphics;
public class Paint {
  public static final int ANTI_ALIAS_FLAG = 1;
  public enum Style { FILL, STROKE, FILL_AND_STROKE }
  public enum Align { LEFT, CENTER, RIGHT }
  public Paint() {}
  public Paint(int f) {}
  public void setColor(int c) {}
  public void setTextSize(float s) {}
  public void setStyle(Style s) {}
  public void setStrokeWidth(float w) {}
  public void setTextAlign(Align a) {}
  public float measureText(String t) { return t==null?0:t.length()*12f; }
}
