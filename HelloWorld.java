public class HelloWorld{

	private static int DELAY = 1000;

	public static void main() {
		while (true) {			
			Native.setLed(true);
			for (int i = 0; i < DELAY; i++);
			
			Native.setLed(false);
			for (int i = 0; i < DELAY; i++);
		}
	}
}