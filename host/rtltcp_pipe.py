#!/usr/bin/env python3
"""
rtltcp_pipe.py -- rtl_tcp クライアント。RP2350 に接続して周波数/レート/ゲインを設定し、
受信した生 IQ (uint8 I,Q 交互) を標準出力に流す。統計は標準エラーに出す。

例:
  python3 rtltcp_pipe.py 192.168.1.150 --freq 1090000000 --rate 2400000 --gain 49.6 \
      | readsb --device-type ifile --ifile /dev/stdin --iformat UC8 ...

  python3 rtltcp_pipe.py 192.168.1.150 --rate 250000 --bench 10   # 10秒間の転送レート計測のみ
"""
import argparse
import socket
import struct
import sys
import time

CMD_FREQ, CMD_RATE, CMD_GAIN_MODE, CMD_GAIN, CMD_PPM = 0x01, 0x02, 0x03, 0x04, 0x05
CMD_AGC, CMD_DIRECT, CMD_BIAS_TEE = 0x08, 0x09, 0x0e

TUNER_NAMES = {0: "unknown", 1: "E4000", 2: "FC0012", 3: "FC0013", 4: "FC2580", 5: "R820T", 6: "R828D"}


def send_cmd(sock, cmd, param):
    sock.sendall(struct.pack(">BI", cmd, int(param) & 0xFFFFFFFF))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--freq", type=int, default=1090000000, help="center frequency [Hz]")
    ap.add_argument("--rate", type=int, default=250000, help="sample rate [S/s]")
    ap.add_argument("--gain", type=float, default=None, help="tuner gain [dB] (省略時 AGC)")
    ap.add_argument("--ppm", type=int, default=0)
    ap.add_argument("--bias-tee", action="store_true")
    ap.add_argument("--bench", type=float, default=0, help="秒数を指定すると stdout に流さず転送レートだけ計測")
    ap.add_argument("--out", default=None, help="生 IQ をファイルにも保存")
    a = ap.parse_args()

    s = socket.create_connection((a.host, a.port), timeout=10)
    s.settimeout(5)
    hdr = b""
    while len(hdr) < 12:
        chunk = s.recv(12 - len(hdr))
        if not chunk:
            sys.exit("connection closed before header")
        hdr += chunk
    magic, tuner, ngains = struct.unpack(">4sII", hdr)
    if magic != b"RTL0":
        sys.exit(f"bad magic {magic!r}")
    print(f"[rtltcp_pipe] connected: tuner={TUNER_NAMES.get(tuner, tuner)} gains={ngains}", file=sys.stderr)

    send_cmd(s, CMD_RATE, a.rate)
    send_cmd(s, CMD_FREQ, a.freq)
    send_cmd(s, CMD_PPM, a.ppm)
    if a.gain is None:
        send_cmd(s, CMD_GAIN_MODE, 0)
        send_cmd(s, CMD_AGC, 1)
    else:
        send_cmd(s, CMD_GAIN_MODE, 1)
        send_cmd(s, CMD_GAIN, int(round(a.gain * 10)))
    send_cmd(s, CMD_BIAS_TEE, 1 if a.bias_tee else 0)

    out = sys.stdout.buffer if not a.bench else None
    fout = open(a.out, "wb") if a.out else None
    t0 = time.time()
    tlast = t0
    total = 0
    last_total = 0
    acc = 0
    n = 0
    try:
        while True:
            data = s.recv(65536)
            if not data:
                print("[rtltcp_pipe] server closed connection", file=sys.stderr)
                break
            total += len(data)
            if out:
                out.write(data)
            if fout:
                fout.write(data)
            # 簡易レベル統計 (中心 127.5 からの平均偏差)
            for b in data[::997]:
                acc += abs(b - 127.5)
                n += 1
            now = time.time()
            if now - tlast >= 2.0:
                rate = (total - last_total) / (now - tlast)
                lvl = acc / n if n else 0
                print(f"[rtltcp_pipe] {rate/1024:8.1f} kB/s  = {rate/2/1000:7.1f} kS/s  "
                      f"(要求 {a.rate/1000:.0f} kS/s, 達成率 {rate/2/a.rate*100:5.1f}%)  level {lvl:5.1f}",
                      file=sys.stderr)
                tlast = now
                last_total = total
                acc = 0
                n = 0
            if a.bench and now - t0 >= a.bench:
                break
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        if fout:
            fout.close()
    el = time.time() - t0
    print(f"[rtltcp_pipe] total {total/1024/1024:.2f} MB in {el:.1f}s = {total/el/1024:.1f} kB/s", file=sys.stderr)


if __name__ == "__main__":
    main()
