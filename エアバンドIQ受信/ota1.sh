#!/bin/sh
# 1号機 (10.5.2.20, rtl_tcp) に build/ を OTA
cd "$(dirname "$0")/../firmware/build" && make -j4 2>&1 | grep -E "error|Built target" && python3 ../../host/ota_push.py 10.5.2.20 rtltcp_rp2350.bin
