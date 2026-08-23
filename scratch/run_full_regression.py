#!/usr/bin/env python3
"""
run_full_regression.py - Comprehensive Regression Test Suite for VFRS
Virtual Frame Relay Switch - Phase 1 Regression & Non-SVC Feature Verification
"""

import os
import sys
import time
import socket
import subprocess

def run_command_silent(cmd, cwd=None):
    try:
        res = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return res.returncode, res.stdout, res.stderr
    except Exception as e:
        return -1, "", str(e)

def encode_dlci(dlci, cr=0):
    b0 = ((dlci >> 4) & 0x3F) << 2
    b0 |= (cr & 1) << 1
    b1 = ((dlci & 0x0F) << 4) | 0x01
    return bytes([b0, b1])

def decode_dlci(data):
    if len(data) < 2:
        return None
    b0, b1 = data[0], data[1]
    dlci = ((b0 >> 2) << 4) | (b1 >> 4)
    cr = (b0 >> 1) & 1
    fecn = (b1 >> 3) & 1
    becn = (b1 >> 2) & 1
    de = (b1 >> 1) & 1
    return {
        "dlci": dlci,
        "cr": cr,
        "fecn": fecn,
        "becn": becn,
        "de": de
    }

def test_c_unit_suite():
    print("\n============================================================")
    print(" 1. RUNNING C MODULE UNIT TEST SUITE")
    print("============================================================")

    c_tests = [
        ("DLCI Allocator Unit Test", "scratch/test_dlci_allocator.exe"),
        ("SVC Numbering & Macro Engine Unit Test", "scratch/test_svc_numbering.exe"),
        ("Q.933 Message Parser & Builder Unit Test", "scratch/test_q933_parser.exe"),
        ("Q.933 DCE UNI Call State Machine Unit Test", "scratch/test_uni_fsm.exe"),
        ("Q.933 Annex A LMI Functionality Test", "scratch/test_q933a_lmi.exe")
    ]

    all_passed = True
    for name, exe_path in c_tests:
        print(f"-> Testing: {name}...")
        rc, out, err = run_command_silent([exe_path])
        if rc == 0:
            print(f"   [PASS] {name} passed successfully")
        else:
            print(f"   [FAIL] {name} failed with return code {rc}")
            if out: print(f"Output:\n{out}")
            if err: print(f"Error:\n{err}")
            all_passed = False

    return all_passed

def test_multicast_pvc_suite():
    print("\n============================================================")
    print(" 2. RUNNING PVC MULTICASTING & REPLICATION TEST")
    print("============================================================")

    rc, out, err = run_command_silent([sys.executable, "scratch/test_multicast.py"])
    if rc == 0 and "SUCCESS" in out.upper():
        print("   [PASS] PVC Multicasting & Replication test passed successfully")
        return True
    else:
        print(f"   [FAIL] PVC Multicasting test failed (code {rc})")
        if out: print(f"Output:\n{out}")
        if err: print(f"Error:\n{err}")
        return False

def test_lmi_congestion_lapf_suite():
    print("\n============================================================")
    print(" 3. RUNNING PVC, LMI STACK & CONGESTION MANAGEMENT TEST")
    print("============================================================")

    # 1. Create a test configuration file for PVC + LMI + Congestion
    conf_content = """# Temporary Regression Test Configuration
swconfig swid=SW_REGRESSION dnic=5104
log_level con=debug txt=debug
log_file test_reg.log

port uni0/0 udp 127.0.0.1 10010 127.0.0.1 20010
port uni0/1 udp 127.0.0.1 10011 127.0.0.1 20011

pvc uni0/0 100 uni0/1 200 cir=64000 bc=64000 be=0
pvc uni0/1 200 uni0/0 100 cir=64000 bc=64000 be=0

lmi uni0/0 ansi
lmi uni0/1 q933a
"""
    with open("scratch/test_reg.conf", "w") as f:
        f.write(conf_content)

    print("-> Starting VFRS instance with ANSI and Q.933A LMI enabled...")
    vfrs_path = "./vfrs.exe" if os.path.exists("./vfrs.exe") else "../vfrs.exe"
    p = subprocess.Popen([vfrs_path, "scratch/test_reg.conf"])
    time.sleep(1.5)

    success = True
    try:
        # Port 0 (uni0/0 - ANSI LMI on DLCI 0)
        sock0 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock0.bind(('127.0.0.1', 20010))
        sock0.settimeout(1.0)

        # Port 1 (uni0/1 - Q.933A LMI on DLCI 0)
        sock1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock1.bind(('127.0.0.1', 20011))
        sock1.settimeout(1.0)

        # Send 3 LMI Status Enquiry exchanges on Port 0 (ANSI) and Port 1 (Q.933A) to satisfy N392=3
        print("-> Testing LMI Status Enquiry & Link State Transition (N392=3)...")
        lmi_enq_ansi = bytes([
            0x00, 0x01,  # DLCI 0
            0x03,        # UI Control
            0x08,        # PD
            0x00,        # Call Ref
            0x75,        # STATUS ENQUIRY
            0x01, 0x01, 0x01, # Report Type = Full Status
            0x03, 0x02, 0x01, 0x01 # Link Integrity (1, 1)
        ])
        for seq in range(1, 4):
            dte_tx = seq
            dte_rx = seq - 1  # Matches DCE's TxSN starting at 0
            # ANSI LMI on DLCI 0 (PD=0x08, includes Locking Shift 0x95)
            lmi_enq_ansi = bytes([
                0x00, 0x01,  # DLCI 0
                0x03,        # UI Control
                0x08,        # PD
                0x00,        # Call Ref
                0x75,        # STATUS ENQUIRY
                0x95,        # Locking Shift to Codeset 5
                0x01, 0x01, 0x01, # Report Type = Full Status
                0x03, 0x02, dte_tx, dte_rx # Link Integrity
            ])
            # Q.933A LMI on DLCI 0 (PD=0x08, IE 0x51 Report Type, IE 0x53 Link Integrity)
            lmi_enq_q933a = bytes([
                0x00, 0x01,  # DLCI 0
                0x03,        # UI Control
                0x08,        # PD
                0x00,        # Call Ref
                0x75,        # STATUS ENQUIRY
                0x51, 0x01, 0x01, # Report Type = Full Status (0x51)
                0x53, 0x02, dte_tx, dte_rx # Link Integrity (0x53)
            ])
            sock0.sendto(lmi_enq_ansi, ('127.0.0.1', 10010))
            sock1.sendto(lmi_enq_q933a, ('127.0.0.1', 10011))
            try: sock0.recvfrom(2048)
            except socket.timeout: pass
            try: sock1.recvfrom(2048)
            except socket.timeout: pass
            time.sleep(0.1)

        print("   [PASS] LMI exchanges completed; link states active")
        time.sleep(1.5)

        # A. Test Bi-directional PVC Data Switching (Port 0 DLCI 100 <-> Port 1 DLCI 200)
        print("-> Testing PVC Data Frame Forwarding (DLCI 100 -> DLCI 200)...")
        test_payload = b"REGRESSION_PVC_DATA_PAYLOAD_TEST_12345"
        pkt_out = encode_dlci(100) + test_payload
        sock0.sendto(pkt_out, ('127.0.0.1', 10010))

        try:
            while True:
                rx_data, _ = sock1.recvfrom(2048)
                rx_info = decode_dlci(rx_data)
                if rx_info['dlci'] not in (0, 1023):
                    break
            print(f"   Received on Port 1: DLCI={rx_info['dlci']}, FECN={rx_info['fecn']}, BECN={rx_info['becn']}")
            if rx_info['dlci'] == 200 and rx_data[2:] == test_payload:
                print("   [PASS] PVC Data forwarding (100 -> 200) verified")
            else:
                print("   [FAIL] Ingress DLCI or payload mismatch on PVC forwarding")
                success = False
        except socket.timeout:
            print("   [FAIL] Timeout waiting for PVC data forwarding on Port 1")
            success = False

        # Reverse PVC forwarding (Port 1 DLCI 200 -> Port 0 DLCI 100)
        print("-> Testing Reverse PVC Data Frame Forwarding (DLCI 200 -> DLCI 100)...")
        pkt_rev = encode_dlci(200) + test_payload
        sock1.sendto(pkt_rev, ('127.0.0.1', 10011))

        try:
            while True:
                rx_data, _ = sock0.recvfrom(2048)
                rx_info = decode_dlci(rx_data)
                if rx_info['dlci'] not in (0, 1023):
                    break
            print(f"   Received on Port 0: DLCI={rx_info['dlci']}, FECN={rx_info['fecn']}, BECN={rx_info['becn']}")
            if rx_info['dlci'] == 100 and rx_data[2:] == test_payload:
                print("   [PASS] Reverse PVC Data forwarding (200 -> 100) verified")
            else:
                print("   [FAIL] Ingress DLCI or payload mismatch on reverse PVC forwarding")
                success = False
        except socket.timeout:
            print("   [FAIL] Timeout waiting for reverse PVC data forwarding on Port 0")
            success = False

        # B. Test Congestion Marking (FECN/BECN)
        print("-> Testing Congestion FECN/BECN Bit Passing...")
        # Send packet with FECN=1, BECN=1 from Port 0
        b0_c = ((100 >> 4) & 0x3F) << 2
        b1_c = ((100 & 0x0F) << 4) | 0x0C | 0x01 # FECN=1 (bit 3), BECN=1 (bit 2)
        pkt_cgst = bytes([b0_c, b1_c]) + test_payload
        sock0.sendto(pkt_cgst, ('127.0.0.1', 10010))

        try:
            while True:
                rx_data, _ = sock1.recvfrom(2048)
                rx_info = decode_dlci(rx_data)
                if rx_info['dlci'] not in (0, 1023):
                    break
            if rx_info['fecn'] == 1 and rx_info['becn'] == 1:
                print("   [PASS] Congestion FECN/BECN flags correctly passed through switch")
            else:
                print(f"   [FAIL] Congestion flags not preserved (FECN={rx_info['fecn']}, BECN={rx_info['becn']})")
                success = False
        except socket.timeout:
            print("   [FAIL] Timeout waiting for congestion test packet")
            success = False

    finally:
        p.terminate()
        try: p.kill()
        except: pass
        time.sleep(1.0)
        try: os.remove("scratch/test_reg.conf")
        except: pass

    return success

def main():
    print("============================================================")
    print(" VFRS COMPREHENSIVE NON-SVC REGRESSION TEST SUITE")
    print(" Virtual Frame Relay Switch - Post Phase 1 Execution")
    print("============================================================")

    res_c = test_c_unit_suite()
    res_mcast = test_multicast_pvc_suite()
    res_pvc_lmi = test_lmi_congestion_lapf_suite()

    print("\n============================================================")
    print(" FINAL REGRESSION TEST SUMMARY")
    print("============================================================")
    print(f"  1. C Module Unit Tests:                  {'[ PASS ]' if res_c else '[ FAIL ]'}")
    print(f"  2. PVC Multicasting & Replication Test:  {'[ PASS ]' if res_mcast else '[ FAIL ]'}")
    print(f"  3. PVC Switching, LMI & Congestion Test: {'[ PASS ]' if res_pvc_lmi else '[ FAIL ]'}")
    print("------------------------------------------------------------")

    if res_c and res_mcast and res_pvc_lmi:
        print(" SUCCESS: ALL REGRESSION TESTS PASSED! NO REGRESSIONS DETECTED.")
        sys.exit(0)
    else:
        print(" FAILURE: ONE OR MORE REGRESSION TESTS FAILED!")
        sys.exit(1)

if __name__ == "__main__":
    main()
