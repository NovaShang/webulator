#!/bin/bash
# Upload the disk manifests and chunks the profiles use to an R2 bucket (default webulator-demo), for the demo site.
# Chunks are content-addressed, so they are cached for a year; manifests for five minutes.
# The bucket needs public access and a CORS rule allowing the site's origin (GET/HEAD, header Range).
set -euo pipefail
cd "$(dirname "$0")/../assets"
BUCKET=${1:-webulator-demo}
DISKS=$(node -e 'const fs=require("fs");const s=new Set();for(const f of fs.readdirSync("../profiles"))for(const d of JSON.parse(fs.readFileSync("../profiles/"+f)).disks)s.add(d.source.manifest.split("/").slice(-2)[0]);console.log([...s].join(" "))')
put() { npx -y wrangler@4 r2 object put "$BUCKET/$1" --file "$1" --remote --content-type "$2" --cache-control "$3" >/dev/null; }
export -f put; export BUCKET
# -n1 with "$1" rather than -I{}: BSD xargs caps -I replacements at 255 bytes.
for d in $DISKS; do /usr/bin/find "disks/$d/chunks" -type f; done |
  xargs -P 16 -n 1 bash -c 'put "$1" application/octet-stream "public, max-age=31536000, immutable" || { echo "failed: $1"; exit 255; }' _
for d in $DISKS; do put "disks/$d/manifest.json" application/json "public, max-age=300"; done
echo "uploaded: $DISKS"
