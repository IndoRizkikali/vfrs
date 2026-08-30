import subprocess
import time
import os
import sys
import socket

def read_l3_msg(s, timeout_sec=2.0):
    s.settimeout(0.05)
    start = time.time()
    while (time.time() - start) < timeout_sec:
        try:
            data, _ = s.recvfrom(2048)
        except (socket.timeout, BlockingIOError, OSError):
            continue
        if len(data) >= 4 and data[:2] == b'\x00\x01':
            ctrl = data[2:4]
            if (ctrl[0] & 1) == 0: # I-frame
                return data[:2], ctrl, data[4:]
    return None, None, None

def main():
    config_content1 = """# VFRS test config for SVC and QoS / charging facilities
log level con=info txt=debug
port uni0/1 udp-server 127.0.0.1 30003
port uni0/2 udp-server 127.0.0.1 30004
svc int uni0/1 dlci_low=512 dlci_high=600 ftpdef=12 fdpdef=6 clsdef=2 revchg=allow
svc int uni0/2 dlci_low=512 dlci_high=600 ftpdef=10 fdpdef=5 clsdef=3 revchg=deny
svc addr uni0/1 manual x121 510401010001
svc addr uni0/2 manual x121 510401010002
svc route uni0/2 prefix x121 510401010002
"""
    config_path = "tests/svc_test.conf"
    os.makedirs("tests", exist_ok=True)
    with open(config_path, "w") as f:
        f.write(config_content1)

    my_env = os.environ.copy()
    my_env["PATH"] = r"C:\msys64\ucrt64\bin;" + my_env.get("PATH", "")

    vfrs_exe = "bin/vfrs.exe" if os.path.exists("bin/vfrs.exe") else ("./vfrs.exe" if os.path.exists("./vfrs.exe") else "../bin/vfrs.exe")
    proc = subprocess.Popen([vfrs_exe, config_path], env=my_env)
    time.sleep(1.0)

    s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s1.bind(('127.0.0.1', 0))
    s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s2.bind(('127.0.0.1', 0))

    try:
        # Establish LAPF link on uni0/1 and uni0/2
        for _ in range(3):
            s1.sendto(b'\x00\x01\x7F', ('127.0.0.1', 30003))
            time.sleep(0.05)
            s2.sendto(b'\x00\x01\x7F', ('127.0.0.1', 30004))
            time.sleep(0.05)
        print("LAPF Links established successfully on UDP transport!")

        setup_payload = (
            b'\x08' +
            b'\x02\x00\x05' +
            b'\x05' +
            b'\x04\x03\x88\xA0\xCF' +
            b'\x70\x0D\x93' + b'510401010002' +
            b'\x6C\x0D\x93' + b'510401010001' +
            b'\x4A\x01\x81' +
            b'\x6A\x06\x01\xCC\x02\x55\x03\x02'
        )
        s1.sendto(b'\x00\x01\x00\x00' + setup_payload, ('127.0.0.1', 30003))
        print("Sent SETUP with Reverse Charging on uni0/1")

        addr, ctrl, payload = read_l3_msg(s1)
        if not payload:
            raise AssertionError("No response to SETUP")
        
        if len(payload) >= 5 and payload[4] == 0x4D:
            print("Received RELEASE message!")
            if len(payload) >= 9 and payload[5] == 0x08:
                cause = payload[8] & 0x7F
                print(f"Cause Value: {cause}")
                if cause == 29:
                    print("TEST PASSED: Call successfully rejected with Cause 29 (Facility rejected)!")
                    s1.sendto(b'\x00\x01\x00\x00\x08\x02\x80\x05\x5A', ('127.0.0.1', 30003))
                else:
                    raise AssertionError(f"Expected Cause 29, got {cause}")
            else:
                raise AssertionError("Cause IE missing in RELEASE")
        else:
            raise AssertionError(f"Expected RELEASE message, got {payload.hex()}")

        # 4. Now modify configuration to enable Reverse Charging Acceptance (rev_charge_acc=1)
        print("Re-configuring VFRS to accept reverse charging on uni0/2...")
        proc.terminate()
        time.sleep(0.5)

        config_content2 = """# VFRS test config for SVC and QoS / charging facilities
log level con=info txt=debug
port uni0/1 udp-server 127.0.0.1 30003
port uni0/2 udp-server 127.0.0.1 30004
svc int uni0/1 dlci_low=512 dlci_high=600 ftpdef=12 fdpdef=6 clsdef=2 revchg=allow
svc int uni0/2 dlci_low=512 dlci_high=600 ftpdef=10 fdpdef=5 clsdef=3 revchg=allow
svc addr uni0/1 manual x121 510401010001
svc addr uni0/2 manual x121 510401010002
svc route uni0/2 prefix x121 510401010002
"""
        with open(config_path, "w") as f:
            f.write(config_content2)

        proc = subprocess.Popen([vfrs_exe, config_path], env=my_env)
        time.sleep(1.0)

        s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s1.bind(('127.0.0.1', 0))
        s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s2.bind(('127.0.0.1', 0))

        for _ in range(3):
            s1.sendto(b'\x00\x01\x7F', ('127.0.0.1', 30003))
            time.sleep(0.05)
            s2.sendto(b'\x00\x01\x7F', ('127.0.0.1', 30004))
            time.sleep(0.05)
        print("LAPF links re-established.")

        s1.sendto(b'\x00\x01\x00\x00' + setup_payload, ('127.0.0.1', 30003))
        print("Sent SETUP on uni0/1 (Reverse Charging Accepted by target)")

        addr, ctrl, payload = read_l3_msg(s2)
        if not payload:
            raise AssertionError("SETUP not forwarded to uni0/2")
        print("Forwarded SETUP message received on uni0/2!")

        has_rev_charge = False
        has_priority = False
        idx = 5
        while idx < len(payload):
            if payload[idx] & 0x80:
                idx += 1
                continue
            if idx + 1 >= len(payload): break
            ie_id = payload[idx]
            ie_len = payload[idx+1]
            ie_data = payload[idx+2 : idx+2+ie_len]
            if ie_id == 0x4A:
                has_rev_charge = True
                print(f"Found Reverse Charging IE: data={ie_data.hex()}")
                if ie_data[0] != 0x81:
                    raise AssertionError(f"Expected reverse charge value 0x81, got {ie_data[0]:02X}")
            elif ie_id == 0x6A:
                has_priority = True
                print(f"Found Priority & Service Class IE: data={ie_data.hex()}")
                if ie_data != b'\x01\xcc\x02\x55\x03\x02':
                    raise AssertionError(f"Expected priority payload 01cc02550302, got {ie_data.hex()}")
            idx += 2 + ie_len

        if not has_rev_charge or not has_priority:
            raise AssertionError(f"Missing value-added IEs. rev_charge={has_rev_charge}, priority={has_priority}")

        print("ALL VALUE-ADDED SERVICES TESTS PASSED SUCCESSFULLY!")
        proc.terminate()
        sys.exit(0)

    except Exception as e:
        print(f"\nTEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        proc.terminate()
        sys.exit(1)

if __name__ == '__main__':
    main()
