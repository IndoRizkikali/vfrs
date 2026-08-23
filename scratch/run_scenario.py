import subprocess
import time
import socket
import sys
import os

# Define absolute paths
VFRS_EXE = r"C:\Users\rizki\Programming\vfrns\vfr_switch\vfrs.exe"
VFRS_DIR = r"C:\Users\rizki\Programming\vfrns\vfr_switch"
DYNAMIPS_EXE = r"C:\Users\rizki\Virtualization\dynamips\dynamips.exe"
IMAGE_PATH = r"C:\Users\rizki\Virtualization\dynamips\c3660-a3jk9s-mz.124-15.T14.image"
R1_DIR = r"C:\Users\rizki\Virtualization\dynamips\R1"
R2_DIR = r"C:\Users\rizki\Virtualization\dynamips\R2"

def wake_and_handle_setup(port, duration):
    """Connects to the console port, wakes it up, and bypasses the setup wizard if it appears."""
    print(f"[{port}] Monitoring console for {duration}s...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        
        # Wait until port is open
        start_time = time.time()
        connected = False
        while time.time() - start_time < 15:
            try:
                s.connect(("127.0.0.1", port))
                connected = True
                break
            except Exception:
                time.sleep(1.0)
                
        if not connected:
            print(f"[{port}] Failed to connect to console port after 15s")
            return
            
        print(f"[{port}] Connected to console. Handling potential prompts...")
        s.sendall(b"\r\n")
        
        buffer = b""
        loop_start = time.time()
        while time.time() - loop_start < duration:
            try:
                data = s.recv(4096)
                if not data:
                    break
                clean_data = bytes([b for b in data if b < 128 or b == 10 or b == 13])
                sys.stdout.write(clean_data.decode("ascii", errors="ignore"))
                sys.stdout.flush()
                
                buffer += clean_data
                if b"initial configuration dialog" in buffer.lower():
                    print(f"\n[{port}] Setup dialog detected! Sending 'no'")
                    s.sendall(b"no\r\n")
                    buffer = b""
                    time.sleep(1.0)
                elif b"terminate autoinstall" in buffer.lower():
                    print(f"\n[{port}] Autoinstall detected! Sending 'yes'")
                    s.sendall(b"yes\r\n")
                    buffer = b""
                    time.sleep(1.0)
                elif b"press return to get started" in buffer.lower() or b"press enter to start" in buffer.lower() or b"press return to active" in buffer.lower():
                    s.sendall(b"\r\n")
                    buffer = b""
            except socket.timeout:
                s.sendall(b"\r\n")
                continue
            except Exception as e:
                print(f"[{port}] Error: {e}")
                break
        s.close()
    except Exception as e:
        print(f"[{port}] Monitoring error: {e}")

def run_telnet_cmds(port, commands):
    """Runs a list of telnet commands and returns the output."""
    output = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect(("127.0.0.1", port))
        time.sleep(1.0)
        s.sendall(b"\r\n")
        time.sleep(0.5)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
            
        for cmd in commands:
            output.append(f"\n--- Sending command: {cmd} ---")
            s.sendall(cmd.encode("ascii") + b"\r\n")
            # Wait longer for ping commands
            if "ping" in cmd:
                time.sleep(6.0)
            else:
                time.sleep(2.0)
            try:
                data = s.recv(8192)
                clean_data = bytes([b for b in data if b < 128 or b == 10 or b == 13])
                output.append(clean_data.decode("ascii", errors="ignore"))
            except socket.timeout:
                output.append("[Timeout reading output]")
        s.close()
    except Exception as e:
        output.append(f"Telnet failed on port {port}: {e}")
    return "\n".join(output)

def run_test(scenario_num):
    print(f"============================================================")
    print(f"STARTING TEST FOR SCENARIO {scenario_num}")
    print(f"============================================================")
    
    # Clean up any orphaned dynamips or vfrs processes in Windows
    try:
        subprocess.run(["taskkill", "/f", "/im", "dynamips.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["taskkill", "/f", "/im", "vfrs.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2.0)
    except Exception as e:
        print(f"Cleanup warning: {e}")
        
    config_file = f"test_slow_{scenario_num}.conf"
    log_name = f"vfrs_slow_{scenario_num}.log"
    
    # 1. Start VFRS
    print(f"Starting VFRS with config: {config_file}...")
    vfrs_proc = subprocess.Popen([VFRS_EXE, f".\\{config_file}"], cwd=VFRS_DIR)
    time.sleep(2.0)
    
    # 2. Start R1 (Dynamips)
    print("Starting R1 (Dynamips)...")
    r1_cmd = [
        DYNAMIPS_EXE, "-P", "3600", "-t", "3660", "--idle-pc", "0x60638578",
        "-i", "1", "-r", "192", "-m", "4268.8000.0100", "-T", "5000",
        "-p1:NM-4T", "-s1:0:udp:10001:127.0.0.1:10000", IMAGE_PATH
    ]
    r1_proc = subprocess.Popen(r1_cmd, cwd=R1_DIR)
    
    # Monitor R1 console for 60 seconds (boot and wait before R2 starts)
    wake_and_handle_setup(5000, 60)
    
    # 3. Start R2 (Dynamips)
    print("\nStarting R2 (Dynamips)...")
    r2_cmd = [
        DYNAMIPS_EXE, "-P", "3600", "-t", "3660", "--idle-pc", "0x60638578",
        "-i", "2", "-r", "192", "-m", "4268.8000.0200", "-T", "5001",
        "-p1:NM-4T", "-s1:0:udp:10003:127.0.0.1:10002", IMAGE_PATH
    ]
    r2_proc = subprocess.Popen(r2_cmd, cwd=R2_DIR)
    
    # Monitor R2 console for 60 seconds (boot and wait for LMI sync)
    wake_and_handle_setup(5001, 60)
    
    # 4. Wait another 45 seconds to ensure stabilization and Inverse ARP
    print("\nWaiting 45 seconds for stabilization and Inverse ARP...")
    time.sleep(45.0)
    
    # 5. Run Diagnostics
    print("\nRunning Diagnostics...")
    r1_cmds = ["enable", "terminal length 0", "show frame-relay lmi", "show frame-relay pvc", "show frame-relay map", "ping 10.10.10.2"]
    r2_cmds = ["enable", "terminal length 0", "show frame-relay lmi", "show frame-relay pvc", "show frame-relay map", "ping 10.10.10.1"]
    
    r1_diag = run_telnet_cmds(5000, r1_cmds)
    r2_diag = run_telnet_cmds(5001, r2_cmds)
    
    # 6. Terminate processes
    print("\nTerminating processes...")
    r1_proc.terminate()
    r2_proc.terminate()
    vfrs_proc.terminate()
    
    # Wait for clean termination
    time.sleep(3.0)
    
    # Force kill if still running
    try:
        r1_proc.kill()
    except Exception:
        pass
    try:
        r2_proc.kill()
    except Exception:
        pass
    try:
        vfrs_proc.kill()
    except Exception:
        pass
        
    print("\nProcesses terminated.")
    
    # 7. Print Router Outputs
    print(f"\n=================== R1 DIAGNOSTICS ===================")
    print(r1_diag)
    print(f"======================================================")
    
    print(f"\n=================== R2 DIAGNOSTICS ===================")
    print(r2_diag)
    print(f"======================================================")
    
    # 8. Print VFRS Log
    log_file_path = os.path.join(VFRS_DIR, log_name)
    if os.path.exists(log_file_path):
        print(f"\n=================== VFRS LOG (Last 40 lines) ===================")
        with open(log_file_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
            for line in lines[-40:]:
                print(line.strip())
        print(f"==============================================================")
    else:
        print(f"Log file not found: {log_file_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python run_scenario.py <scenario_num_1_2_3>")
        sys.exit(1)
    run_test(int(sys.argv[1]))
