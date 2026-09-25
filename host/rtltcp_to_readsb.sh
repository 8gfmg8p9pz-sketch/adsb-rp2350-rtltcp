#!/bin/bash
# rtltcp_to_readsb.sh -- RP2350 の rtl_tcp から IQ を受け取り、readsb でデコードして
# 既設の readsb/tar1090 (Beast 入力 30004) に流し込む。
#
#   使い方:  ./rtltcp_to_readsb.sh <RP2350のIP> [既設readsbのホスト(既定 127.0.0.1)]
#
# ※ USB1.1 の帯域制限により 2.4 MS/s は達成できないため、ADS-B のデコードは
#    ほぼ成立しません (検証用)。転送率は rtltcp_pipe.py の stderr に出ます。
set -u
RP2350_IP="${1:?RP2350 の IP アドレスを指定してください}"
BEAST_HOST="${2:-127.0.0.1}"
DIR="$(cd "$(dirname "$0")" && pwd)"

DECODER=""
if command -v readsb >/dev/null 2>&1; then
  DECODER="readsb"
elif command -v dump1090-fa >/dev/null 2>&1; then
  DECODER="dump1090-fa"
else
  echo "readsb も dump1090-fa も見つかりません" >&2
  exit 1
fi
echo "decoder: $DECODER  ->  beast_out ${BEAST_HOST}:30004" >&2

exec python3 "$DIR/rtltcp_pipe.py" "$RP2350_IP" --freq 1090000000 --rate 2400000 --gain 49.6 \
  | "$DECODER" --device-type ifile --ifile /dev/stdin --iformat UC8 \
      --net --net-ri-port 0 --net-ro-port 0 --net-sbs-port 0 --net-bi-port 0 --net-bo-port 0 \
      --net-connector "${BEAST_HOST},30004,beast_out" --quiet
