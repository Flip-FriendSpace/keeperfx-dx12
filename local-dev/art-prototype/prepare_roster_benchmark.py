# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare scripts only; never start the game or change runtime."""
from pathlib import Path

here = Path(__file__).resolve().parent
base = (here / 'test_imp_set.mjs').read_text(encoding='utf-8-sig')
prefix = base[:base.index('try {\n  socket=')]
prefix = prefix.replace("const [out,mode='Validation',variant='HD']", "const [out,mode='Benchmark',variant='HD']")
prefix = prefix.replace("stage.creatures=r.objects.filter(o=>o.class===5);", "stage.creatures=r.objects.filter(o=>o.class===5&&o.simulationPosition[0]>=47000&&o.simulationPosition[0]<=52500&&o.simulationPosition[1]>=47000&&o.simulationPosition[1]<=52500);")
body = '''try {
  socket=await new Promise((resolve,reject)=>{const s=net.createConnection({host:'127.0.0.1',port:5599});s.once('error',reject);s.once('connect',()=>resolve(s));});
  socket.on('error',e=>{failure=e;pending?.reject(e);});socket.on('data',d=>{buffer+=d.toString().replaceAll('\\0','');let r;try{r=JSON.parse(buffer);}catch{return;}buffer='';if(r.ack===pending?.ack)pending.resolve(r);});
  for(let i=0;i<120;i++){const level=await request('get_level_info'),game=await request('get_current_game_info');if(level.level_number===1&&game.game_turn>0){assert.equal(level.is_multiplayer,false);report.startTurn=game.game_turn;break;}await delay(250);}
  assert.ok(report.startTurn);await command('scenecheck overhead');await command('zoomto.subtilecoord 193 193');await delay(700);await command('pause');await waitPaused();
  const models=['WIZARD','BARBARIAN','ARCHER','MONK','DWARFA','KNIGHT','AVATAR','TUNNELLER','WITCH','GIANT','FAIRY','THIEF','SAMURAI','IMP'];
  report.models=models;report.expectedCrowd=168;report.scope='Matched 168-creature original versus HD scene. Both variants retain all HD archives loaded; measures drawing/simulation cost, not cold loading or total-memory savings.';
  await setup(`for _,t in ipairs(GetThingsOfClass("CREATURE")) do t:delete() end; for x=59,69 do for y=59,69 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0); if x==59 or x==69 or y==59 or y==69 then ChangeSlabType(x,y,"HARD") end end end; RevealMapRect(PLAYER0,193,193,40,40); artModels={${models.map(m=>'"'+m+'"').join(',')}}; artCrowd={}; for i=0,167 do local t=AddCreatureToLevel(PLAYER0,artModels[(i%14)+1],{stl_x=186+(i%14),stl_y=186+math.floor(i/14)},1,0); assert(t); t.state="ManualControl"; t.orientation=(i%8)*256; table.insert(artCrowd,t) end; assert(#artCrowd==168); for _,x in ipairs({185,193,201}) do for _,y in ipairs({185,193,201}) do assert(AddObjectToLevelAtPos("CANDLESTCK",x,y,0,PLAYER0,0)) end end`);
  await command('scenecheck reference camera 49536 49152 0 0 -266 4200 94');await command('scenecheck reference clear-ui');await command('scenecheck reference short-walls 0');
  const before=await receipt('crowd-before',false);assert.equal(before.creatures.length,168,'Every crowd member must be visible');
  const counts={};for(const c of before.creatures){assert.equal(c.size,300);counts[c.model]=(counts[c.model]??0)+1;}assert.equal(Object.keys(counts).length,14);assert.ok(Object.values(counts).every(n=>n===12));report.visibleModels=counts;report.beforeStats=await stats();
  await command('pause');await command('renderbench live');let completed;
  for(let n=0;n<160;n++){try{completed=JSON.parse(await readFile(join(out,'dx12-live.json'),'utf8'));break;}catch(e){if(e.code!=='ENOENT'&&!(e instanceof SyntaxError))throw e;}await delay(250);}
  assert.ok(completed?.passed);assert.equal(completed.debug,false);assert.equal(completed.gpuValidation,false);report.benchmark=completed;
  const lines=(await readFile(join(out,'dx12-live.csv'),'utf8')).trim().split(/\\r?\\n/),headers=lines.shift().split(',');const rows=lines.map(line=>Object.fromEntries(line.split(',').map((v,i)=>[headers[i],Number(v)])));
  assert.equal(rows.length,completed.rows);assert.ok(rows.every(r=>r.diagnostic===0&&r.palette_replay===0));
  function summary(key){const values=rows.map(r=>r[key]).sort((a,b)=>a-b);assert.ok(values.every(Number.isFinite));return {median:values[Math.floor(values.length/2)],p95:values[Math.floor(values.length*.95)],mean:values.reduce((a,b)=>a+b,0)/values.length};}
  report.timings=Object.fromEntries(['producer_wall_ms','preparation_submit_cpu_ms','gpu_upload_render_ms','admission_interval_ms'].map(k=>[k,summary(k)]));
  await command('pause');await waitPaused();const after=await receipt('crowd-after',false);assert.equal(after.creatures.length,168);const afterCounts={};for(const c of after.creatures){assert.equal(c.size,300);afterCounts[c.model]=(afterCounts[c.model]??0)+1;}assert.deepEqual(afterCounts,counts);report.finalStats=await stats();report.passed=true;await command('quit');
} catch(e){report.error=e.message;throw e;}
finally{socket?.destroy();await writeFile(join(out,'fixture-report.json'),JSON.stringify(report,null,2)+'\\n');console.log(JSON.stringify({passed:report.passed,error:report.error,stages:report.stages.length}));}
'''
(here / 'test_heroes_benchmark.mjs').write_text(prefix + body, encoding='utf-8')

ps = (here / 'test_imp_set.ps1').read_text(encoding='utf-8-sig')
ps = ps.replace("[ValidateSet('Validation','Benchmark','Actions')][string]$Mode='Validation'", "[ValidateSet('Benchmark')][string]$Mode='Benchmark'")
ps = ps.replace(",\n  [ValidateSet('imp-set','imp-actions')][string]$AssetSet='imp-set'", "")
ps = ps.replace("('artifacts/art-prototype/'+$AssetSet)", "'artifacts/art-prototype/heroes'")
ps = ps.replace('imp_art_prototype', 'heroes_art_prototype')
ps = ps.replace('keeperfx-imp-art.cfg', 'keeperfx-heroes-benchmark.cfg')
ps = ps.replace('Imp', 'Heroes').replace('imp art', 'hero roster')
old = '''  if ($Variant -eq 'Original') {[IO.File]::WriteAllText((Join-Path $modTarget 'creatrs/imp.cfg'),"[sprites]`nStand = 556`nAmbulate = 554`nDig = 558`n")}'''
new = '''  $roster=@('wizard','barbarian','archer','monk','dwarfa','knight','avatar','tunneller','witch','giant','fairy','thief','samurai','imp')
  $creatureBackups=@{}
  foreach ($slug in $roster) {
    $target=Join-Path $modTarget ('creatrs/'+$slug+'.cfg')
    if (-not (Test-Path -LiteralPath $target)) {throw "Combined package missing creature config: $slug"}
    $creatureBackups[$target]=[IO.File]::ReadAllBytes($target)
    if ($Variant -eq 'Original') {
      $source=Get-Content -LiteralPath (Join-Path $workspace ('keeperfx/config/creatrs/'+$slug+'.cfg')) -Raw
      $section=[regex]::Match($source,'(?ms)^\\[sprites\\]\\s*$(.*?)(?=^\\[|\\z)')
      if (-not $section.Success) {throw "Missing original sprite section: $slug"}
      [IO.File]::WriteAllText($target,$section.Value)
    }
  }'''
assert old in ps
ps = ps.replace(old,new)
ps = ps.replace("  $clientScript=if ($Mode -eq 'Actions') {'test_imp_actions.mjs'} else {'test_imp_set.mjs'}", "  $clientScript='test_heroes_benchmark.mjs'")
ps = ps.replace("  Copy-Item -LiteralPath (Join-Path $art 'mod/heroes_art_prototype/creatrs/imp.cfg') -Destination (Join-Path $modTarget 'creatrs/imp.cfg') -Force", "  if ($creatureBackups) {foreach ($path in $creatureBackups.Keys) {[IO.File]::WriteAllBytes($path,$creatureBackups[$path])}}")
ps = ps.replace("packageSHA256=(Get-FileHash (Join-Path $art 'mod/heroes_art_prototype/fxdata/imp-sprites.zip')).Hash", "packageHashes=@(Get-ChildItem -LiteralPath (Join-Path $art 'mod/heroes_art_prototype/fxdata') -Filter '*.zip' | ForEach-Object {@{file=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}});comparison='Both variants keep HD archives loaded; original overrides only sprite mappings, not archive residency.'")
(here / 'test_heroes_benchmark.ps1').write_text(ps, encoding='utf-8')
