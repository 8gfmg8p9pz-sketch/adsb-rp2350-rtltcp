#!/bin/sh
# 5号機 (10.5.2.24, rtl_tcp) に build5/ を OTA
cd "$(dirname "$0")/../firmware/build5" && make -j4 2>&1 | grep -E "error|Built target" && python3 ../../host/ota_push.py 10.5.2.24 rtltcp_rp2350.bin
