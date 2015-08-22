import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;

public class m {

    public static void main(String[] args) throws IOException {
        byte[] bytes = Files.readAllBytes(Paths.get(new File("C:\\Users\\user\\workspace_arm\\JavaSTM32\\HelloWorld.class").toURI()));
        
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) {
            sb.append(String.format("0x%02X", b));
            sb.append(", ");
        }
        
        Files.write(Paths.get(new File("array.txt").toURI()), sb.toString().getBytes(), StandardOpenOption.CREATE);
		
		System.out.println("Array is generated");
    }
}