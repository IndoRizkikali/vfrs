import subprocess
import time
import os
import sys
import ctypes
from ctypes import wintypes
import msvcrt

kernel32 = ctypes.windll.kernel32
kernel32.SetNamedPipeHandleState.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
    ctypes.c_void_p
]
kernel32.SetNamedPipeHandleState.restype = wintypes.BOOL

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

def read_one_frame_timeout(pipe, timeout_sec=2.0):
    buf = bytearray()
    flag_count = 0
    start = time.time()
    while (time.time() - start) < timeout_sec and flag_count < 2:
        try:
            b = pipe.read(1)
        except (BlockingIOError, OSError):
            b = None
        if b:
            if b == b'\x7e':
                flag_count += 1
                if flag_count == 1:
                    buf.clear()
                else:
                    return bytes(buf)
            else:
                if flag_count == 1:
                    buf.append(b[0])
        else:
            time.sleep(0.005)
    return None

def parse_frame(frame_bytes):
    if len(frame_bytes) < 4:
        return None, None, None
    body = frame_bytes[:-2]
    crc_received = int.from_bytes(frame_bytes[-2:], 'little')
    calc_crc = crc16(body)
    if calc_crc != crc_received:
        raise ValueError(f"CRC Error: received {crc_received:04X}, calculated {calc_crc:04X} on body {body.hex()}")
    
    addr = body[:2]
    ctrl = body[2:3]
    if (body[2] & 1) == 0:
        ctrl = body[2:4]
        payload = body[4:]
    else:
        payload = body[3:]
    return addr, ctrl, payload

def parse_q933_msg(payload):
    if len(payload) < 5:
        return None
    prot_disc = payload[0]
    crv_len = payload[1] & 0x0F
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

def send_l3_p1(pipe, payload):
    global p1_ns, p1_nr
    ctrl = bytes([p1_ns << 1, p1_nr << 1])
    pipe.write(make_frame(b'\x00\x01', ctrl, payload))
    pipe.flush()
    print(f"DEBUG send_l3_p1: Sent I-frame N(S)={p1_ns}, N(R)={p1_nr}")
    p1_ns = (p1_ns + 1) % 128

def send_l3_p2(pipe, payload):
    global p2_ns, p2_nr
    ctrl = bytes([p2_ns << 1, p2_nr << 1])
    pipe.write(make_frame(b'\x00\x01', ctrl, payload))
    pipe.flush()
    print(f"DEBUG send_l3_p2: Sent I-frame N(S)={p2_ns}, N(R)={p2_nr}")
    p2_ns = (p2_ns + 1) % 128

def read_l3_p1_timeout(pipe, timeout_sec=2.0):
    global p1_nr
    start = time.time()
    while (time.time() - start) < timeout_sec:
        remaining = timeout_sec - (time.time() - start)
        if remaining <= 0:
            break
        resp = read_one_frame_timeout(pipe, timeout_sec=remaining)
        if not resp:
            continue
        addr, ctrl, payload = parse_frame(resp)
        if (ctrl[0] & 1) == 0:
            # I-frame
            vfrs_ns = ctrl[0] >> 1
            p1_nr = (vfrs_ns + 1) % 128
            print(f"DEBUG read_l3_p1: Received I-frame N(S)={vfrs_ns}, updated p1_nr={p1_nr}")
            return addr, ctrl, payload
        else:
            print(f"DEBUG read_l3_p1: Ignored S/U-frame Control={ctrl.hex()}")
    return None, None, None

def read_l3_p2_timeout(pipe, timeout_sec=2.0):
    global p2_nr
    start = time.time()
    while (time.time() - start) < timeout_sec:
        remaining = timeout_sec - (time.time() - start)
        if remaining <= 0:
            break
        resp = read_one_frame_timeout(pipe, timeout_sec=remaining)
        if not resp:
            continue
        addr, ctrl, payload = parse_frame(resp)
        if (ctrl[0] & 1) == 0:
            # I-frame
            vfrs_ns = ctrl[0] >> 1
            p2_nr = (vfrs_ns + 1) % 128
            print(f"DEBUG read_l3_p2: Received I-frame N(S)={vfrs_ns}, updated p2_nr={p2_nr}")
            return addr, ctrl, payload
        else:
            print(f"DEBUG read_l3_p2: Ignored S/U-frame Control={ctrl.hex()}")
    return None, None, None

def read_data_frame(pipe, timeout_sec=2.0):
    start = time.time()
    while (time.time() - start) < timeout_sec:
        remaining = timeout_sec - (time.time() - start)
        if remaining <= 0:
            break
        resp = read_one_frame_timeout(pipe, timeout_sec=min(remaining, 0.5))
        if not resp:
            continue
        addr, ctrl, payload = parse_frame(resp)
        if addr in (b'\x00\x01', b'\x02\x01'):
            # Ignore LAPF supervisory frames on DLCI 0
            continue
        return addr, ctrl, payload
    return None, None, None

def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(line_buffering=True)
    config_content = """# VFRS test config for SVC compliance
log_level con=info txt=debug
port uni0/1 pipe-server vfrs_svc_pipe_1
port uni0/2 pipe-server vfrs_svc_pipe_2
log_level con=info txt=debug
svc_int uni0/1 dlci_low=512 dlci_high=600 t303=0.5 t305=0.5 t308=0.5 t310=1 t322=1
svc_int uni0/2 dlci_low=512 dlci_high=600 t303=0.5 t305=0.5 t308=0.5 t310=1 t322=1
svc_addr uni0/1 x121 510401010001 rev_charge_acc=1
svc_addr uni0/2 x121 510401010002 rev_charge_acc=1
svc_route 510401010002 uni0/2
"""
    config_path = "tests/svc_compliance.conf"
    os.makedirs("tests", exist_ok=True)
    with open(config_path, "w") as f:
        f.write(config_content)

    my_env = os.environ.copy()
    my_env["PATH"] = r"C:\msys64\ucrt64\bin;" + my_env.get("PATH", "")

    vfrs_exe = "bin/vfrs.exe" if os.path.exists("bin/vfrs.exe") else ("./vfrs.exe" if os.path.exists("./vfrs.exe") else "../bin/vfrs.exe")
    proc = subprocess.Popen(
        [vfrs_exe, config_path],
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

    # Configure pipes for non-blocking read mode (PIPE_NOWAIT = 0x00000001)
    mode = wintypes.DWORD(0x00000001)
    h1 = wintypes.HANDLE(msvcrt.get_osfhandle(p1.fileno()))
    h2 = wintypes.HANDLE(msvcrt.get_osfhandle(p2.fileno()))
    kernel32.SetNamedPipeHandleState(h1, ctypes.byref(mode), None, None)
    kernel32.SetNamedPipeHandleState(h2, ctypes.byref(mode), None, None)

    try:
        # Establish LAPF links
        p1.write(make_frame(b'\x00\x01', b'\x7F'))
        p1.flush()
        print("Sent SABME on uni0/1 DLCI 0")
        ua_1 = read_one_frame_timeout(p1, timeout_sec=2.0)
        if not ua_1:
            print("Error: No UA response on uni0/1")
            proc.terminate()
            sys.exit(1)
        print("Received UA on uni0/1")

        p2.write(make_frame(b'\x00\x01', b'\x7F'))
        p2.flush()
        print("Sent SABME on uni0/2 DLCI 0")
        ua_2 = read_one_frame_timeout(p2, timeout_sec=2.0)
        if not ua_2:
            print("Error: No UA response on uni0/2")
            proc.terminate()
            sys.exit(1)
        print("Received UA on uni0/2")
        print("LAPF links established.")

        # ==========================================================
        # Test Case 1: Status Enquiry on unrecognized CRV
        # ==========================================================
        # Message type 0x75 (STATUS ENQUIRY), unrecognized CRV 0x0099
        status_enq_payload = b'\x08\x02\x00\x99\x75'
        send_l3_p1(p1, status_enq_payload)
        print("Sent STATUS ENQUIRY with unrecognized CRV 0x0099")

        addr, ctrl, payload = read_l3_p1_timeout(p1)
        if not payload:
            raise AssertionError("No response to STATUS ENQUIRY")
        msg = parse_q933_msg(payload)
        if msg['msg_type'] != 0x7D: # STATUS
            raise AssertionError(f"Expected STATUS (0x7D), got {msg['msg_type']:02X}")
        print("Received STATUS response for unrecognized CRV 0x0099!")
        
        # Verify Call State is Null (0) and Cause is Response to STATUS ENQUIRY (30)
        ie_data = msg['ie_data']
        has_cause_30 = False
        has_state_0 = False
        idx = 0
        while idx + 2 <= len(ie_data):
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
        # Test Case 2: Status reporting non-Null on unrecognized CRV
        # ==========================================================
        # Send STATUS (0x7D) with unrecognized CRV 0x0099, Call State = 10 (Active)
        # Call State IE: [0x14, 0x01, 0x0A]
        status_payload = b'\x08\x02\x00\x99\x7D\x08\x02\x80\x1E\x14\x01\x0A'
        send_l3_p1(p1, status_payload)
        print("Sent STATUS message with unrecognized CRV 0x0099 reporting active state")

        addr, ctrl, payload = read_l3_p1_timeout(p1)
        if not payload:
            raise AssertionError("No response to STATUS message reporting non-Null")
        msg = parse_q933_msg(payload)
        if msg['msg_type'] != 0x5A: # RELEASE COMPLETE
            raise AssertionError(f"Expected RELEASE COMPLETE (0x5A), got {msg['msg_type']:02X}")
        
        # Verify Cause is 101 (Message incompatible with call state)
        cause_val = msg['ie_data'][3] & 0x7F # Offset to cause value
        if cause_val != 101:
            raise AssertionError(f"Expected Cause 101, got {cause_val}")
        print("Test Case 2 PASSED: STATUS reporting non-Null on unrecognized CRV returned RELEASE COMPLETE with Cause 101!")

        # ==========================================================
        # Test Case 3: Status reporting Null on unrecognized CRV
        # ==========================================================
        # Send STATUS (0x7D) with unrecognized CRV 0x0099, Call State = 0 (Null)
        # Call State IE: [0x14, 0x01, 0x00]
        status_null_payload = b'\x08\x02\x00\x99\x7D\x08\x02\x80\x1E\x14\x01\x00'
        send_l3_p1(p1, status_null_payload)
        print("Sent STATUS message with unrecognized CRV 0x0099 reporting Null state")

        addr, ctrl, payload = read_l3_p1_timeout(p1, timeout_sec=0.5)
        if payload:
            raise AssertionError("Expected VFRS to ignore STATUS reporting Null state on unrecognized CRV, but received response")
        print("Test Case 3 PASSED: STATUS reporting Null on unrecognized CRV ignored!")

        # ==========================================================
        # Test Case 4: Called DTE Connect DLCI Validation Failure
        # ==========================================================
        # Place a call from uni0/1 to uni0/2
        setup_payload = (
            b'\x08' +
            b'\x02\x00\x05' +
            b'\x05' +
            b'\x04\x03\x88\xA0\xCF' +
            b'\x70\x0D\x93' + b'510401010002' +
            b'\x6C\x0D\x93' + b'510401010001'
        )
        send_l3_p1(p1, setup_payload)
        print("Sent SETUP from uni0/1")

        # Read CALL PROCEEDING on uni0/1
        addr, ctrl, call_proc_payload = read_l3_p1_timeout(p1)
        if not call_proc_payload:
            raise AssertionError("No CALL PROCEEDING on uni0/1")
        print("Received CALL PROCEEDING on uni0/1")

        # Read SETUP forwarded to uni0/2
        addr, ctrl, fwd_payload = read_l3_p2_timeout(p2)
        if not fwd_payload:
            raise AssertionError("No SETUP forwarded to uni0/2")
        fwd_msg = parse_q933_msg(fwd_payload)
        fwd_crv = fwd_msg['crv_val']
        print(f"Received forwarded SETUP on uni0/2 (CRV=0x{fwd_crv:04X})")

        # Called DTE on uni0/2 responds with CONNECT but without DLCI IE!
        connect_no_dlci = b'\x08\x02' + (fwd_crv | 0x8000).to_bytes(2, 'big') + b'\x07'
        send_l3_p2(p2, connect_no_dlci)
        print("Sent CONNECT on uni0/2 WITHOUT DLCI IE")

        # VFRS should reject/clear call by sending RELEASE on uni0/2 (Cause 96)
        addr, ctrl, reject_payload_p2 = read_l3_p2_timeout(p2)
        if not reject_payload_p2:
            raise AssertionError("No RELEASE on uni0/2")
        msg_p2 = parse_q933_msg(reject_payload_p2)
        if msg_p2['msg_type'] != 0x4D: # RELEASE
            raise AssertionError(f"Expected RELEASE (0x4D) on uni0/2, got {msg_p2['msg_type']:02X}")
        cause_p2 = msg_p2['ie_data'][3] & 0x7F
        if cause_p2 != 96:
            raise AssertionError(f"Expected Cause 96 on uni0/2, got {cause_p2}")
        print("Received RELEASE on uni0/2 with Cause 96!")

        # VFRS should also propagate the clearing to uni0/1 (sends RELEASE with Cause 96)
        addr, ctrl, reject_payload_p1 = read_l3_p1_timeout(p1)
        if not reject_payload_p1:
            raise AssertionError("No RELEASE propagated to uni0/1")
        msg_p1 = parse_q933_msg(reject_payload_p1)
        if msg_p1['msg_type'] != 0x4D: # RELEASE
            raise AssertionError(f"Expected RELEASE (0x4D) on uni0/1, got {msg_p1['msg_type']:02X}")
        cause_p1 = msg_p1['ie_data'][3] & 0x7F
        if cause_p1 != 96:
            raise AssertionError(f"Expected propagated Cause 96 on uni0/1, got {cause_p1}")
        print("Received propagated RELEASE on uni0/1 with Cause 96!")

        # Acknowledge rejections to VFRS to finalize clearing
        send_l3_p1(p1, b'\x08\x02\x00\x05\x5A') # RELEASE COMPLETE for CRV 5
        send_l3_p2(p2, b'\x08\x02' + (fwd_crv | 0x8000).to_bytes(2, 'big') + b'\x5A') # RELEASE COMPLETE for peer
        print("Test Case 4 PASSED: Called DTE DLCI omission correctly triggered standard clearing with Cause 96!")

        # Sleep briefly to allow VFRS to process RELEASE COMPLETE
        time.sleep(0.1)

        # ==========================================================
        # Test Case 5: T308 retransmission & T305 expiry clearing
        # ==========================================================
        # Place another call with CRV 6
        setup_payload2 = (
            b'\x08' +
            b'\x02\x00\x06' +
            b'\x05' +
            b'\x04\x03\x88\xA0\xCF' +
            b'\x70\x0D\x93' + b'510401010002' +
            b'\x6C\x0D\x93' + b'510401010001'
        )
        send_l3_p1(p1, setup_payload2)
        read_l3_p1_timeout(p1) # CALL PROCEEDING
        addr, ctrl, fwd_payload2 = read_l3_p2_timeout(p2) # SETUP on called DTE
        fwd_msg2 = parse_q933_msg(fwd_payload2)
        if fwd_msg2:
            print(f"DEBUG fwd_msg2: msg_type={fwd_msg2['msg_type']:02X}, crv_val={fwd_msg2['crv_val']:02X}, payload={fwd_payload2.hex()}")
        else:
            print(f"DEBUG fwd_msg2: None, payload={fwd_payload2.hex() if fwd_payload2 else 'None'}")
        fwd_crv2 = fwd_msg2['crv_val'] if fwd_msg2 else 0

        # DTE on uni0/2 responds with CONNECT with correct DLCI (say, 512)
        # DLCI IE: [0x19, 0x02, 0x60, 0x80] (DLCI 512)
        connect_valid = b'\x08\x02' + (fwd_crv2 | 0x8000).to_bytes(2, 'big') + b'\x07\x19\x02\x60\x80'
        send_l3_p2(p2, connect_valid)

        # Read CONNECT on uni0/1 (DCE originating side per X.36 §10.7.1.2)
        addr, ctrl, conn_payload_p1 = read_l3_p1_timeout(p1)
        conn_msg = parse_q933_msg(conn_payload_p1)
        if not conn_msg or conn_msg['msg_type'] != 0x07: # CONNECT
            raise AssertionError(f"Expected CONNECT (0x07) on uni0/1, got {conn_msg}")
        print("Received CONNECT on uni0/1. Call fully active and established.")

        # Verify dynamic fast-path data switching on DLCI 512 (uni0/1 -> uni0/2)
        data_payload = b'\x03\xCC\x45\x00PingTestData'
        p1.write(make_frame(b'\x80\x01', b'', data_payload)) # Send frame on DLCI 512
        p1.flush()
        addr_rx, ctrl_rx, payload_rx = read_data_frame(p2, timeout_sec=2.0)
        if not payload_rx:
            raise AssertionError("Data frame on DLCI 512 was not switched from uni0/1 to uni0/2!")
        dlci_rx = ((addr_rx[0] >> 2) << 4) | (addr_rx[1] >> 4)
        if dlci_rx != 512 or payload_rx != b'\xCC\x45\x00PingTestData':
            raise AssertionError(f"Switched data frame mismatch: DLCI={dlci_rx}, payload={payload_rx.hex() if payload_rx else 'None'}")
        print("Successfully verified user-plane data packet switching on DLCI 512 from uni0/1 to uni0/2!")

        # Verify reverse data switching on DLCI 512 (uni0/2 -> uni0/1)
        data_payload_rev = b'\x03\xCC\x45\x00PingReplyData'
        p2.write(make_frame(b'\x80\x01', b'', data_payload_rev)) # Send frame on DLCI 512
        p2.flush()
        addr_rx2, ctrl_rx2, payload_rx2 = read_data_frame(p1, timeout_sec=2.0)
        if not payload_rx2:
            raise AssertionError("Data frame on DLCI 512 was not switched in reverse from uni0/2 to uni0/1!")
        dlci_rx2 = ((addr_rx2[0] >> 2) << 4) | (addr_rx2[1] >> 4)
        if dlci_rx2 != 512 or payload_rx2 != b'\xCC\x45\x00PingReplyData':
            raise AssertionError(f"Switched reverse data frame mismatch: DLCI={dlci_rx2}, payload={payload_rx2.hex() if payload_rx2 else 'None'}")
        print("Successfully verified user-plane reverse data packet switching on DLCI 512 from uni0/2 to uni0/1!")

        # Send DISCONNECT from uni0/2 to clear the call
        send_l3_p2(p2, b'\x08\x02' + (fwd_crv2 | 0x8000).to_bytes(2, 'big') + b'\x45')
        print("Sent DISCONNECT from uni0/2")

        # Read RELEASE on uni0/2
        read_l3_p2_timeout(p2)
        # Send RELEASE COMPLETE from uni0/2 to clear called side
        send_l3_p2(p2, b'\x08\x02' + (fwd_crv2 | 0x8000).to_bytes(2, 'big') + b'\x5A')

        # VFRS should propagate DISCONNECT to uni0/1 and start T305
        addr, ctrl, disc_payload_p1 = read_l3_p1_timeout(p1)
        msg_disc = parse_q933_msg(disc_payload_p1)
        if not msg_disc or msg_disc['msg_type'] != 0x45: # DISCONNECT
            payload_hex = disc_payload_p1.hex() if disc_payload_p1 else "None"
            msg_type_str = f"{msg_disc['msg_type']:02X}" if msg_disc else "None"
            raise AssertionError(f"Expected DISCONNECT (0x45), got msg_type={msg_type_str}, payload={payload_hex}")
        print("Received propagated DISCONNECT on uni0/1. VFRS started T305.")

        # We (DTE on uni0/1) do NOT respond. VFRS's T305 should expire (500ms)
        # VFRS should send RELEASE on uni0/1 and start T308
        addr, ctrl, release_payload_p1_1 = read_l3_p1_timeout(p1, timeout_sec=1.5)
        if not release_payload_p1_1:
            raise AssertionError("No RELEASE received after T305 expiry")
        msg_rel1 = parse_q933_msg(release_payload_p1_1)
        if msg_rel1['msg_type'] != 0x4D: # RELEASE
            raise AssertionError(f"Expected RELEASE (0x4D) on T305 expiry, got {msg_rel1['msg_type']:02X}")
        print("Received RELEASE on uni0/1 after T305 expiry. VFRS started T308.")

        # We still do NOT respond. VFRS's T308 should expire (500ms) and retransmit RELEASE
        addr, ctrl, release_payload_p1_2 = read_l3_p1_timeout(p1, timeout_sec=1.5)
        if not release_payload_p1_2:
            raise AssertionError("No RELEASE retransmitted on first T308 expiry")
        msg_rel2 = parse_q933_msg(release_payload_p1_2)
        if msg_rel2['msg_type'] != 0x4D: # RELEASE
            raise AssertionError(f"Expected retransmitted RELEASE (0x4D), got {msg_rel2['msg_type']:02X}")
        print("Received retransmitted RELEASE on uni0/1 (first T308 expiry retry)!")

        # We still do NOT respond. VFRS's T308 should expire second time and clear call references
        # Let's wait for second expiry and verify that no more releases are sent
        addr, ctrl, release_payload_p1_3 = read_l3_p1_timeout(p1, timeout_sec=1.5)
        if release_payload_p1_3:
            raise AssertionError("Received third RELEASE message, but T308 should have cleared on second expiry")
        print("No third RELEASE received. T308 second expiry successfully cleared the call reference locally.")
        print("Test Case 5 PASSED: T305 expiry and T308 retransmissions behaved fully in accordance with the standard!")

        print("\nALL SVC PROTOCOL COMPLIANCE TEST CASES PASSED SUCCESSFULLY!")
        proc.terminate()
        sys.exit(0)

    except Exception as e:
        print(f"\nTEST SUITE FAILED: {e}")
        proc.terminate()
        sys.exit(1)

if __name__ == '__main__':
    main()
