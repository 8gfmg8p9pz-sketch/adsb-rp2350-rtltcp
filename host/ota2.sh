#!/bin/sh
# 2号機 (10.5.2.21, scanner) に build2/ を OTA
cd "$(dirname "$0")/../firmware/build2" && make -j4 2>&1 | grep -E "error|Built target" && python3 ../../host/ota_push.py 10.5.2.21 rtltcp_rp2350.bin
