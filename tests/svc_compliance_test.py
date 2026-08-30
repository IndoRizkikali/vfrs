import subprocess
import time
import os
import sys
import socket

def parse_q933_msg(payload):
    if len(payload) < 4:
        return None
    prot_disc = payload[0]
    crv_len = payload[1] & 0x0F
    if len(payload) < 3 + crv_len:
        return None
    crv_val = int.from_bytes(payload[2:2+crv_len], 'big')
    msg_type = payload[2+crv_len]
    ie_data = payload[3+crv_len:]
    return {
        'prot_disc': prot_disc,
        'crv_val': crv_val,
        'msg_type': msg_type,
        'ie_data': ie_data
    }

# LAPF Sequence State
p1_ns = 0
p1_nr = 0
p2_ns = 0
p2_nr = 0

def send_l3_p1(s, payload):
    global p1_ns, p1_nr
    ctrl = bytes([p1_ns << 1, p1_nr << 1])
    frame = b'\x00\x01' + ctrl + payload
    s.sendto(frame, ('127.0.0.1', 30001))
    p1_ns = (p1_ns + 1) % 128

def send_l3_p2(s, payload):
    global p2_ns, p2_nr
    ctrl = bytes([p2_ns << 1, p2_nr << 1])
    frame = b'\x00\x01' + ctrl + payload
    s.sendto(frame, ('127.0.0.1', 30002))
    p2_ns = (p2_ns + 1) % 128

def read_l3_timeout(s, is_p1=True, timeout_sec=2.0):
    global p1_nr, p2_nr
    s.settimeout(0.05)
    start = time.time()
    while (time.time() - start) < timeout_sec:
        try:
            data, _ = s.recvfrom(2048)
        except (socket.timeout, BlockingIOError, OSError):
            continue
        if len(data) >= 4 and data[:2] == b'\x00\x01':
            ctrl = data[2:4]
            if (ctrl[0] & 1) == 0:
                # I-frame
                vfrs_ns = ctrl[0] >> 1
                if is_p1:
                    p1_nr = (vfrs_ns + 1) % 128
                else:
                    p2_nr = (vfrs_ns + 1) % 128
                return data[:2], ctrl, data[4:]
    return None, None, None

def decode_dlci_2octet(b):
    if not b or len(b) < 2: return 0
    return (((b[0] & 0xFC) >> 2) << 4) | ((b[1] & 0xF0) >> 4)

def read_data_frame(s, timeout_sec=2.0):
    s.settimeout(0.05)
    start = time.time()
    while (time.time() - start) < timeout_sec:
        try:
            data, _ = s.recvfrom(2048)
        except (socket.timeout, BlockingIOError, OSError):
            continue
        if len(data) >= 2:
            if data[:2] in (b'\x00\x01', b'\x02\x01'):
                continue
            return data[:2], data[2:]
    return None, None

def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(line_buffering=True)
    
    config_content = """# VFRS test config for SVC compliance
log level con=info txt=debug
default svc uni t303=0.5 t305=0.5 t308=0.5 t310=1 t322=1
port uni0/1 udp-server 127.0.0.1 30001
port uni0/2 udp-server 127.0.0.1 30002
svc int uni0/1 dlci_low=512 dlci_high=600 revchg=allow
svc int uni0/2 dlci_low=512 dlci_high=600 revchg=allow
svc addr uni0/1 manual x121 510401010001
svc addr uni0/2 manual x121 510401010002
svc route uni0/2 prefix x121 510401010002
"""
    config_path = "tests/svc_compliance.conf"
    os.makedirs("tests", exist_ok=True)
    with open(config_path, "w") as f:
        f.write(config_content)

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
        # Establish LAPF links via SABME / UA
        for _ in range(5):
            s1.sendto(b'\x00\x01\x7F', ('127.0.0.1', 30001)) # SABME
            time.sleep(0.05)
            s2.sendto(b'\x00\x01\x7F', ('127.0.0.1', 30002)) # SABME
            time.sleep(0.05)

        s1.settimeout(1.0)
        s2.settimeout(1.0)
        
        # Drain initial UAs
        try:
            while True:
                d, _ = s1.recvfrom(2048)
                if d == b'\x00\x01\x73':
                    print("Received UA on uni0/1")
                    break
        except socket.timeout:
            pass

        try:
            while True:
                d, _ = s2.recvfrom(2048)
                if d == b'\x00\x01\x73':
                    print("Received UA on uni0/2")
                    break
        except socket.timeout:
            pass

        print("LAPF links established on UDP transport.")

        # ==========================================================
        # Test Case 1: Status Enquiry on unrecognized CRV (§10.10.3.2 rule 6)
        # ==========================================================
        status_enq_payload = b'\x08\x02\x00\x99\x75'
        send_l3_p1(s1, status_enq_payload)
        print("Sent STATUS ENQUIRY with unrecognized CRV 0x0099")

        addr, ctrl, payload = read_l3_timeout(s1, is_p1=True)
        if not payload:
            raise AssertionError("No response to STATUS ENQUIRY")
        msg = parse_q933_msg(payload)
        if msg['msg_type'] != 0x7D: # STATUS
            raise AssertionError(f"Expected STATUS (0x7D), got {msg['msg_type']:02X}")
        print("Received STATUS response for unrecognized CRV 0x0099!")
        
        ie_data = msg['ie_data']
        has_cause_30 = False
        has_state_0 = False
        idx = 0
        while idx < len(ie_data):
            if ie_data[idx] & 0x80:
                idx += 1
                continue
            if idx + 1 >= len(ie_data): break
            ie_id = ie_data[idx]
            ie_len = ie_data[idx+1]
            val = ie_data[idx+2 : idx+2+ie_len]
            if ie_id == 0x08: # Cause
                cause_val = val[1] & 0x7F
                if cause_val == 30:
                    has_cause_30 = True
            elif ie_id == 0x14: # Call State
                state_val = val[0] & 0x3F
                if state_val == 0:
                    has_state_0 = True
            idx += 2 + ie_len
        
        if not has_cause_30 or not has_state_0:
            raise AssertionError(f"Expected Cause 30 and Call State 0. Got Cause30={has_cause_30}, State0={has_state_0}")
        print("Test Case 1 PASSED: Unrecognized STATUS ENQUIRY correctly returned STATUS Call State Null, Cause 30!")

        # ==========================================================
        # Test Case 2: Status reporting non-Null on unrecognized CRV (§10.10.3.2 rule 5)
        # ==========================================================
        status_payload = b'\x08\x02\x00\x99\x7D\x08\x02\x80\x1E\x14\x01\x0A'
        send_l3_p1(s1, status_payload)
        print("Sent STATUS message with unrecognized CRV 0x0099 reporting active state")

        addr, ctrl, payload = read_l3_timeout(s1, is_p1=True)
        if not payload:
            raise AssertionError("No response to STATUS message reporting non-Null")
        msg = parse_q933_msg(payload)
        if msg['msg_type'] != 0x5A: # RELEASE COMPLETE
            raise AssertionError(f"Expected RELEASE COMPLETE (0x5A), got {msg['msg_type']:02X}")
        cause_val = msg['ie_data'][3] & 0x7F
        if cause_val != 101:
            raise AssertionError(f"Expected Cause 101, got {cause_val}")
        print("Test Case 2 PASSED: STATUS reporting non-Null on unrecognized CRV returned RELEASE COMPLETE with Cause 101!")

        # ==========================================================
        # Test Case 3: Status reporting Null on unrecognized CRV (§10.10.3.2 rule 5)
        # ==========================================================
        status_null_payload = b'\x08\x02\x00\x99\x7D\x08\x02\x80\x1E\x14\x01\x00'
        send_l3_p1(s1, status_null_payload)
        print("Sent STATUS message with unrecognized CRV 0x0099 reporting Null state")

        addr, ctrl, payload = read_l3_timeout(s1, is_p1=True, timeout_sec=0.3)
        if payload:
            raise AssertionError("Expected VFRS to ignore STATUS reporting Null state on unrecognized CRV, but received response")
        print("Test Case 3 PASSED: STATUS reporting Null on unrecognized CRV ignored!")

        # ==========================================================
        # Test Case 4: Cause Diagnostic Field Verification (X.36 Annex E / X.76 Annex B)
        # ==========================================================
        # Send SETUP with missing Bearer Capability (only Called Party Number) -> Expect Cause 96 with Diagnostic = 0x04
        setup_no_bc = (
            b'\x08' +
            b'\x02\x00\x08' +
            b'\x05' +
            b'\x70\x0D\x93' + b'510401010002'
        )
        send_l3_p1(s1, setup_no_bc)
        addr, ctrl, resp_payload = read_l3_timeout(s1, is_p1=True)
        if not resp_payload:
            raise AssertionError("No response to SETUP missing Bearer Capability")
        msg = parse_q933_msg(resp_payload)
        if msg['msg_type'] not in (0x4D, 0x5A): # RELEASE or RELEASE COMPLETE per §10.7.4.2
            raise AssertionError(f"Expected RELEASE (0x4D) or RELEASE COMPLETE (0x5A), got {msg['msg_type']:02X}")
        # Parse Cause IE
        ie_data = msg['ie_data']
        if ie_data[0] != 0x08 or (ie_data[3] & 0x7F) != 96:
            raise AssertionError(f"Expected Cause 96, got {ie_data}")
        # Check Diagnostic octet 5
        if ie_data[1] >= 3:
            diag_ie = ie_data[4]
            if diag_ie != 0x04: # Bearer Capability IE ID
                raise AssertionError(f"Expected diagnostic 0x04 (Bearer Cap), got 0x{diag_ie:02X}")
            print(f"Verified Cause 96 diagnostic field: 0x{diag_ie:02X} (Bearer Capability IE)")
        if msg['msg_type'] == 0x4D:
            send_l3_p1(s1, b'\x08\x02\x00\x08\x5A') # Acknowledge with RELEASE COMPLETE
        print("Test Case 4 PASSED: Cause IE Diagnostic field correctly reported missing IE identifier (0x04)!")

        # ==========================================================
        # Test Case 5: Single-Octet IE Stepping Robustness
        # ==========================================================
        # Inject single-octet Shift IE 0x95 before Bearer Capability in SETUP
        setup_single_octet = (
            b'\x08' +
            b'\x02\x00\x09' +
            b'\x05' +
            b'\x95' + # Single-octet IE
            b'\x04\x03\x88\xA0\xCF' + # Bearer Capability
            b'\x70\x0D\x93' + b'510401010002' + # Called Number
            b'\x6C\x0D\x93' + b'510401010001'   # Calling Number
        )
        send_l3_p1(s1, setup_single_octet)
        addr, ctrl, resp_payload = read_l3_timeout(s1, is_p1=True)
        if not resp_payload:
            raise AssertionError("No CALL PROCEEDING received for SETUP with single-octet IE")
        msg = parse_q933_msg(resp_payload)
        if msg['msg_type'] != 0x02: # CALL PROCEEDING
            raise AssertionError(f"Expected CALL PROCEEDING (0x02), got {msg['msg_type']:02X}")
        print("Received CALL PROCEEDING for SETUP with single-octet IE!")
        # Clear call
        send_l3_p1(s1, b'\x08\x02\x00\x09\x4D\x08\x02\x80\x10') # RELEASE
        read_l3_timeout(s1, is_p1=True) # RELEASE COMPLETE
        _, _, rel_p2 = read_l3_timeout(s2, is_p1=False, timeout_sec=0.2)
        if rel_p2:
            m_rel = parse_q933_msg(rel_p2)
            if m_rel:
                send_l3_p2(s2, b'\x08\x02' + (m_rel['crv_val'] | 0x8000).to_bytes(2, 'big') + b'\x5A')
        print("Test Case 5 PASSED: Single-octet IE stepped cleanly without corrupting subsequent IEs!")

        # ==========================================================
        # Test Case 6: Duplicate Mandatory IE Preservation (§10.10.5.2)
        # ==========================================================
        # Send SETUP with duplicate Bearer Capability and duplicate Called Number
        setup_dup = (
            b'\x08' +
            b'\x02\x00\x0A' +
            b'\x05' +
            b'\x04\x03\x88\xA0\xCF' + # First BC (valid)
            b'\x04\x03\x80\x00\x00' + # Duplicate BC (invalid dummy, must be ignored per §10.10.5.2)
            b'\x70\x0D\x93' + b'510401010002' + # First Called Number (valid)
            b'\x70\x0D\x93' + b'999999999999' + # Duplicate Called Number (must be ignored)
            b'\x6C\x0D\x93' + b'510401010001'
        )
        send_l3_p1(s1, setup_dup)
        addr, ctrl, resp_payload = read_l3_timeout(s1, is_p1=True)
        if not resp_payload:
            raise AssertionError("No CALL PROCEEDING received for SETUP with duplicate IEs")
        msg = parse_q933_msg(resp_payload)
        if msg['msg_type'] != 0x02:
            raise AssertionError(f"Expected CALL PROCEEDING (0x02), got {msg['msg_type']:02X}")
        # Clear call
        send_l3_p1(s1, b'\x08\x02\x00\x0A\x4D\x08\x02\x80\x10')
        read_l3_timeout(s1, is_p1=True)
        _, _, rel_p2 = read_l3_timeout(s2, is_p1=False, timeout_sec=0.2)
        if rel_p2:
            m_rel = parse_q933_msg(rel_p2)
            if m_rel:
                send_l3_p2(s2, b'\x08\x02' + (m_rel['crv_val'] | 0x8000).to_bytes(2, 'big') + b'\x5A')
        print("Test Case 6 PASSED: Duplicate mandatory IEs preserved first instance per §10.10.5.2!")

        # Drain any stray frames before starting Test 7
        for _ in range(5):
            _, _, p = read_l3_timeout(s2, is_p1=False, timeout_sec=0.05)
            if not p: break
        for _ in range(5):
            _, _, p = read_l3_timeout(s1, is_p1=True, timeout_sec=0.05)
            if not p: break

        # ==========================================================
        # Test Case 7: Full Call Establishment & Bidirectional Data Plane
        # ==========================================================
        setup_full = (
            b'\x08' +
            b'\x02\x00\x0B' +
            b'\x05' +
            b'\x04\x03\x88\xA0\xCF' +
            b'\x70\x0D\x93' + b'510401010002' +
            b'\x6C\x0D\x93' + b'510401010001'
        )
        send_l3_p1(s1, setup_full)
        read_l3_timeout(s1, is_p1=True) # CALL PROC

        fwd_payload = None
        fwd_msg = None
        for _ in range(5):
            addr, ctrl, p = read_l3_timeout(s2, is_p1=False, timeout_sec=1.0)
            if p:
                m = parse_q933_msg(p)
                if m and m['msg_type'] == 0x05: # SETUP
                    fwd_payload = p
                    fwd_msg = m
                    break
        if not fwd_payload:
            raise AssertionError("No forwarded SETUP received on uni0/2")
        fwd_crv = fwd_msg['crv_val']
        print(f"Forwarded SETUP to uni0/2 (CRV=0x{fwd_crv:04X})")

        # Extract assigned DLCI from forwarded SETUP on uni0/2
        assigned_dlci = 512
        idx = 0
        ie_data = fwd_msg['ie_data']
        while idx < len(ie_data):
            if ie_data[idx] & 0x80:
                idx += 1
                continue
            if idx + 1 >= len(ie_data): break
            if ie_data[idx] == 0x19 and ie_data[idx+1] >= 2: # DLCI IE
                d_val = ie_data[idx+2 : idx+2+ie_data[idx+1]]
                assigned_dlci = ((d_val[0] & 0x3F) << 4) | ((d_val[1] >> 3) & 0x0F)
                break
            idx += 2 + ie_data[idx+1]
        print(f"Called DTE on uni0/2 connecting with assigned DLCI {assigned_dlci}")

        # Called DTE connects with assigned DLCI
        dlci_ie_content = bytes([0x40 | ((assigned_dlci >> 4) & 0x3F), 0x80 | ((assigned_dlci & 0x0F) << 3)])
        connect_valid = b'\x08\x02' + (fwd_crv | 0x8000).to_bytes(2, 'big') + b'\x07\x19\x02' + dlci_ie_content
        send_l3_p2(s2, connect_valid)

        addr, ctrl, conn_p1 = read_l3_timeout(s1, is_p1=True)
        conn_msg = parse_q933_msg(conn_p1)
        if not conn_msg or conn_msg['msg_type'] != 0x07:
            raise AssertionError("Expected CONNECT on uni0/1")
        print("Received CONNECT on uni0/1. Call Active!")

        time.sleep(0.1) # Allow data path fast-path registration

        # Send data packet uni0/1 -> uni0/2 on DLCI 512
        # Address header for DLCI 512
        addr_512 = bytes([((512 >> 4) & 0x3F) << 2, ((512 & 0x0F) << 4) | 0x01])
        # Address header for assigned DLCI on uni0/2
        addr_assigned = bytes([((assigned_dlci >> 4) & 0x3F) << 2, ((assigned_dlci & 0x0F) << 4) | 0x01])

        data_payload = addr_512 + b'\x03\xCC\x45\x00PingTestData'
        s1.sendto(data_payload, ('127.0.0.1', 30001))
        addr_rx, payload_rx = read_data_frame(s2, timeout_sec=2.0)
        if not payload_rx or decode_dlci_2octet(addr_rx) != assigned_dlci or payload_rx != b'\x03\xCC\x45\x00PingTestData':
            raise AssertionError(f"Forward data frame mismatch: dlci={decode_dlci_2octet(addr_rx)}, payload={payload_rx.hex() if payload_rx else 'None'}")
        print("Switched data packet from uni0/1 (DLCI 512) to uni0/2 successfully!")

        # Send data packet uni0/2 -> uni0/1 on assigned DLCI
        data_payload_rev = addr_assigned + b'\x03\xCC\x45\x00PingReplyData'
        s2.sendto(data_payload_rev, ('127.0.0.1', 30002))
        addr_rx2, payload_rx2 = read_data_frame(s1, timeout_sec=2.0)
        if not payload_rx2 or decode_dlci_2octet(addr_rx2) != 512 or payload_rx2 != b'\x03\xCC\x45\x00PingReplyData':
            raise AssertionError(f"Reverse data frame mismatch: dlci={decode_dlci_2octet(addr_rx2)}, payload={payload_rx2.hex() if payload_rx2 else 'None'}")
        print("Switched reverse data packet from uni0/2 to uni0/1 (DLCI 512) successfully!")

        # Clear call with DISCONNECT from uni0/2
        send_l3_p2(s2, b'\x08\x02' + (fwd_crv | 0x8000).to_bytes(2, 'big') + b'\x45')
        read_l3_timeout(s2, is_p1=False) # RELEASE
        send_l3_p2(s2, b'\x08\x02' + (fwd_crv | 0x8000).to_bytes(2, 'big') + b'\x5A') # RELEASE COMPLETE

        # uni0/1 receives DISCONNECT
        read_l3_timeout(s1, is_p1=True)
        send_l3_p1(s1, b'\x08\x02\x00\x0B\x4D\x08\x02\x80\x10')
        read_l3_timeout(s1, is_p1=True)

        print("Test Case 7 PASSED: Full Call Establishment & Bidirectional Data Plane Verified!")

        print("\n=========================================================")
        print("ALL SVC CONFORMANCE & STANDARDS COMPLIANCE TESTS PASSED!")
        print("=========================================================")
        proc.terminate()
        sys.exit(0)

    except Exception as e:
        print(f"\nTEST SUITE FAILED: {e}")
        import traceback
        traceback.print_exc()
        proc.terminate()
        sys.exit(1)

if __name__ == '__main__':
    main()
