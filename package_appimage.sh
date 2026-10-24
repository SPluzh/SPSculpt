#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== [1/5] Building SPSculpt Release ==="
mkdir -p build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

echo "=== [2/5] Preparing AppDir Structure ==="
rm -rf AppDir
mkdir -p AppDir/usr/bin
mkdir -p AppDir/usr/lib
mkdir -p AppDir/usr/share/applications
mkdir -p AppDir/usr/share/icons/hicolor/256x256/apps

# Ensure 256x256 icon exists
if [ ! -f resources/icons/spsculpt_256.png ]; then
    if which convert >/dev/null 2>&1; then
        convert resources/icons/app_icon.png -resize 256x256 resources/icons/spsculpt_256.png
    else
        cp resources/icons/app_icon.png resources/icons/spsculpt_256.png
    fi
fi

echo "=== [3/5] Deploying dependencies with linuxdeploy ==="
TOOLS_DIR="$HOME/tools"
mkdir -p "$TOOLS_DIR"
if [ ! -f "$TOOLS_DIR/linuxdeploy-x86_64.AppImage" ]; then
    wget -q -nc -O "$TOOLS_DIR/linuxdeploy-x86_64.AppImage" https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x "$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
fi

if [ ! -f "$TOOLS_DIR/appimagetool-x86_64.AppImage" ]; then
    wget -q -nc -O "$TOOLS_DIR/appimagetool-x86_64.AppImage" https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOLS_DIR/appimagetool-x86_64.AppImage"
fi

if [ ! -f "$TOOLS_DIR/runtime-x86_64" ]; then
    wget -q -nc -O "$TOOLS_DIR/runtime-x86_64" https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
fi

cp build/SPSculpt AppDir/usr/bin/

"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" --appimage-extract-and-run \
    --appdir AppDir \
    -e build/SPSculpt \
    -l build/libmanifold.so.2 \
    -l build/libClipper2.so.1 \
    -l build/libpolygon.so \
    -d resources/spsculpt.desktop \
    -i resources/icons/spsculpt_256.png \
    --icon-filename spsculpt

echo "=== [4/5] Bundling Resources and Custom AppRun ==="
# Ensure binary was not overwritten by any symlink
cp -f build/SPSculpt AppDir/usr/bin/SPSculpt

# Bundle resources
cp -r resources AppDir/usr/bin/
rm -f AppDir/usr/bin/resources/app.rc AppDir/usr/bin/resources/AppRun

if [ -d presets ]; then
    cp -r presets AppDir/usr/bin/
fi

# Replace default AppRun symlink with custom launcher script
rm -f AppDir/AppRun
cp resources/AppRun AppDir/AppRun
chmod +x AppDir/AppRun

echo "=== [5/5] Generating AppImage with universal static runtime ==="
export ARCH=x86_64
VERSION="0.3.11"
OUTPUT_NAME="SPSculpt-${VERSION}-x86_64.AppImage"

"$TOOLS_DIR/appimagetool-x86_64.AppImage" --appimage-extract-and-run \
    --runtime-file "$TOOLS_DIR/runtime-x86_64" \
    AppDir "$OUTPUT_NAME"

echo "=========================================================="
echo " SUCCESS! AppImage generated: $OUTPUT_NAME"
ls -lh "$OUTPUT_NAME"
echo "=========================================================="
