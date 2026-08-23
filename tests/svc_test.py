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

def make_frame(addr_bytes, control_bytes, payload=b""):
    body = addr_bytes + control_bytes + payload
    c = crc16(body)
    crc_bytes = bytes([c & 0xff, (c >> 8) & 0xff])
    return b'\x7e' + body + crc_bytes + b'\x7e'

def read_one_frame(pipe):
    buf = bytearray()
    flag_count = 0
    while flag_count < 2:
        b = pipe.read(1)
        if not b:
            break
        if b == b'\x7e':
            flag_count += 1
            if flag_count == 1:
                buf.clear()
            else:
                return bytes(buf)
        else:
            if flag_count == 1:
                buf.append(b[0])
    return None

def parse_frame(frame_bytes):
    if len(frame_bytes) < 4:
        return None, None, None
    body = frame_bytes[:-2]
    crc_received = int.from_bytes(frame_bytes[-2:], 'little')
    if crc16(body) != crc_received:
        raise ValueError("CRC Error")
    
    # 2-byte Address
    addr = body[:2]
    # Check control byte
    ctrl = body[2:3]
    if (body[2] & 1) == 0:
        # I-frame has 2-byte control
        ctrl = body[2:4]
        payload = body[4:]
    else:
        payload = body[3:]
    return addr, ctrl, payload

def main():
    config_content = """# VFRS test config for SVC and QoS / charging facilities
port uni0/1 pipe-server vfrs_svc_pipe_1
port uni0/2 pipe-server vfrs_svc_pipe_2
svc_int uni0/1 dlci_low=512 dlci_high=600 default_ftp=12 default_fdp=6 default_svc_class=2
svc_int uni0/2 dlci_low=512 dlci_high=600 default_ftp=10 default_fdp=5 default_svc_class=3
svc_addr uni0/1 x121 510401010001 rev_charge_acc=1 rev_charge_prev=0
svc_addr uni0/2 x121 510401010002 rev_charge_acc=0 rev_charge_prev=0
svc_route 510401010002 uni0/2
"""
    config_path = "tests/svc_test.conf"
    os.makedirs("tests", exist_ok=True)
    with open(config_path, "w") as f:
        f.write(config_content)

    my_env = os.environ.copy()
    my_env["PATH"] = r"C:\msys64\ucrt64\bin;" + my_env.get("PATH", "")

    proc = subprocess.Popen(
        ["./vfrs.exe", config_path],
        env=my_env
    )
    time.sleep(1.5)

    pipe1_path = r"\\.\pipe\vfrs_svc_pipe_1"
    pipe2_path = r"\\.\pipe\vfrs_svc_pipe_2"

    try:
        p1 = open(pipe1_path, "r+b", buffering=0)
        p2 = open(pipe2_path, "r+b", buffering=0)
    except Exception as e:
        print(f"Failed to open named pipes: {e}")
        proc.terminate()
        sys.exit(1)

    # 1. Establish LAPF link on uni0/1: Send SABME (Control 0x7F)
    # User Command DLCI 0 Address = [0x00, 0x01]
    sabme_frame = make_frame(b'\x00\x01', b'\x7F')
    p1.write(sabme_frame)
    print("Sent SABME on uni0/1 DLCI 0")

    # Read UA Response from VFRS
    ua_raw = read_one_frame(p1)
    if not ua_raw:
        print("Error: No UA response from VFRS")
        proc.terminate()
        sys.exit(1)
    addr, ctrl, payload = parse_frame(ua_raw)
    print(f"Received response on uni0/1: Address={addr.hex()}, Control={ctrl.hex()}")
    # UA response control byte is 0x73 (UA with F=1)
    if ctrl != b'\x73':
        print(f"Error: Expected UA control byte 0x73, got {ctrl.hex()}")
        proc.terminate()
        sys.exit(1)
    print("LAPF Link established successfully on uni0/1 DLCI 0!")

    # 2. Establish LAPF link on uni0/2
    sabme_frame2 = make_frame(b'\x00\x01', b'\x7F')
    p2.write(sabme_frame2)
    print("Sent SABME on uni0/2 DLCI 0")
    ua_raw2 = read_one_frame(p2)
    addr2, ctrl2, payload2 = parse_frame(ua_raw2)
    if ctrl2 != b'\x73':
        print(f"Error: Expected UA control byte 0x73 on uni0/2, got {ctrl2.hex()}")
        proc.terminate()
        sys.exit(1)
    print("LAPF Link established successfully on uni0/2 DLCI 0!")

    # 3. Send SETUP message on uni0/1 with Reverse Charging Requested (acc=0 on destination, should reject!)
    # Q.933 SETUP payload:
    # Protocol Discriminator: 0x08
    # Call Ref len: 0x02, Call Ref value: 0x0005 (originating, flag=0) -> [0x02, 0x00, 0x05]
    # Message type: SETUP = 0x05
    # Bearer Capability: [0x04, 0x03, 0x88, 0xC0, 0x8F]
    # Called Party Number: 510401010002 -> [0x70, 0x0D, 0x93] + b'510401010002'
    # Calling Party Number: 510401010001 -> [0x6C, 0x0D, 0x93] + b'510401010001'
    # Reverse Charging Indication IE: [0x4A, 0x01, 0x81] (requested)
    # Priority & Service Class IE: [0x6A, 0x06, 0x01, 0xCC, 0x02, 0x55, 0x03, 0x02] (FTP: out=12, in=12; FDP: out=5, in=5; Class=2)
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
    # Wrap in I-frame command (N(S)=0, N(R)=0)
    # Control: [0x00, 0x00]
    setup_frame = make_frame(b'\x00\x01', b'\x00\x00', setup_payload)
    p1.write(setup_frame)
    print("Sent SETUP with Reverse Charging on uni0/1")

    # We expect RELEASE on uni0/1 with Cause 29 (Facility rejected)
    resp = read_one_frame(p1)
    if not resp:
        print("Error: No response to SETUP")
        proc.terminate()
        sys.exit(1)
    
    addr, ctrl, payload = parse_frame(resp)
    # Check payload: message type is RELEASE (0x4D)
    # Release message format:
    # Prot Disc (1 byte), CRV (3 bytes), Msg Type (1 byte), Cause IE (4 bytes: [0x08, 0x02, 0x80, cause_value])
    if len(payload) >= 5 and payload[4] == 0x4D:
        print("Received RELEASE message!")
        if len(payload) >= 9 and payload[5] == 0x08: # Cause IE present
            cause = payload[8] & 0x7F
            print(f"Cause Value: {cause}")
            if cause == 29:
                print("TEST PASSED: Call successfully rejected with Cause 29 (Facility rejected)!")
                # Respond to VFRS with RELEASE COMPLETE to finalize clearing
                # Prot Disc (0x08), CRV flag=1 (0x82 for CRV 5), Msg Type = RELEASE COMPLETE (0x5A)
                rel_comp = make_frame(b'\x00\x01', b'\x00\x00', b'\x08\x02\x80\x05\x5A')
                p1.write(rel_comp)
                print("Sent RELEASE COMPLETE to VFRS")
            else:
                print(f"TEST FAILED: Expected Cause 29, got {cause}")
                proc.terminate()
                sys.exit(1)
        else:
            print("Error: Cause IE missing in RELEASE")
            proc.terminate()
            sys.exit(1)
    else:
        print(f"Error: Expected RELEASE message, got {payload.hex()}")
        proc.terminate()
        sys.exit(1)

    # 4. Now modify the configuration to enable Reverse Charging Acceptance on uni0/2 (rev_charge_acc=1)
    print("Re-configuring VFRS to accept reverse charging on uni0/2...")
    proc.terminate()
    time.sleep(0.5)

    config_content = """# VFRS test config for SVC and QoS / charging facilities
port uni0/1 pipe-server vfrs_svc_pipe_1
port uni0/2 pipe-server vfrs_svc_pipe_2
svc_int uni0/1 dlci_low=512 dlci_high=600 default_ftp=12 default_fdp=6 default_svc_class=2
svc_int uni0/2 dlci_low=512 dlci_high=600 default_ftp=10 default_fdp=5 default_svc_class=3
svc_addr uni0/1 x121 510401010001 rev_charge_acc=1 rev_charge_prev=0
svc_addr uni0/2 x121 510401010002 rev_charge_acc=1 rev_charge_prev=0
svc_route 510401010002 uni0/2
"""
    with open(config_path, "w") as f:
        f.write(config_content)

    proc = subprocess.Popen(
        ["./vfrs.exe", config_path],
        env=my_env
    )
    time.sleep(1.5)

    try:
        p1 = open(pipe1_path, "r+b", buffering=0)
        p2 = open(pipe2_path, "r+b", buffering=0)
    except Exception as e:
        print(f"Failed to open named pipes: {e}")
        proc.terminate()
        sys.exit(1)

    # Establish LAPF links
    p1.write(make_frame(b'\x00\x01', b'\x7F'))
    read_one_frame(p1)
    p2.write(make_frame(b'\x00\x01', b'\x7F'))
    read_one_frame(p2)
    print("LAPF links re-established.")

    # Send SETUP with reverse charging again
    p1.write(make_frame(b'\x00\x01', b'\x00\x00', setup_payload))
    print("Sent SETUP on uni0/1 (Reverse Charging Accepted by target)")

    # Read SETUP forwarded to uni0/2
    forwarded = read_one_frame(p2)
    if not forwarded:
        print("Error: SETUP not forwarded to uni0/2")
        proc.terminate()
        sys.exit(1)
    
    addr, ctrl, payload = parse_frame(forwarded)
    print("Forwarded SETUP message received on uni0/2!")
    
    # Let's inspect the forwarded SETUP payload to check if:
    # 1. Reverse charge IE (0x4A) is present.
    # 2. Priority parameters IE (0x6A) is present with the correct values.
    has_rev_charge = False
    has_priority = False
    idx = 5 # skip prot_disc (1), crv_len (1), crv (2), msg_type (1)
    while idx + 2 <= len(payload):
        ie_id = payload[idx]
        ie_len = payload[idx+1]
        ie_data = payload[idx+2 : idx+2+ie_len]
        if ie_id == 0x4A:
            has_rev_charge = True
            print(f"Found Reverse Charging IE: data={ie_data.hex()}")
            if ie_data[0] != 0x81:
                print(f"Error: Expected reverse charge value 0x81, got {ie_data[0]:02X}")
                proc.terminate()
                sys.exit(1)
        elif ie_id == 0x6A:
            has_priority = True
            print(f"Found Priority & Service Class IE: data={ie_data.hex()}")
            # Expected content of Priority IE payload:
            # 0x01, 0xCC (FTP: out=12, in=12), 0x02, 0x55 (FDP: out=5, in=5), 0x03, 0x02 (SrvCls=2)
            if ie_data != b'\x01\xcc\x02\x55\x03\x02':
                print(f"Error: Expected priority payload 01cc02550302, got {ie_data.hex()}")
                proc.terminate()
                sys.exit(1)
        idx += 2 + ie_len

    if not has_rev_charge or not has_priority:
        print(f"Error: Missing value-added IEs. rev_charge={has_rev_charge}, priority={has_priority}")
        proc.terminate()
        sys.exit(1)

    print("ALL VALUE-ADDED SERVICES TESTS PASSED SUCCESSFULLY!")
    proc.terminate()
    sys.exit(0)

if __name__ == '__main__':
    main()
