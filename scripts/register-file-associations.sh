#!/usr/bin/env bash
#
# Registers ModelViewer as the handler for the 3D formats it reads.
#
# Four things have to happen, and skipping any one of them leaves the
# association silently not working:
#
#   1. The MIME types must exist. Most 3D formats have no registered media
#      type, so a stock system does not know what a .fbx is and there is
#      nothing to associate against.
#   2. The MIME database must be rebuilt so the new types are known.
#   3. The .desktop file must be installed and the desktop database rebuilt.
#   4. The types must be set as defaults, which is a separate step from merely
#      declaring the app can open them.
#
# Installs per-user into ~/.local by default; pass a prefix to change that.
#
#   ./scripts/register-file-associations.sh              # ~/.local
#   sudo ./scripts/register-file-associations.sh /usr/local
#
set -euo pipefail

PREFIX="${1:-$HOME/.local}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MIME_DIR="$PREFIX/share/mime"
APP_DIR="$PREFIX/share/applications"
ICON_DIR="$PREFIX/share/icons/hicolor/256x256/apps"
BIN_DIR="$PREFIX/bin"

DESKTOP_ID="modelviewer.desktop"

TYPES=(
    model/gltf+json
    model/gltf-binary
    model/obj
    model/stl
    model/vnd.collada+xml
    model/x.fbx
    model/x.ply
    model/x.usd
    model/vnd.usdz+zip
    model/x.3ds
    model/x.off
)

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m warning:\033[0m %s\n' "$*" >&2; }

# --- locate the executable -------------------------------------------------
EXEC_PATH=""
for candidate in \
    "$BIN_DIR/ModelViewer" \
    "$HERE/build/linux-dist/ModelViewer" \
    "$HERE/build/linux-release/ModelViewer" \
    "$HERE/build/linux-debug/ModelViewer"
do
    if [[ -x "$candidate" ]]; then EXEC_PATH="$candidate"; break; fi
done

if [[ -z "$EXEC_PATH" ]]; then
    echo "Could not find a ModelViewer binary." >&2
    echo "Build one first, or run 'cmake --install <build-dir> --prefix $PREFIX'." >&2
    exit 1
fi
say "Using executable: $EXEC_PATH"

# --- 1 + 2: MIME types -----------------------------------------------------
say "Installing MIME type definitions"
mkdir -p "$MIME_DIR/packages"
install -m644 "$HERE/resources/modelviewer-mime.xml" "$MIME_DIR/packages/"

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "$MIME_DIR"
else
    warn "update-mime-database not found (install shared-mime-info); associations will not work"
fi

# --- 3: desktop entry + icon ----------------------------------------------
say "Installing desktop entry"
mkdir -p "$APP_DIR" "$ICON_DIR"
sed "s|@MV_EXEC_PATH@|$EXEC_PATH|g" \
    "$HERE/resources/modelviewer.desktop.in" > "$APP_DIR/$DESKTOP_ID"
chmod 644 "$APP_DIR/$DESKTOP_ID"
install -m644 "$HERE/resources/icon.png" "$ICON_DIR/modelviewer.png"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APP_DIR"
else
    warn "update-desktop-database not found (install desktop-file-utils)"
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

# --- 4: set as default -----------------------------------------------------
# xdg-mime default writes to the *calling user's* ~/.config/mimeapps.list.
# Run under sudo it sets root's defaults, which is almost never what anyone
# wants, so say so rather than appear to succeed.
if [[ ${EUID} -eq 0 && -n "${SUDO_USER:-}" ]]; then
    warn "running as root: defaults will be set for root, not $SUDO_USER"
    warn "re-run without sudo to set defaults for your own user"
fi

say "Setting ModelViewer as the default handler"
if command -v xdg-mime >/dev/null 2>&1; then
    for type in "${TYPES[@]}"; do
        xdg-mime default "$DESKTOP_ID" "$type"
        printf '    %s\n' "$type"
    done
else
    warn "xdg-mime not found (install xdg-utils); types are declared but not defaulted"
fi

# --- report ----------------------------------------------------------------
echo
say "Done. Verify with:"
echo "    xdg-mime query filetype yourmodel.fbx"
echo "    xdg-mime query default  model/x.fbx"
echo
echo "A running file manager may need restarting before it notices:"
echo "    nautilus -q      # GNOME Files"
echo "    killall dolphin  # KDE"
