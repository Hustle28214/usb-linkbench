#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN="$ROOT/.toolchain/bin"
SRC="$ROOT/.toolchain/root"
mkdir -p "$BIN"
if [ -x "$SRC/usr/bin/x86_64-w64-mingw32-g++-posix" ]; then
    cat > "$BIN/x86_64-w64-mingw32-g++" << EOF
#!/bin/sh
export GCC_EXEC_PREFIX="$SRC/usr/lib/gcc/"
exec "$SRC/usr/bin/x86_64-w64-mingw32-g++-posix" -B"$SRC/usr/bin" --sysroot="$SRC/usr/x86_64-w64-mingw32" "\$@"
EOF
    cat > "$BIN/x86_64-w64-mingw32-gcc" << EOF
#!/bin/sh
export GCC_EXEC_PREFIX="$SRC/usr/lib/gcc/"
exec "$SRC/usr/bin/x86_64-w64-mingw32-gcc-posix" -B"$SRC/usr/bin" --sysroot="$SRC/usr/x86_64-w64-mingw32" "\$@"
EOF
    cat > "$BIN/x86_64-w64-mingw32-windres" << EOF
#!/bin/sh
exec "$SRC/usr/bin/x86_64-w64-mingw32-windres" -D_WIN32 --include-dir "$SRC/usr/x86_64-w64-mingw32/include" "\$@"
EOF
    chmod +x "$BIN/x86_64-w64-mingw32-g++" "$BIN/x86_64-w64-mingw32-gcc" "$BIN/x86_64-w64-mingw32-windres"
    export PATH="$BIN:$PATH"
    export MINGW_ROOT="$BIN"
fi

LIB_ROOT="$ROOT/third_party/libusb"
header="$LIB_ROOT/include/libusb-1.0/libusb.h"
static_lib="$LIB_ROOT/lib/libusb-1.0.a"
if [ ! -f "$header" ] || [ ! -f "$static_lib" ]; then
    mkdir -p "$ROOT/third_party"
    archive="$ROOT/third_party/libusb-1.0.27.tar.bz2"
    if [ ! -f "$archive" ]; then
        wget -O "$archive" "https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2"
    fi
    rm -rf "$ROOT/third_party/libusb-1.0.27"
    tar -xf "$archive" -C "$ROOT/third_party"
    (
        cd "$ROOT/third_party/libusb-1.0.27"
        ./configure --host=x86_64-w64-mingw32 --prefix="$LIB_ROOT" --enable-static --disable-shared
        make -j"$(nproc)"
        make install
    )
fi

cmake -S "$ROOT" -B "$ROOT/build-win" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GUI=OFF \
    -DLIBUSB_INCLUDE_DIR="$LIB_ROOT/include/libusb-1.0" \
    -DLIBUSB_LIBRARY="$static_lib" \
    -DLIBUSB_DLL=""
cmake --build "$ROOT/build-win" --config Release
if command -v wine >/dev/null 2>&1; then
    ctest --test-dir "$ROOT/build-win" --output-on-failure || true
fi

stage="$ROOT/dist/usb-linkbench-windows-x64"
rm -rf "$stage"
mkdir -p "$stage"
cp "$ROOT/build-win/usb-linkbench.exe" "$stage/"
if [ -f "$ROOT/build-win/libusb-1.0.dll" ]; then
    cp "$ROOT/build-win/libusb-1.0.dll" "$stage/"
fi
cp "$ROOT/docs/benchmark-report.md" "$stage/"
cp "$ROOT/contrib/windows/README.txt" "$stage/README.txt"
cp "$ROOT/LICENSE-MIT" "$ROOT/LICENSE-APACHE" "$stage/"
(
    cd "$ROOT/dist"
    rm -f usb-linkbench-windows-x64.zip
    zip -r usb-linkbench-windows-x64.zip usb-linkbench-windows-x64
)
echo "已生成 $ROOT/dist/usb-linkbench-windows-x64.zip"
