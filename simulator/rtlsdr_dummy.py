#!/usr/bin/env python3
import socket
import struct
import time
import argparse
import sys

class DummyRTLSDRServer:
    def __init__(self, host='127.0.0.1', port=1234, sample_rate=2400000, freq=1090000000):
        self.host = host
        self.port = port
        self.sample_rate = sample_rate
        self.freq = freq
        self.server_socket = None
        self.running = False

    def start(self):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind((self.host, self.port))
        self.server_socket.listen(1)
        self.running = True
        print(f"[RTL_TCP] Listening on {self.host}:{self.port}")
        try:
            while self.running:
                try:
                    self.server_socket.settimeout(1.0)
                    self.client_socket, addr = self.server_socket.accept()
                    print(f"[RTL_TCP] Client connected from {addr}")
                    self.handle_client()
                except socket.timeout:
                    continue
        finally:
            self.stop()

    def handle_client(self):
        try:
            self.send_tuner_header()
            self.stream_iq_data()
        except Exception as e:
            print(f"[RTL_TCP] Client error: {e}")
        finally:
            if self.client_socket:
                self.client_socket.close()

    def send_tuner_header(self):
        header = b'RTL0' + struct.pack('<B', 1) + struct.pack('<B', 50) + b'\x00' * 28
        self.client_socket.sendall(header)
        print(f"[RTL_TCP] Sent tuner header")

    def stream_iq_data(self):
        chunk_size = 16384
        packet_count = 0
        try:
            while self.running:
                iq_data = bytearray([0xFF if (i % 2) else 0x00 for i in range(chunk_size)])
                self.client_socket.sendall(iq_data)
                packet_count += 1
                if packet_count % 100 == 0:
                    print(f"[RTL_TCP] Sent {packet_count} packets")
                time.sleep(0.01)
        except:
            pass

    def stop(self):
        self.running = False
        if self.server_socket:
            self.server_socket.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Dummy RTL-SDR TCP Server')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=1234)
    args = parser.parse_args()
    server = DummyRTLSDRServer(host=args.host, port=args.port)
    try:
        server.start()
    except KeyboardInterrupt:
        server.stop()
