# SPDX-License-Identifier: GPL-2.0-or-later
"""Bind completed local large-sprite checks to current executable/source hashes."""
from pathlib import Path
import hashlib,json
from datetime import datetime,timezone
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'artifacts/art-prototype/heroes/engine-large-sprites'
FILES={
'src/custom_sprites.c':'Reject PNG extents outside1..1024 before decoding; decoded-size error checked; existing RLE capacity retained.',
'src/console_cmd.c':'Paused local reference mouse placement only, using existing reference/cheat gate and screen bounds.',
'src/engine_render.c':'Create shadows up to512; large SOFTWARE shadow masks use logical coverage and divided UVs, retaining projected world vertices and full creature pixels.',
'src/kfx/renderer/KeeperShadowMask.h':'Guarded512-to-logical256 coverage helper for software fallback only.',
'src/kfx/renderer/OwnedWorldDrawSink.cpp':'Keeper sources admit1024; padded RGBA slots adapt256/512/1024; additive scan uses actual slot stride.',
'src/kfx/renderer/OwnedRenderFrame.cpp':'Owned sprite validator admits1024 source axes.',
'src/kfx/renderer/SceneAssets.cpp':'GL sprite capture validator admits1024 and full512 shadow coverage; existing codec fields and UV units retained.',
'src/kfx/renderer/d3d12/Dx12Device.cpp':'Runtime sprite asset admission and enhanced draws admit1024; existing bounded page allocator retained.',
'src/kfx/renderer/d3d12/RuntimeWorldValidation.h':'Runtime source rectangles admit1024 and retain leased image bounds checks.',
'src/kfx/renderer/d3d12/EnhancedWorldPass.h':'Captured/enhanced shadow coverage admits512.',
'src/kfx/renderer/d3d12/EnhancedWorld.hlsl':'Shadow UV units remain pixel/256; clamp extends511 for512 sources, source rectangle still clips.',
'src/kfx/renderer/d3d12/RuntimeWorld.h':'Comment records adaptive padded dimensions; no struct changes.',
'src/kfx/renderer/opengl/GLWorldViewRenderer.cpp':'1024 decode scratch/fallback texture,512 atlas uploads with1024 scratch stride,512 capture coverage; oversize frames bypass atlas.',
'src/kfx/renderer/opengl/GLWorldViewRenderer.h':'512 atlas layers of512x512 RG8 preserve prior256MiB atlas allocation.',
'src/kfx/renderer/opengl/GLShaders.h':'Shadow texel loads preserve pixel/256 UVs on resized atlas.',
'local-dev/keeper-sprite-decode-test.cpp':'Production decode boundary cases254/255/256/508/512/596/1024, guards and4000 legacy comparisons.',
'local-dev/keeper-large-shadow-test.cpp':'Production logical shadow helper checks508 and mirrored164x268 coverage, padding, invalid extents and guard bytes.',
'local-dev/scene-assets-test.cpp':'Oversized GL sprite rejection boundary updated1025; canonical fixtures remain identical.',
'local-dev/dx12/EnhancedSpriteOperatorTests.cpp':'Enhanced sprite rejection boundary updated1025.'}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
latest={}
for path in sorted(OUT.glob('Validation-HD-*')):
    if not (path/'fixture-report.json').exists():continue
    fixture=json.loads((path/'fixture-report.json').read_text())
    if fixture.get('passed') and (path/'run-report.json').exists():
        run=json.loads((path/'run-report.json').read_text())
        if run.get('passed') and run.get('settingsRestored'):latest[fixture['renderer']]={'root':str(path.relative_to(ROOT)),'stages':len(fixture['stages']),'run':run}
assert set(latest)=={'DX12','OPENGL','SOFTWARE'}, latest
codec=json.loads((ROOT/'artifacts/scene-assets/report.json').read_text());assert codec['passed']
report={'checkedAt':datetime.now(timezone.utc).isoformat(),'status':'scoped_large_sprite_support_verified','testOnly':True,'heroArtworkAcceptance':False,
 'limits':{'customSpriteAxes':1024,'worldShadowAxes':512,'originalHeroTDMaximumRequiredAxis':508,'knightHeldRequiredAxis':596},
 'normalWorldSize':'Resolution flags retain4x texture detail at original projection size; sprite_size unchanged.',
 'diskAndSaveLayouts':'Unchanged; no native or disk struct changes. Canonical capture v1-v11 fixtures cross-read x86/x64 and fixed legacy hashes passed.',
 'runtimeFixtures':latest,'unitChecks':{'largeDecoder':'passed; guarded boundaries254/255/256/508/512/596/1024 plus4000 randomized legacy comparisons','softwareShadow':'passed; mirrored coverage,512 source limits, transparent padding and guard bytes'},
 'codecEvidence':'artifacts/scene-assets/report.json',
 'visualInspection':'Centered 596-pixel held sprite inspected intact at unchanged displayed body size in DX12, OPENGL and SOFTWARE. The 512-pixel overhead sprite and full coverage ground shadow were inspected in DX12 and OPENGL. Software logical shadow coverage has production-helper guard and mirror checks; its in-game extent was exercised without overflow.',
 'tradeoffs':['OpenGL atlas remains256MiB but cache capacity drops2048to512 entries. Broad campaign/crowd GL performance is not qualified.','OpenGL1024 fallback is used for sprite axes>512; held frames avoid larger persistent atlas allocation.','DX12 sprite slots grow256/512/1024 only for the source that needs them, within existing admission/eviction budgets.','SOFTWARE only reduces large shadow coverage to logical resolution; full-colour creature textures remain4x.','Test source padding deliberately changes empty canvas bounds; these diagnostic sprites are not finished hero artwork or hero integration acceptance.'],
 'executables':{name:sha(ROOT/f'runtime/keeperfx-x64/{name}') for name in ('keeperfx-dx12.exe','keeperfx-local.exe')},
 'packageSha256':sha(OUT/'mod/large_sprite_test/fxdata/imp-sprites.zip'),
 'changedSources':[{ 'path':'keeperfx/'+file,'sha256':sha(ROOT/'keeperfx'/file),'change':description} for file,description in FILES.items()],
 'commands':['pwsh -File keeperfx/local-dev/build.ps1 -DX12 -Jobs 6','pwsh -File keeperfx/local-dev/build.ps1 -Jobs 6','pwsh -File keeperfx/local-dev/smoke.ps1 -Renderer DX12/OPENGL/SOFTWARE (sequential runs)','pwsh -File keeperfx/local-dev/art-prototype/test_large_sprites.ps1 -Renderer DX12/OPENGL/SOFTWARE (sequential runs)','pwsh -File keeperfx/local-dev/scene-assets-test.ps1','keeperfx-dev g++ -std=c++20 guarded decoder and large shadow test binaries']}
(OUT/'engine-support-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'report':str(OUT/'engine-support-report.json'),'limits':report['limits'],'executables':report['executables'],'fixtures':{k:v['root'] for k,v in latest.items()}},indent=2))
