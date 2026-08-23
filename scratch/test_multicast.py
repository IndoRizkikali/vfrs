import socket
import subprocess
import time
import sys
import os

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

def main():
    print("=== PVC Multicasting Verification Test ===")
    
    # Clean old logs
    if os.path.exists("test_mcast.log"):
        try: os.remove("test_mcast.log")
        except: pass

    # Start VFRS switch
    print("Starting VFRS with test configuration...")
    # Executable path is relative to scratch folder
    vfrs_path = "../vfrs.exe" if os.path.exists("../vfrs.exe") else "./vfrs.exe"
    p = subprocess.Popen([vfrs_path, "scratch/test_mcast.conf"])
    
    # Sockets setup
    # uni0/0: binds to local 20001, sends to remote 10001
    # uni0/1: binds to local 20002, sends to remote 10002
    # uni0/2: binds to local 20003, sends to remote 10003
    
    sock0 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock0.bind(('127.0.0.1', 20001))
    sock0.settimeout(1.0)
    
    sock1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock1.bind(('127.0.0.1', 20002))
    sock1.settimeout(1.0)
    
    sock2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock2.bind(('127.0.0.1', 20003))
    sock2.settimeout(1.0)
    
    # Wait for VFRS to start and bind
    time.sleep(1.5)
    
    success = True
    try:
        # ----------------------------------------------------
        # TEST 1: One-way root to leaves
        # ----------------------------------------------------
        print("\nTest 1: One-way root to leaves...")
        payload = b"Hello Oneway"
        pkt = encode_dlci(100) + payload
        sock0.sendto(pkt, ('127.0.0.1', 10001))
        
        # Check leaf 1 (uni0/1 on port 20002)
        try:
            data, addr = sock1.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/1 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 201 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on leaf 1")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for leaf 1")
            success = False
            
        # Check leaf 2 (uni0/2 on port 20003)
        try:
            data, addr = sock2.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/2 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 202 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on leaf 2")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for leaf 2")
            success = False

        # ----------------------------------------------------
        # TEST 2: One-way leaf drop
        # ----------------------------------------------------
        print("\nTest 2: One-way leaf frames must be dropped...")
        pkt = encode_dlci(201) + b"Leaf reply should be dropped"
        sock1.sendto(pkt, ('127.0.0.1', 10002))
        
        # Verify no receipt on root or other leaf
        try:
            sock0.recvfrom(1024)
            print("  [FAIL] Root received frame sent by one-way leaf!")
            success = False
        except socket.timeout:
            print("  [PASS] Root did not receive one-way leaf frame.")
            
        try:
            sock2.recvfrom(1024)
            print("  [FAIL] Leaf 2 received frame sent by leaf 1 in one-way!")
            success = False
        except socket.timeout:
            print("  [PASS] Leaf 2 did not receive one-way leaf frame.")

        # ----------------------------------------------------
        # TEST 3: Two-way root to leaves
        # ----------------------------------------------------
        print("\nTest 3: Two-way root to leaves...")
        payload = b"Hello Twoway"
        pkt = encode_dlci(500) + payload
        sock0.sendto(pkt, ('127.0.0.1', 10001))
        
        # Check leaf 1
        try:
            data, addr = sock1.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/1 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 601 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on leaf 1")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for leaf 1")
            success = False
            
        # Check leaf 2
        try:
            data, addr = sock2.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/2 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 602 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on leaf 2")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for leaf 2")
            success = False

        # ----------------------------------------------------
        # TEST 4: Two-way leaf to root (and not to other leaves)
        # ----------------------------------------------------
        print("\nTest 4: Two-way leaf to root...")
        payload = b"Leaf reply"
        pkt = encode_dlci(601) + payload
        sock1.sendto(pkt, ('127.0.0.1', 10002))
        
        # Check root
        try:
            data, addr = sock0.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/0 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 500 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on root")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for root")
            success = False
            
        # Verify leaf 2 received nothing
        try:
            sock2.recvfrom(1024)
            print("  [FAIL] Leaf 2 received frame sent by leaf 1 in two-way!")
            success = False
        except socket.timeout:
            print("  [PASS] Leaf 2 did not receive two-way leaf frame.")

        # ----------------------------------------------------
        # TEST 5: N-way peers
        # ----------------------------------------------------
        print("\nTest 5: N-way member to others...")
        payload = b"N-way broadcast"
        pkt = encode_dlci(800) + payload
        sock0.sendto(pkt, ('127.0.0.1', 10001))
        
        # Check member 1 (uni0/1)
        try:
            data, addr = sock1.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/1 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 801 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on member 1")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for member 1")
            success = False
            
        # Check member 2 (uni0/2)
        try:
            data, addr = sock2.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/2 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 802 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on member 2")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for member 2")
            success = False
            
        # Verify sender did not receive it
        try:
            sock0.recvfrom(1024)
            print("  [FAIL] Sender received its own N-way copy!")
            success = False
        except socket.timeout:
            print("  [PASS] Sender did not receive its own copy.")

        # Send from member 1 (uni0/1)
        print("\nTest 5b: N-way other member to peers...")
        payload = b"Peer frame"
        pkt = encode_dlci(801) + payload
        sock1.sendto(pkt, ('127.0.0.1', 10002))
        
        # Check member 0 (uni0/0)
        try:
            data, addr = sock0.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/0 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 800 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on member 0")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for member 0")
            success = False
            
        # Check member 2 (uni0/2)
        try:
            data, addr = sock2.recvfrom(1024)
            info = decode_dlci(data)
            print(f"  uni0/2 received: DLCI={info['dlci']}, payload={data[2:]}")
            if info['dlci'] != 802 or data[2:] != payload:
                print("  [FAIL] Incorrect DLCI or payload on member 2")
                success = False
        except socket.timeout:
            print("  [FAIL] Timeout waiting for member 2")
            success = False
            
        # Verify sender did not receive it
        try:
            sock1.recvfrom(1024)
            print("  [FAIL] Sender 2 received its own N-way copy!")
            success = False
        except socket.timeout:
            print("  [PASS] Sender 2 did not receive its own copy.")

    finally:
        # Shutdown
        print("\nShutting down VFRS switch...")
        p.terminate()
        p.wait()
        
    if success:
        print("\n=== [ALL TESTS PASSED SUCCESSFULLY] ===")
        sys.exit(0)
    else:
        print("\n=== [SOME TESTS FAILED] ===")
        sys.exit(1)

if __name__ == "__main__":
    main()
