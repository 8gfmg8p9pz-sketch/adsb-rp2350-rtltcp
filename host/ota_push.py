#!/usr/bin/env python3
import socket, struct, sys, zlib, os, time
ip = sys.argv[1] if len(sys.argv) > 1 else "10.5.2.20"
path = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/adsb-rp2350-rtltcp/firmware/build/rtltcp_rp2350.bin")
data = open(path, "rb").read()
crc = zlib.crc32(data) & 0xFFFFFFFF
print(f"file : {path}  {len(data)} bytes  crc32={crc:08x}")
s = socket.create_connection((ip, 1235), timeout=10)
print("board:", s.recv(128).decode(errors="replace").strip())
s.sendall(b"OTA1" + struct.pack("<II", len(data), crc))
sent = 0
for i in range(0, len(data), 4096):
    chunk = data[i:i + 4096]
    s.sendall(chunk)
    sent += len(chunk)
    print(f"\rsent : {sent}/{len(data)}", end="", flush=True)
print()
s.settimeout(60)
resp = s.recv(64).decode(errors="replace").strip()
print("reply:", resp)
s.close()
if not resp.startswith("OK"):
    sys.exit(1)
print("rebooting, waiting for board ...")
time.sleep(3)
for _ in range(30):
    try:
        c = socket.create_connection((ip, 1235), timeout=2)
        print("back :", c.recv(128).decode(errors="replace").strip())
        c.close()
        break
    except OSError:
        time.sleep(1)
else:
    print("board did not come back (check LED / recover via USB BOOTSEL)")
