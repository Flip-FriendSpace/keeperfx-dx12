# SPDX-License-Identifier: GPL-2.0-or-later
"""Reuse the established owned-runtime test wrapper for per-hero validation."""
from pathlib import Path
HERE=Path(__file__).resolve().parent
client=(HERE/'test_imp_actions.mjs').read_text(encoding='utf-8')
prefix=client.split("  const names=['hit'")[0]
prefix=prefix.replace("const [out,mode='Validation',variant='HD']=process.argv.slice(2);assert.ok(out);", "const [out,hero]=process.argv.slice(2);assert.ok(out);assert.match(hero,/^[a-z_]+$/);\nconst mode='HeroValidation',variant='HD';\nconst root=join(out,'..');\nconst meta=JSON.parse(await readFile(join(root,'source-manifest.json'),'utf8'));\nconst plan=JSON.parse(await readFile(join(root,'generation-plan.json'),'utf8'));\nconst names=Object.keys(meta);")
prefix=prefix.replace("Remaining imp actions: full forced heading/frame coverage, real creature instances, slap, collapse, death and pickup. No natural hauling claim.","All hero actions: forced heading/frame coverage, creature instances, walking, slap, collapse, death, possession and pickup. No natural combat or hauling claim.")
prefix=prefix.replace('objects.length,18','objects.length,Object.keys(meta).length*2')
(HERE/'test_hero.mjs').write_text(prefix+(HERE/'hero_fixture_body.mjs.txt').read_text(encoding='utf-8'),encoding='utf-8')
wrapper=(HERE/'test_imp_set.ps1').read_text(encoding='utf-8')
start=wrapper.index("$ErrorActionPreference")
wrapper="param([Parameter(Mandatory=$true)][ValidatePattern('^[a-z_]+$')][string]$Hero)\n$Mode='HeroValidation';$Variant='HD'\n"+wrapper[start:]
wrapper=wrapper.replace("('artifacts/art-prototype/'+$AssetSet)","('artifacts/art-prototype/heroes/'+$Hero)")
wrapper=wrapper.replace("$modTarget=Join-Path $runtime 'mods/imp_art_prototype'", "$modName='hero_'+$Hero+'_art_prototype'\n$modTarget=Join-Path $runtime ('mods/'+$modName)")
wrapper=wrapper.replace("'keeperfx-imp-art.cfg'","'keeperfx-hero-art.cfg'").replace('-config keeperfx-imp-art.cfg','-config keeperfx-hero-art.cfg')
wrapper=wrapper.replace("if ($order -match '(?m)^imp_art_prototype\\s*$')", "if ($order -match ('(?m)^'+[regex]::Escape($modName)+'\\s*$'))")
wrapper=wrapper.replace("(Join-Path $art 'mod/imp_art_prototype/*')", "(Join-Path $art ('mod/'+$modName+'/*'))")
wrapper=wrapper.replace('"`r`nimp_art_prototype`r`n"','("`r`n"+$modName+"`r`n")')
wrapper='\n'.join(line for line in wrapper.splitlines() if "if ($Variant -eq 'Original')" not in line)
wrapper=wrapper.replace("$clientScript=if ($Mode -eq 'Actions') {'test_imp_actions.mjs'} else {'test_imp_set.mjs'}", "$clientScript='test_hero.mjs'")
wrapper=wrapper.replace("$Mode,$Variant)","$Hero)").replace('TotalSeconds -gt 150','TotalSeconds -gt 480')
wrapper=wrapper.replace("  Copy-Item -LiteralPath (Join-Path $art 'mod/imp_art_prototype/creatrs/imp.cfg') -Destination (Join-Path $modTarget 'creatrs/imp.cfg') -Force",'')
wrapper=wrapper.replace("(Join-Path $art 'mod/imp_art_prototype/fxdata/imp-sprites.zip')", "(Join-Path $art ('mod/'+$modName+'/fxdata/'+$Hero+'-sprites.zip'))")
wrapper=wrapper.replace('imp art test','hero art test').replace('Imp sprite evidence:','Hero sprite evidence:').replace('Imp prototype','Hero prototype')
(HERE/'test_hero.ps1').write_text(wrapper+'\n',encoding='utf-8')
print('Created test_hero.mjs and test_hero.ps1; no runtime mutation.')
