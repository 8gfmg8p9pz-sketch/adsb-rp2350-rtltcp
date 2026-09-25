#!/bin/bash
# build.sh -- Mac (Homebrew の gcc-arm-embedded + ~/.pico-sdk/pico-sdk) でビルドして
#             firmware/build/rtltcp_rp2350.uf2 を作る
set -e
cd "$(dirname "$0")"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/.pico-sdk/pico-sdk}"
if [ ! -d "$PICO_SDK_PATH" ]; then
  echo "pico-sdk が見つかりません: $PICO_SDK_PATH" >&2
  exit 1
fi
if [ ! -f "$PICO_SDK_PATH/lib/tinyusb/src/tusb.h" ]; then
  echo "TinyUSB サブモジュールを取得します..." >&2
  git -C "$PICO_SDK_PATH" submodule update --init lib/tinyusb
fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  echo "arm-none-eabi-gcc が見つかりません (brew install --cask gcc-arm-embedded)" >&2
  exit 1
fi

GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"

mkdir -p build
cmake -S . -B build -G "$GEN" -DPICO_SDK_PATH="$PICO_SDK_PATH" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo
echo "==== 完成: $(pwd)/build/rtltcp_rp2350.uf2 ===="
ls -la build/rtltcp_rp2350.uf2
