# SPDX-License-Identifier: GPL-2.0-or-later
"""TEST ONLY large PNG import fixture; canvas padding is not finished hero art."""
from pathlib import Path
import io,json,zipfile
from PIL import Image
ROOT=Path(__file__).resolve().parents[3];HERE=Path(__file__).resolve().parent
OUT=ROOT/'artifacts/art-prototype/heroes/engine-large-sprites';MOD=OUT/'mod/large_sprite_test'
(MOD/'fxdata').mkdir(parents=True,exist_ok=True);(MOD/'creatrs').mkdir(exist_ok=True)
base=ROOT/'artifacts/art-prototype/imp-actions/mod/imp_art_prototype/fxdata/imp-sprites.zip'
with zipfile.ZipFile(base) as old,zipfile.ZipFile(MOD/'fxdata/imp-sprites.zip','w',zipfile.ZIP_DEFLATED) as archive:
    entries=json.loads(old.read('sprites.json'))
    for entry in entries:
        for view in ('td','fp'):
            for direction in entry[view]:
                for frame in direction:
                    image=Image.open(io.BytesIO(old.read(frame['file']))).convert('RGBA')
                    if entry['name']=='ARTIMP_HD_IDLE' and view=='td':
                        canvas=Image.new('RGBA',(512,512));canvas.alpha_composite(image);image=canvas
                    elif entry['name']=='ARTIMP_HD_GRAB' and view=='fp':
                        canvas=Image.new('RGBA',(596,248));canvas.alpha_composite(image);image=canvas
                    frame['frame_w']=image.width;frame['frame_h']=image.height
                    buf=io.BytesIO();image.save(buf,format='PNG');archive.writestr(frame['file'],buf.getvalue())
    # Deliberately rejected, unused entry proves dimension checks run before
    # assigning frame metadata or allocating decoder/source memory.
    bad=Image.new('RGBA',(1025,1),(255,255,255,255));buf=io.BytesIO();bad.save(buf,format='PNG');archive.writestr('reject-1025.png',buf.getvalue())
    entries.append({'name':'ARTLARGE_REJECT_1025','rotatable':False,'td':[[{'file':'reject-1025.png'}]],'fp':[[{'file':'reject-1025.png'}]]})
    archive.writestr('sprites.json',json.dumps(entries))
(MOD/'creatrs/imp.cfg').write_text('[sprites]\nStand = ARTIMP_HD_IDLE\nPowerGrab = ARTIMP_HD_GRAB\n')
(MOD/'local-prototype.json').write_text(json.dumps({'test_only':True,'local_only':True,'purpose':'Exact512TD and596FP PNG import fixture. Existing local imp artwork padded only; not finished hero artwork.'}))
ps=(HERE/'test_imp_set.ps1').read_text()
ps=ps.replace("[ValidateSet('imp-set','imp-actions')][string]$AssetSet='imp-set'","[string]$AssetSet='heroes/engine-large-sprites',\n  [ValidateSet('DX12','OPENGL','SOFTWARE')][string]$Renderer='DX12'")
ps=ps.replace("$executable=Join-Path $runtime 'keeperfx-dx12.exe'","$executable=Join-Path $runtime $(if($Renderer -eq 'DX12') {'keeperfx-dx12.exe'} else {'keeperfx-local.exe'})")
ps=ps.replace('imp_art_prototype','large_sprite_test').replace("RENDERER='DX12'","RENDERER=$Renderer").replace("$clientScript=if ($Mode -eq 'Actions') {'test_imp_actions.mjs'} else {'test_imp_set.mjs'}","$clientScript='test_large_sprites.mjs'")
ps=ps.replace('$Mode,$Variant)','$Mode,$Variant,$Renderer)').replace("if (-not $validation.initialized -or -not $validation.worldSubmitted -or $validation.submitted -le 0 -or $validation.rejected -ne 0)","if ($Renderer -eq 'DX12' -and (-not $validation.initialized -or -not $validation.worldSubmitted -or $validation.submitted -le 0 -or $validation.rejected -ne 0))")
(HERE/'test_large_sprites.ps1').write_text(ps)
