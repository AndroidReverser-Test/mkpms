public class DobbyJniFixture {
    public String text = "中文\u0000\ud83d\ude00";
    public int number = -123;
    public long wide = Long.MIN_VALUE + 17;
    public Object anything = new int[] {7, -8, Integer.MIN_VALUE};
    public byte[] bytes = {-128, 0, 127};
    public boolean[] booleans = {true, false};
    public char[] chars = {'\u0000', '\uffff'};
    public short[] shorts = {Short.MIN_VALUE, Short.MAX_VALUE};
    public long[] longs = {Long.MIN_VALUE, Long.MAX_VALUE};
    public float[] floats = {-1.25f, Float.NaN};
    public double[] doubles = {Math.PI, Double.POSITIVE_INFINITY};
    public Object[] nested = {text, null, new int[] {1, 2}};
    public Object[] cycle = new Object[1];
    public String malformed = "\ud800x\udc00";

    public DobbyJniFixture() { cycle[0] = cycle; }

    private static native long probe(String text, byte[] bytes, Object object,
                                     int number, float fraction, double precise, long wide);
    public static long invokeProbe() {
        DobbyJniFixture value = new DobbyJniFixture();
        return probe(value.text, value.bytes, value, -123, 1.25f, 2.5, 9007199254740993L);
    }

    private static native void runChecks(boolean shadow);
    public static void main(String[] args) {
        System.load(args[0]);
        runChecks(args.length > 1 && args[1].equals("shadow"));
    }
}
