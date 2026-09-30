#!/bin/bash
# Assemble a relocatable, host-independent Mercurygram AppImage carrying its own
# shared Qt + QtWebEngine, so the in-app webview works without any host
# WebKitGTK/GTK or system Qt. Meant to run INSIDE the WEBENGINE-flavour
# centos_env container, where the shared Qt is installed under /usr/local and
# patchelf/qmake are available.
#
# The lib-closure walk, the AppImage excludelist, the $ORIGIN rpaths, the
# generated AppRun + qt.conf, the Qt plugin/QML deploy and the QtWebEngine
# helper/resources/locales are all handled by linuxdeploy + linuxdeploy-plugin-qt
# off the binary's real linkage. The only bespoke piece is the Chromium sandbox
# apprun-hook (a relocatable image cannot ship a setuid chrome-sandbox).
#
# Usage: linux-appimage.sh <Mercurygram-binary> <Updater-binary> <out-AppImage>
#
# Tools (fetched + verified by the CI job, or on PATH for a local run):
#   LINUXDEPLOY    path to linuxdeploy-<arch>.AppImage      (default: linuxdeploy)
#   APPIMAGETOOL   path to appimagetool-<arch>.AppImage     (default: appimagetool)
# linuxdeploy locates its qt plugin as `linuxdeploy-plugin-qt` next to itself or
# on PATH, so the fetch step drops all three in the same directory.
set -euo pipefail

BIN=${1:?Mercurygram binary path required}
UPD=${2:?Updater binary path required}
OUT=${3:?output AppImage path required}

QTROOT=${QTROOT:-/usr/local}
LINUXDEPLOY=${LINUXDEPLOY:-linuxdeploy}
APPIMAGETOOL=${APPIMAGETOOL:-appimagetool}
APPDIR=${APPDIR:-AppDir}

# No FUSE inside the container: run each tool AppImage by self-extracting.
export APPIMAGE_EXTRACT_AND_RUN=1
# appimagetool cannot always infer the target arch from a stripped AppDir.
export ARCH=${ARCH:-$(uname -m)}
# Point linuxdeploy-plugin-qt at the modded shared Qt under $QTROOT.
export QMAKE="$QTROOT/bin/qmake"
test -x "$QMAKE" || { echo "qmake not found at $QMAKE"; exit 1; }
# Let linuxdeploy find linuxdeploy-plugin-qt when it sits next to it.
case "$LINUXDEPLOY" in
    */*) ld_dir=$(cd "$(dirname "$LINUXDEPLOY")" && pwd); export PATH="$ld_dir:$PATH" ;;
esac

echo "== assembling AppDir in $APPDIR (Qt from $QTROOT) =="
rm -rf "$APPDIR"

# --- Custom AppRun: Chromium sandbox handling ---------------------------------
# linuxdeploy otherwise symlinks AppRun straight at the binary (no script runs),
# so the sandbox logic has to be an explicit --custom-apprun. A relocatable image
# cannot ship a setuid chrome-sandbox, so it relies on the unprivileged
# user-namespace sandbox and disables the sandbox only when namespaces are
# unavailable. Override with MERCURYGRAM_WEBENGINE_SANDBOX=1 (force keep) / =0
# (force disable). Qt itself resolves plugins/qml/webengine via the qt.conf
# linuxdeploy-plugin-qt drops next to the binary, so no env wiring is needed here.
apprun=$(mktemp)
cat > "$apprun" <<'EOF'
#!/bin/bash
HERE=$(dirname "$(readlink -f "$0")")
case "${MERCURYGRAM_WEBENGINE_SANDBOX:-auto}" in
    0) export QTWEBENGINE_DISABLE_SANDBOX=1 ;;
    1) : ;;
    *) unshare -U true 2>/dev/null || export QTWEBENGINE_DISABLE_SANDBOX=1 ;;
esac
exec "$HERE/usr/bin/Mercurygram" "$@"
EOF
chmod +x "$apprun"

# --- Populate the AppDir off the binaries' real linkage -----------------------
# linuxdeploy: copies the transitive lib closure (minus its built-in AppImage
# excludelist), sets $ORIGIN rpaths, installs the desktop entry + icon and our
# custom AppRun. --plugin qt: deploys the Qt platform/TLS/imageformat plugins,
# the QML trees, and (detecting the QtWebEngine module) QtWebEngineProcess + its
# *.pak / icudtl.dat / qtwebengine_locales, plus a matching qt.conf.
"$LINUXDEPLOY" --appdir "$APPDIR" \
    -e "$BIN" \
    -e "$UPD" \
    -d lib/xdg/it.belloworld.mercurygram.desktop \
    -i Telegram/Resources/art/icon256.png --icon-filename it.belloworld.mercurygram \
    --custom-apprun "$apprun" \
    --plugin qt
rm -f "$apprun"

# --- Guard: ensure the Chromium data files made it in -------------------------
# linuxdeploy-plugin-qt deploys these from the qmake-reported Qt, but if a layout
# mismatch makes it miss one, copy it from $QTROOT into the conventional plugin-qt
# locations so the webview still has its resources. Loud on every fallback.
find_in_appdir() { find "$APPDIR" -name "$1" -print -quit; }

if [ -z "$(find_in_appdir QtWebEngineProcess)" ]; then
    src="$QTROOT/libexec/QtWebEngineProcess"; [ -f "$src" ] || src="$QTROOT/bin/QtWebEngineProcess"
    if [ -f "$src" ]; then
        echo "  guard: QtWebEngineProcess missing, copying from $src"
        mkdir -p "$APPDIR/usr/libexec"; cp "$src" "$APPDIR/usr/libexec/QtWebEngineProcess"
    else
        echo "::warning::QtWebEngineProcess not deployed and not found under $QTROOT"
    fi
fi
if [ -z "$(find_in_appdir icudtl.dat)" ] && [ -d "$QTROOT/resources" ]; then
    echo "  guard: QtWebEngine resources missing, copying from $QTROOT/resources"
    mkdir -p "$APPDIR/usr/resources"; cp "$QTROOT"/resources/*.pak "$QTROOT/resources/icudtl.dat" "$APPDIR/usr/resources/" 2>/dev/null || true
fi
if [ -z "$(find_in_appdir qtwebengine_locales)" ] && [ -d "$QTROOT/translations/qtwebengine_locales" ]; then
    echo "  guard: qtwebengine_locales missing, copying from $QTROOT/translations"
    mkdir -p "$APPDIR/usr/translations"; cp -r "$QTROOT/translations/qtwebengine_locales" "$APPDIR/usr/translations/"
fi

# --- Pack into a single self-mounting AppImage --------------------------------
mkdir -p "$(dirname "$OUT")"
"$APPIMAGETOOL" "$APPDIR" "$OUT"
chmod +x "$OUT"

echo "== AppImage assembled: $OUT =="
