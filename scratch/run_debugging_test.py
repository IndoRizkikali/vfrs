import subprocess
import time
import socket
import sys
import os
import threading

# Define absolute paths
VFRS_EXE = r"C:\Users\rizki\Programming\vfrns\vfr_switch\vfrs.exe"
VFRS_DIR = r"C:\Users\rizki\Programming\vfrns\vfr_switch"
DYNAMIPS_EXE = r"C:\Users\rizki\Virtualization\dynamips\dynamips.exe"
IMAGE_PATH = r"C:\Users\rizki\Virtualization\dynamips\c3660-a3jk9s-mz.124-15.T14.image"
R1_DIR = r"C:\Users\rizki\Virtualization\dynamips\R1"
R2_DIR = r"C:\Users\rizki\Virtualization\dynamips\R2"

# Global log files
r1_log_file = open(os.path.join(VFRS_DIR, "r1_console.log"), "w", encoding="utf-8")
r2_log_file = open(os.path.join(VFRS_DIR, "r2_console.log"), "w", encoding="utf-8")

stop_threads = False

def console_reader(port, log_file, label):
    """Continuously reads from console port and writes to a file and stdout."""
    global stop_threads
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(1.0)
        s.connect(("127.0.0.1", port))
        
        # Non-blocking-ish loop
        while not stop_threads:
            try:
                data = s.recv(4096)
                if not data:
                    break
                clean_data = bytes([b for b in data if b < 128 or b == 10 or b == 13])
                decoded = clean_data.decode("ascii", errors="ignore")
                
                # Write to file
                log_file.write(decoded)
                log_file.flush()
                
                # Write to stdout with prefix
                for line in decoded.splitlines():
                    if line.strip():
                        print(f"[{label}] {line}")
            except socket.timeout:
                continue
            except Exception as e:
                print(f"[{label}] Error: {e}")
                break
        s.close()
    except Exception as e:
        print(f"[{label}] Reader thread failed to connect: {e}")

def run_telnet_cmds(port, commands, wait_pings=False):
    """Sends a list of commands over a one-off telnet connection."""
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

def wait_for_prompt(port, timeout=180):
    """Waits for the router to boot and responds to setup dialogs."""
    print(f"Waiting for prompt on 127.0.0.1:{port}...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    
    connected = False
    start_time = time.time()
    while time.time() - start_time < 90:
        try:
            s.connect(("127.0.0.1", port))
            connected = True
            break
        except Exception:
            time.sleep(1.0)
            
    if not connected:
        print(f"[{port}] Failed to connect to port")
        return False
        
    s.sendall(b"\r\n")
    buffer = b""
    loop_start = time.time()
    
    while time.time() - loop_start < timeout:
        try:
            data = s.recv(4096)
            if not data:
                break
            clean_data = bytes([b for b in data if b < 128 or b == 10 or b == 13])
            buffer += clean_data
            
            if b"initial configuration dialog" in buffer.lower():
                print(f"[{port}] Sending 'no' to configuration dialog")
                s.sendall(b"no\r\n")
                buffer = b""
            elif b"terminate autoinstall" in buffer.lower():
                print(f"[{port}] Sending 'yes' to terminate autoinstall")
                s.sendall(b"yes\r\n")
                buffer = b""
            elif b"press return to get started" in buffer.lower() or b"press enter to start" in buffer.lower():
                s.sendall(b"\r\n")
                buffer = b""
            elif b"router>" in buffer.lower() or b"router#" in buffer.lower():
                print(f"[{port}] Prompt reached!")
                s.close()
                return True
        except socket.timeout:
            s.sendall(b"\r\n")
            continue
        except Exception as e:
            print(f"[{port}] Error waiting: {e}")
            break
    s.close()
    return False

def main():
    global stop_threads
    
    if len(sys.argv) < 2:
        print("Usage: python run_debugging_test.py <scenario_num_1_2_3>")
        sys.exit(1)
        
    scenario_num = int(sys.argv[1])
    config_file = f"test_slow_{scenario_num}.conf"
    log_name = f"vfrs_slow_{scenario_num}.log"
    r1_log_name = f"r1_console_slow_{scenario_num}.log"
    r2_log_name = f"r2_console_slow_{scenario_num}.log"
    
    global r1_log_file, r2_log_file
    r1_log_file = open(os.path.join(VFRS_DIR, r1_log_name), "w", encoding="utf-8")
    r2_log_file = open(os.path.join(VFRS_DIR, r2_log_name), "w", encoding="utf-8")
    
    # 1. Process Cleanup
    print("Cleaning up existing processes...")
    subprocess.run(["taskkill", "/f", "/im", "dynamips.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["taskkill", "/f", "/im", "vfrs.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.0)
    
    # Delete stale lock files
    for lock_path in [os.path.join(R1_DIR, "c3600_i1_lock"), os.path.join(R2_DIR, "c3600_i2_lock")]:
        if os.path.exists(lock_path):
            try:
                os.remove(lock_path)
                print(f"Removed stale lock file: {lock_path}")
            except Exception as e:
                print(f"Warning: could not remove lock file {lock_path}: {e}")
    
    # 2. Start VFRS
    print(f"Starting VFRS with config {config_file}...")
    vfrs_proc = subprocess.Popen([VFRS_EXE, f".\\{config_file}"], cwd=VFRS_DIR)
    time.sleep(2.0)
    
    # 3. Start R1
    print("Starting R1 (Dynamips)...")
    r1_cmd = [
        DYNAMIPS_EXE, "-P", "3600", "-t", "3660", "--idle-pc", "0x60638578",
        "-i", "1", "-r", "192", "-m", "4268.8000.0100", "-T", "5000",
        "-p1:NM-4T", "-s1:0:udp:10001:127.0.0.1:10000", IMAGE_PATH
    ]
    r1_proc = subprocess.Popen(r1_cmd, cwd=R1_DIR)
    
    # Wait for R1 to boot
    if not wait_for_prompt(5000):
        print("R1 failed to reach prompt. Exiting.")
        r1_proc.terminate()
        vfrs_proc.terminate()
        sys.exit(1)
        
    # Enable debugging on R1
    print("Enabling debugging on R1...")
    debug_cmds_r1 = [
        "enable",
        "terminal length 0",
        "debug frame-relay verbose",
        "debug frame-relay lmi",
        "debug frame-relay event"
    ]
    run_telnet_cmds(5000, debug_cmds_r1)
    
    # Start background thread to capture R1 output
    r1_thread = threading.Thread(target=console_reader, args=(5000, r1_log_file, "R1"))
    r1_thread.daemon = True
    r1_thread.start()
    
    # Wait 60 seconds before booting R2
    print("Waiting 60 seconds before starting R2...")
    time.sleep(60.0)
    
    # 4. Start R2
    print("Starting R2 (Dynamips)...")
    r2_cmd = [
        DYNAMIPS_EXE, "-P", "3600", "-t", "3660", "--idle-pc", "0x60638578",
        "-i", "2", "-r", "192", "-m", "4268.8000.0200", "-T", "5001",
        "-p1:NM-4T", "-s1:0:udp:10003:127.0.0.1:10002", IMAGE_PATH
    ]
    r2_proc = subprocess.Popen(r2_cmd, cwd=R2_DIR)
    
    # Wait for R2 to boot
    if not wait_for_prompt(5001):
        print("R2 failed to reach prompt. Exiting.")
        r1_proc.terminate()
        r2_proc.terminate()
        vfrs_proc.terminate()
        sys.exit(1)
        
    # Enable debugging on R2
    print("Enabling debugging on R2...")
    debug_cmds_r2 = [
        "enable",
        "terminal length 0",
        "debug frame-relay verbose",
        "debug frame-relay lmi",
        "debug frame-relay event"
    ]
    run_telnet_cmds(5001, debug_cmds_r2)
    
    # Start background thread to capture R2 output
    r2_thread = threading.Thread(target=console_reader, args=(5001, r2_log_file, "R2"))
    r2_thread.daemon = True
    r2_thread.start()
    
    # Let both DTEs run concurrently for 150 seconds to complete multiple LMI and Full Status cycles
    print("Letting both DTEs run concurrently for 150 seconds (LMI synchronization & Full Status exchanges)...")
    time.sleep(150.0)
    
    # Check current status before shutdown
    print("\n--- Diagnostic Check BEFORE shutdown ---")
    diag_before = run_telnet_cmds(5000, ["enable", "show frame-relay pvc", "show frame-relay map", "ping 10.10.10.2"])
    print(diag_before)
    
    # 5. Shutdown R2 interface
    print("\n--- Shutting down R2 interface Serial1/0 ---")
    shutdown_cmds = [
        "enable",
        "configure terminal",
        "interface Serial1/0",
        "shutdown",
        "end"
    ]
    run_telnet_cmds(5001, shutdown_cmds)
    
    # Wait dynamically depending on the scenario to allow LMI status propagation.
    # Failure detection time is N392 * T392 (60s for Scenarios 1/2, 135s for Scenario 3).
    # Plus up to 60s for R1's next periodic Full Status poll (N391 = 6 polls) to pull the update.
    # Total wait: 130s for Scenarios 1/2, 230s for Scenario 3.
    wait_time = 130.0 if scenario_num != 3 else 230.0
    print(f"Waiting {wait_time} seconds for status propagation...")
    time.sleep(wait_time)
    
    # 6. Check R1 status after R2 shutdown
    print("\n--- Diagnostic Check AFTER R2 shutdown (Expect DLCI 100 to be INACTIVE) ---")
    diag_after = run_telnet_cmds(5000, ["enable", "show frame-relay pvc", "show frame-relay map", "ping 10.10.10.2"])
    print(diag_after)
    
    # 7. Cleanup processes
    print("\nTerminating processes...")
    stop_threads = True
    time.sleep(2.0)
    
    r1_proc.terminate()
    r2_proc.terminate()
    vfrs_proc.terminate()
    time.sleep(3.0)
    
    # Force kill
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
        
    r1_log_file.close()
    r2_log_file.close()
    print("Test run completed successfully.")

if __name__ == "__main__":
    main()
