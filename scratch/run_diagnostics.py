import socket
import sys
import time

def send_cmds(port, commands):
    print(f"=== Diagnostics on Port {port} ===")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect(("127.0.0.1", port))
        time.sleep(1.0)
        s.sendall(b"\r\n")
        time.sleep(0.5)
        
        # Read greeting
        try:
            data = s.recv(4096)
        except socket.timeout:
            pass
            
        for cmd in commands:
            print(f"\nSending: {cmd}")
            s.sendall(cmd.encode("ascii") + b"\r\n")
            time.sleep(2.0)
            try:
                data = s.recv(8192)
                clean_data = bytes([b for b in data if b < 128 or b == 10 or b == 13])
                print(clean_data.decode("ascii", errors="ignore"), end="")
            except socket.timeout:
                print("[Timeout reading output]")
        s.close()
    except Exception as e:
        print(f"Failed: {e}")
    print("\n" + "="*40 + "\n")

if __name__ == "__main__":
    r1_cmds = [
        "enable",
        "show frame-relay pvc",
        "show frame-relay pvc 100",
        "show frame-relay pvc 101",
        "show frame-relay map",
        "ping 10.10.10.2"
    ]
    r2_cmds = [
        "enable",
        "show frame-relay pvc",
        "show frame-relay pvc 100",
        "show frame-relay map",
        "ping 10.10.10.1"
    ]
    
    # Run diagnostics on R1
    send_cmds(5000, r1_cmds)
    
    # Run diagnostics on R2
    send_cmds(5001, r2_cmds)
