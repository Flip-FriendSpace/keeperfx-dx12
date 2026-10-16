#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
workspace="${1:-$(dirname -- "$repo")}"
cache="$workspace/downloads/keeperfx-dx12"
build=/opt/keeperfx-build/dx12
deps="$build/deps"
out="$workspace/artifacts/dx12/bin"
mkdir -p "$deps" "$out/D3D12" "$out/licenses"
# Reverify before extraction/execution, including direct invocations of this script.
python3 - "$repo/local-dev/dependencies-dx12.json" "$cache" "$deps" <<'PY'
import hashlib, json, pathlib, sys, tarfile, zipfile
manifest, cache, deps = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
for item in json.loads(manifest.read_text(encoding='utf-8-sig')):
    archive = cache / item['file']
    if hashlib.sha256(archive.read_bytes()).hexdigest().upper() != item['sha256'].upper():
        raise RuntimeError('Dependency hash mismatch: ' + archive.name)
    name = item['name'].split('-')[0]
    target = deps / name
    marker = target / '.verified-sha256'
    if marker.exists() and marker.read_text() == item['sha256']:
        continue
    target.mkdir(parents=True, exist_ok=True)
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as z:
            for entry in z.infolist():
                if not (target / entry.filename).resolve().is_relative_to(target.resolve()):
                    raise RuntimeError('Invalid ZIP path')
            z.extractall(target)
    else:
        with tarfile.open(archive) as t:
            members = t.getmembers()
            if name in ('headers', 'allocator'):
                for entry in members:
                    entry.name = '/'.join(entry.name.split('/')[1:])
                members = [entry for entry in members if entry.name]
            t.extractall(target, members=members, filter='data')
    marker.write_text(item['sha256'])
PY
cp "$deps/agility/build/native/bin/x64/D3D12Core.dll" "$out/D3D12/"
cp "$deps/agility/build/native/bin/x64/d3d12SDKLayers.dll" "$out/D3D12/"
cp "$deps/agility/LICENSE.txt" "$out/licenses/Agility-SDK.txt"
cp "$deps/agility/LICENSE-CODE.txt" "$out/licenses/Agility-code.txt"
cp "$deps/headers/LICENSE" "$out/licenses/DirectX-Headers.txt"
cp "$deps/allocator/LICENSE.txt" "$out/licenses/D3D12MA.txt"
# DXC is a build dependency only. Preserve all its license notices in the cache.
"$deps/dxc/bin/dxc" --version
