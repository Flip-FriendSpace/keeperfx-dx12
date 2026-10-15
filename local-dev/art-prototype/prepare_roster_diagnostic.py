# SPDX-License-Identifier: GPL-2.0-or-later
"""Write combined fixture scripts only. Execution requires runtime coordination."""
from pathlib import Path
here=Path(__file__).resolve().parent
base=(here/'test_heroes_benchmark.mjs').read_text()
prefix=base[:base.index('try {\n  socket=')]
prefix=prefix.replace("mode='Benchmark'", "mode='Diagnostic'")
prefix=prefix.replace('stage.camera=r.camera;', 'stage.allObjects=r.objects;stage.camera=r.camera;')
prefix=prefix.replace('o.class===5&&o.simulationPosition[0]>=47000&&o.simulationPosition[0]<=52500&&o.simulationPosition[1]>=47000&&o.simulationPosition[1]<=52500','o.class===5')
body=r'''try {
  const slugs=['wizard','barbarian','archer','monk','dwarfa','knight','avatar','tunneller','witch','giant','fairy','thief','samurai','imp'],actions=['idle','walk','attack','grab'];
  const metadata={};for(const slug of slugs.slice(0,-1))metadata[slug]=JSON.parse(await readFile(join(out,'..',slug,'source-manifest.json'),'utf8'));
  metadata.imp={idle:{td_animation:557},walk:{td_animation:555},attack:{td_animation:559},grab:{td_animation:575}};
  const symbol=(slug,action)=>slug==='imp'?'ARTIMP_HD_'+(action==='attack'?'DIG':action.toUpperCase()):'ARTHERO_'+slug.toUpperCase()+'_HD_'+action.toUpperCase();
  socket=await new Promise((resolve,reject)=>{const s=net.createConnection({host:'127.0.0.1',port:5599});s.once('error',reject);s.once('connect',()=>resolve(s));});
  socket.on('error',e=>{failure=e;pending?.reject(e);});socket.on('data',d=>{buffer+=d.toString().replaceAll('\0','');let r;try{r=JSON.parse(buffer);}catch{return;}buffer='';if(r.ack===pending?.ack)pending.resolve(r);});
  for(let i=0;i<120;i++){const level=await request('get_level_info'),game=await request('get_current_game_info');if(level.level_number===1&&game.game_turn>0){assert.equal(level.is_multiplayer,false);report.startTurn=game.game_turn;break;}await delay(250);}assert.ok(report.startTurn);
  await command('scenecheck overhead');await command('zoomto.subtilecoord 193 193');await delay(700);await command('pause');await waitPaused();
  await setup(`for _,t in ipairs(GetThingsOfClass("CREATURE")) do t:delete() end; for x=58,70 do for y=59,69 do ChangeSlabType(x,y,11);ChangeSlabOwner(x,y,PLAYER0) end end;RevealMapRect(PLAYER0,193,193,44,40); diagOriginal={};diagHD={};diagCreatures={};diagModels={${slugs.map(s=>'"'+s.toUpperCase()+'"').join(',')}};for i=0,13 do local x=180+i*2; for v=1,2 do local y=188+(v-1)*5;local t=AddObjectToLevelAtPos("GOLD_CHEST",x,y,0,PLAYER0,0);assert(t);t.sprite_size=300;t.sprite_size_min=300;t.sprite_size_max=300;t.anim_speed=256;t.pos={val_x=x*256+128,val_y=y*256+128,val_z=256};table.insert(v==1 and diagOriginal or diagHD,t) end;local c=AddCreatureToLevel(PLAYER0,diagModels[i+1],{stl_x=x,stl_y=198},1,0);assert(c);c.state="ManualControl";assert(c.sprite_size==300);table.insert(diagCreatures,c) end;assert(#diagCreatures==14);assert(AddObjectToLevelAtPos("CANDLESTCK",190,191,0,PLAYER0,0));assert(AddObjectToLevelAtPos("TORCH",196,191,0,PLAYER0,0))`);
  await command('scenecheck reference clear-ui');await command('scenecheck reference short-walls 0');const camera='scenecheck reference camera 49536 49152 0 0 -266 2600 94';
  const ids={};
  for(const action of actions){
    await setup(`local originals={${slugs.map(s=>metadata[s][action].td_animation).join(',')}};local hd={${slugs.map(s=>'"'+symbol(s,action)+'"').join(',')}};for i=1,14 do diagOriginal[i].anim_sprite=originals[i];diagHD[i].anim_sprite=hd[i];diagCreatures[i].anim_sprite=hd[i] end`);
    for(const [heading,angle] of [1024,768,512,256,0,1792,1536,1280].entries()){
      await setup(`for i=1,14 do diagOriginal[i].orientation=${angle};diagHD[i].orientation=${angle};diagCreatures[i].orientation=${angle} end`);
      await step();await command(camera);const stage=await receipt(action+'-heading-'+heading,false);
      assert.equal(stage.creatures.length,14);assert.equal(new Set(stage.creatures.map(c=>c.model)).size,14);assert.ok(stage.creatures.every(c=>c.size===300));
      for(let i=0;i<14;i++){const x=(180+i*2)*256+128;const find=y=>stage.allObjects.find(o=>o.class===1&&o.model===3&&o.simulationPosition[0]===x&&o.simulationPosition[1]===y*256+128);const orig=find(188),hd=find(193);assert.ok(orig&&hd,slugs[i]+' visible controls');assert.equal(orig.animation,metadata[slugs[i]][action].td_animation);assert.ok(hd.animation>1000);assert.equal(orig.size,hd.size);ids[slugs[i]+':'+action]=hd.animation;}
    }

  }
  // Terrain replacement deletes gold-chest controls; perform it after all retained controls are used.
  await setup(`for x=59,69 do ChangeSlabType(x,64,"WATER") end;local hd={${slugs.map(s=>'"'+symbol(s,'grab')+'"').join(',')}};for i=1,14 do local x=180+(i-1)*2;local t=AddObjectToLevelAtPos("GOLD_CHEST",x,193,0,PLAYER0,0);assert(t);t.anim_sprite=hd[i];t.sprite_size=300;t.sprite_size_min=300;t.sprite_size_max=300;t.anim_speed=256;t.pos={val_x=x*256+128,val_y=193*256+128,val_z=256} end`);await step();await command(camera);await receipt('grab-water-cutoff',false);
  report.loadedAnimationIds=ids;assert.equal(Object.keys(ids).length,56);report.scope='All14 models share loaded archives and rendering caches. Original/HD controls cover idle/walk/attack/grab at8 headings with large sprites, realcreatures, dynamic light/shadow and water cutoff. Forced motion controls; no natural combat behavior claim.';report.finalStats=await stats();report.passed=true;await command('quit');
}catch(e){report.error=e.message;throw e;}
finally{socket?.destroy();await writeFile(join(out,'fixture-report.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({passed:report.passed,error:report.error,stages:report.stages.length}));}
'''
(here/'test_heroes_diagnostic.mjs').write_text(prefix+body)
ps=(here/'test_heroes_benchmark.ps1').read_text()
ps=ps.replace("[ValidateSet('Benchmark')][string]$Mode='Benchmark'", "[ValidateSet('Diagnostic')][string]$Mode='Diagnostic'")
ps=ps.replace('keeperfx-heroes-benchmark.cfg','keeperfx-heroes-diagnostic.cfg').replace("$clientScript='test_heroes_benchmark.mjs'","$clientScript='test_heroes_diagnostic.mjs'")
ps=ps.replace('150)', '300)')
ps=ps.replace("'KFX_DX12_PERFORMANCE')", "'KFX_DX12_PERFORMANCE','KFX_DX12_GPU_VALIDATION')")
ps=ps.replace("  $game=Start-Process", "  $env:KFX_DX12_GPU_VALIDATION='1'\n  $game=Start-Process")
(here/'test_heroes_diagnostic.ps1').write_text(ps,encoding='utf-8')
