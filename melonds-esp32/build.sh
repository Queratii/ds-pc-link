#!/bin/bash
# Construit melonDS avec l'interface multijoueur ESP32 de ds-pc-link.
#
# À lancer depuis le terminal « MSYS2 UCRT64 » (Windows) ou un shell Linux :
#   ./melonds-esp32/build.sh [dossier des sources melonDS]
#
# Par défaut, les sources sont récupérées dans ../melonDS-esp32 (à côté du
# projet) à la version testée ci-dessous, puis ESP32MP.cpp/.h y sont copiés
# et melonds.patch appliqué. Les dépendances de melonDS doivent être
# installées (voir melonds-esp32/README.md).
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$HERE/../../melonDS-esp32}"
# Version de melonDS sur laquelle le patch a été écrit et compilé.
MELONDS_COMMIT=1e654443e4c0a54b40709c0208aa0eeba41e6142

if [ ! -d "$SRC/.git" ]; then
    echo "== Récupération de melonDS dans $SRC"
    git clone https://github.com/melonDS-emu/melonDS.git "$SRC"
    git -C "$SRC" checkout "$MELONDS_COMMIT"
fi

echo "== Ajout de l'interface ESP32"
cp "$HERE/src/net/ESP32MP.h" "$HERE/src/net/ESP32MP.cpp" "$SRC/src/net/"
if git -C "$SRC" apply --check "$HERE/melonds.patch" 2>/dev/null; then
    git -C "$SRC" apply "$HERE/melonds.patch"
elif git -C "$SRC" apply --reverse --check "$HERE/melonds.patch" 2>/dev/null; then
    echo "   (patch déjà appliqué)"
else
    echo "Le patch ne s'applique pas : version de melonDS différente de $MELONDS_COMMIT ?" >&2
    exit 1
fi

echo "== Compilation"
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build" -j"$(nproc)"
echo "== Terminé : $SRC/build/melonDS(.exe)"
