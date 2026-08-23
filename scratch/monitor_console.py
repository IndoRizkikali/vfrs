import socket
import sys
import time

def monitor(port, duration=30):
    print(f"Connecting to 127.0.0.1:{port}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(("127.0.0.1", port))
        print("Connected. Reading console output...")
        
        start_time = time.time()
        s.sendall(b"\r\n") # Send initial return to wake up console
        
        buffer = b""
        while time.time() - start_time < duration:
            try:
                data = s.recv(4096)
                if not data:
                    print("\nConnection closed by remote host.")
                    break
                # Filter out telnet options (IAC commands)
                clean_data = bytes([b for b in data if b < 128 or b == 10 or b == 13])
                sys.stdout.write(clean_data.decode("ascii", errors="ignore"))
                sys.stdout.flush()
                
                buffer += clean_data
                if b"initial configuration dialog" in buffer.lower():
                    print("\n[Detected setup dialog, sending 'no']")
                    s.sendall(b"no\r\n")
                    buffer = b""
                    time.sleep(1.0)
                elif b"terminate autoinstall" in buffer.lower():
                    print("\n[Detected autoinstall, sending 'yes']")
                    s.sendall(b"yes\r\n")
                    buffer = b""
                    time.sleep(1.0)
                elif b"press return to get started" in buffer.lower() or b"press enter to start" in buffer.lower() or b"press return to active" in buffer.lower():
                    s.sendall(b"\r\n")
                    buffer = b""
            except socket.timeout:
                # periodically send newline to wake up
                s.sendall(b"\r\n")
                continue
            except Exception as e:
                print(f"\nError: {e}")
                break
        s.close()
    except Exception as e:
        print(f"Failed to connect: {e}")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    monitor(port, duration)
