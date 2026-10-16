#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run real decoder/presentation contracts without a game or GPU."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--ui-packs', type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
results=[]
for source in ['local-dev/remaster/ui-pack-test.cpp','local-dev/remaster/raw-index-test.c',
               'local-dev/remaster/emissive-test.cpp','local-dev/remaster/lighting-contract-test.cpp',
               'local-dev/keeper-shadow-coverage-test.cpp','local-dev/keeper-large-shadow-test.cpp']:
    s=root/source; output=a.output/s.stem
    cpp=s.suffix=='.cpp'
    subprocess.run(['g++' if cpp else 'gcc', '-std=c++20' if cpp else '-std=c11',
                    '-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I'+str(root/'src'),str(s),'-o',str(output)],check=True)
    args=[str(output.resolve())]
    if s.stem=='ui-pack-test' and a.ui_packs: args += [str(f) for f in sorted(a.ui_packs.glob('*.kfui'))]
    run=subprocess.run(args,check=True,text=True,capture_output=True)
    print(run.stdout,end='')
    results.append({'source':source,'sha256':hashlib.sha256(s.read_bytes()).hexdigest(),'passed':True,'output':run.stdout})
    if s.stem=='emissive-test':
        for value,expected in [('', 'disabled'),('source-v1','enabled'),('1','disabled'),('invalid','disabled')]:
            env=dict(os.environ);env.pop('KFX_DX12_EMISSIVE',None)
            if value:env['KFX_DX12_EMISSIVE']=value
            subprocess.run([str(output.resolve()),expected],env=env,check=True)
(a.output/'report.json').write_text(json.dumps({'passed':True,'gpu_validation':False,'tests':results},indent=2)+'\n')
