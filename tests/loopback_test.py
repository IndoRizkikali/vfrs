import subprocess
import time
import os
import sys

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc ^ 0xFFFF

def encode_address(dlci):
    b1 = ((dlci >> 4) & 0x3F) << 2
    b2 = ((dlci & 0x0F) << 4) | 0x01
    return bytes([b1, b2])

def decode_address(addr_bytes):
    if len(addr_bytes) < 2:
        return 0
    dlci = (((addr_bytes[0] & 0xFC) >> 2) << 4) | ((addr_bytes[1] & 0xF0) >> 4)
    return dlci

def make_frame(dlci, payload):
    addr = encode_address(dlci)
    body = addr + payload
    c = crc16(body)
    crc_bytes = bytes([c & 0xff, (c >> 8) & 0xff])
    return b'\x7e' + body + crc_bytes + b'\x7e'

def main():
    config_content = """port uni0/1 pipe-server vfrs_test_pipe_1
port uni0/2 pipe-server vfrs_test_pipe_2
pvc uni0/1 100 uni0/2 200
"""
    config_path = "tests/loopback.conf"
    os.makedirs("tests", exist_ok=True)
    with open(config_path, "w") as f:
        f.write(config_content)

    print("Starting VFRS process...")
    # Enable console thread in VFRS by passing "-c" and redirecting stdin/stdout
    proc = subprocess.Popen(
        ["./vfrs.exe", "-c", config_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    time.sleep(1.5) # Wait for VFRS to start and create named pipes

    # Check if the process exited early
    retcode = proc.poll()
    if retcode is not None:
        print(f"VFRS process exited prematurely with code {retcode}")
        stdout, stderr = proc.communicate()
        print(f"STDOUT:\n{stdout}")
        print(f"STDERR:\n{stderr}")
        sys.exit(1)

    pipe1_path = r"\\.\pipe\vfrs_test_pipe_1"
    pipe2_path = r"\\.\pipe\vfrs_test_pipe_2"

    print("Opening named pipes...")
    try:
        # Open client connections to the named pipes
        p1 = open(pipe1_path, "r+b", buffering=0)
        p2 = open(pipe2_path, "r+b", buffering=0)
    except Exception as e:
        print(f"Failed to open named pipes: {e}")
        proc.terminate()
        stdout, stderr = proc.communicate()
        print(f"STDOUT:\n{stdout}")
        print(f"STDERR:\n{stderr}")
        sys.exit(1)

    print("Sending test frames...")
    payloads = [b"Hello VFRS Frame 1", b"Test Packet Number 2", b"Final Check Packet 3"]
    for i, payload in enumerate(payloads):
        f = make_frame(100, payload)
        p1.write(f)
        print(f"Sent Frame {i+1} on uni0/1 (DLCI 100)")

    print("Reading and verifying response frames...")
    # Read response frames from pipe2
    # The reader thread byte-by-byte reads until closing 0x7E flag
    for i, expected_payload in enumerate(payloads):
        buf = bytearray()
        flag_count = 0
        while flag_count < 2:
            b = p2.read(1)
            if not b:
                break
            if b == b'\x7e':
                flag_count += 1
                if flag_count == 1:
                    buf.append(0x7e)
            else:
                if flag_count >= 1:
                    buf.append(b[0])
        buf.append(0x7e)

        # Parse frame
        if len(buf) < 6:
            print(f"Error: Frame {i+1} too short: {buf.hex()}")
            proc.terminate()
            stdout, stderr = proc.communicate()
            print(f"STDOUT:\n{stdout}")
            print(f"STDERR:\n{stderr}")
            sys.exit(1)

        addr_bytes = buf[1:3]
        rcvd_dlci = decode_address(addr_bytes)
        rcvd_payload = bytes(buf[3:-3])
        rcvd_crc = int.from_bytes(buf[-3:-1], byteorder='little')
        calc_crc = crc16(bytes(buf[1:-3]))

        print(f"Received Frame {i+1}: DLCI={rcvd_dlci}, Payload={rcvd_payload.decode(errors='replace')}, CRC OK={rcvd_crc == calc_crc}")

        if rcvd_dlci != 200:
            print(f"Error: Frame {i+1} expected DLCI 200, got {rcvd_dlci}")
            p1.close()
            p2.close()
            proc.terminate()
            sys.exit(1)

        if rcvd_payload != expected_payload:
            print(f"Error: Frame {i+1} expected payload '{expected_payload}', got '{rcvd_payload}'")
            p1.close()
            p2.close()
            proc.terminate()
            sys.exit(1)

        if rcvd_crc != calc_crc:
            print(f"Error: Frame {i+1} CRC check failed")
            p1.close()
            p2.close()
            proc.terminate()
            sys.exit(1)

    print("All test frames successfully verified!")
    
    # Final cleanup and print stdout/stderr
    p1.close()
    p2.close()

    # Send "exit" command to console to shut down VFRS gracefully
    try:
        proc.stdin.write("exit\n")
        proc.stdin.flush()
    except Exception as e:
        print(f"Failed to write exit command: {e}")
    proc.stdin.close()
    
    try:
        stdout, stderr = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        print("VFRS did not exit in time, terminating process...")
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            
    print(f"VFRS Console Output:\n{stdout}")
    print(f"VFRS Error Output:\n{stderr}")

    # Clean config file
    if os.path.exists(config_path):
        os.remove(config_path)

    sys.exit(0)

if __name__ == "__main__":
    main()
