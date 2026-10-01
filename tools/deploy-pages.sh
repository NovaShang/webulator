#!/bin/bash
# Build the demo site and publish it as the orphan gh-pages branch (force-pushed; it has no history to keep).
set -euo pipefail
cd "$(dirname "$0")/.."
npm run build
node tools/build-site.mjs
SRC=$(git rev-parse --short HEAD)
TMP=$(mktemp -d)
cp -R site/. "$TMP"
cd "$TMP"
git init -q -b gh-pages
git config core.autocrlf false
git add -A
git commit -q -m "Live demo site (built from main $SRC with tools/build-site.mjs)"
# Every file must be stored byte for byte.
git ls-tree -r -l HEAD | while IFS=$'\t' read -r meta file; do
  size=$(echo "$meta" | awk '{print $4}')
  [ "$size" = "$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")" ] || { echo "size mismatch: $file"; exit 1; }
done
git push -f -q git@github.com:NovaShang/webulator.git gh-pages
echo "pushed gh-pages from $SRC"
