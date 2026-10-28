# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic guard checks for selective FP packaging, no real game data."""
from pathlib import Path
import copy, io, json, tempfile, unittest, zipfile
from PIL import Image
import restore

class PackageGuards(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='fp-package-tests-')
        self.base=Path(self.temp.name);self.out=self.base/'imp';self.out.mkdir()
        im=Image.new('RGBA',(3,3));im.putpixel((1,1),(80,40,20,255));b=io.BytesIO();im.save(b,format='PNG');self.png=b.getvalue()
        frame={'file':'native.png','frame_w':3,'frame_h':3,'offset_x':1,'offset_y':2,'shadow_offset':1,'offset_w':0,'offset_h':0}
        self.entry={'name':'ARTIMP_HD_IDLE','rotatable':False,'td_resolution_scale':4,'fp_resolution_scale':1,'td':[[dict(frame,file='td.png')]],'fp':[[frame]]}
        self.held=copy.deepcopy(self.entry);self.held['name']='ARTIMP_HD_GRAB';self.held['fp_resolution_scale']=4
        self.archive=self.base/'original.zip'
        with zipfile.ZipFile(self.archive,'w') as z:
            z.writestr('sprites.json',json.dumps([self.entry,self.held]));z.writestr('td.png',self.png);z.writestr('native.png',self.png)
        info={'file':'references/idle.png','frame_size':[3,3],'anchor':[-1,-2],'metadata':frame,'original_member':'native.png','sha256':restore.digest(self.png)}
        (self.out/'references').mkdir();(self.out/'references/idle.png').write_bytes(self.png)
        self.config=self.base/'imp.cfg';self.config.write_text('Stand = ARTIMP_HD_IDLE\nGrab = ARTIMP_HD_GRAB\n')
        self.source={'baseline_manifest':'test-only','model':{'archive':str(self.archive),'archive_sha256':restore.digest(self.archive.read_bytes()),'config':str(self.config),'config_sha256':restore.digest(self.config.read_bytes())},
            'actions':[{'action':'idle','entry_name':'ARTIMP_HD_IDLE','sequences':[[info]],'entry':self.entry}]}
        restore.save_json(self.out/'source.json',self.source)
        restore.save_json(self.out/'generation-plan.json',{'jobs':[{'id':'idle-d0','action':'idle','cells':[[0,0]]}]})
        (self.out/'frames').mkdir();candidate=Image.new('RGBA',(12,12));candidate.paste((60,32,12,255),(2,2,10,10));candidate.save(self.out/'frames/idle.png')
        self.report={'job':'idle-d0','source_sha256':restore.digest((self.out/'source.json').read_bytes()),'frames':[{'direction':0,'frame':0,'original':'references/idle.png','hd':'frames/idle.png','sha256':restore.digest((self.out/'frames/idle.png').read_bytes())}]}
        self.save_report()
    def save_report(self):
        restore.save_json(self.out/'review/idle-d0-report.json',self.report)
        restore.save_json(self.out/'selection.json',{'jobs':{'idle-d0':{'visual_review':'pass','report_sha256':restore.digest((self.out/'review/idle-d0-report.json').read_bytes())}}})
    def tearDown(self):self.temp.cleanup()
    def run_pack(self):restore.package(self.base,self.base,'imp',self.base/'candidate.zip')
    def test_existing_pixels_and_td_held_are_exact(self):
        self.run_pack()
        with zipfile.ZipFile(self.archive) as original,zipfile.ZipFile(self.base/'candidate.zip') as new:
            for name in ('td.png','native.png'):self.assertEqual(original.read(name),new.read(name))
            entries=json.loads(new.read('sprites.json'));self.assertEqual(entries[0]['td'],self.entry['td']);self.assertEqual(entries[1],self.held)
            f=entries[0]['fp'][0][0];self.assertEqual([f[k] for k in ('frame_w','frame_h','offset_x','offset_y','shadow_offset')],[12,12,4,8,4])
    def test_missing_review_fails(self):
        restore.save_json(self.out/'selection.json',{'jobs':{}})
        with self.assertRaises(AssertionError):self.run_pack()
    def test_missing_frame_fails(self):
        self.report['frames']=[];self.save_report()
        with self.assertRaises(AssertionError):self.run_pack()
    def test_wrong_canvas_fails(self):
        Image.new('RGBA',(11,12)).save(self.out/'frames/idle.png');self.report['frames'][0]['sha256']=restore.digest((self.out/'frames/idle.png').read_bytes());self.save_report()
        with self.assertRaises(AssertionError):self.run_pack()
    def test_stale_report_fails(self):
        (self.out/'source.json').write_text(json.dumps(self.source,sort_keys=True))
        with self.assertRaises(AssertionError):self.run_pack()
    def test_replacement_hash_fails(self):
        (self.out/'frames/idle.png').write_bytes(b'changed')
        with self.assertRaises(AssertionError):self.run_pack()
    def test_existing_candidate_not_overwritten(self):
        (self.base/'candidate.zip').write_bytes(b'keep')
        with self.assertRaises(ValueError):self.run_pack()
        self.assertEqual((self.base/'candidate.zip').read_bytes(),b'keep')
    def test_modified_original_reference_fails(self):
        (self.out/'references/idle.png').write_bytes(b'changed original')
        with self.assertRaises(AssertionError):self.run_pack()
    def test_changed_config_fails(self):
        self.config.write_text('Stand = changed')
        with self.assertRaises(AssertionError):self.run_pack()
    def test_missing_plan_direction_fails(self):
        restore.save_json(self.out/'generation-plan.json',{'jobs':[{'id':'idle-d0','action':'idle','cells':[]}]})
        with self.assertRaises(AssertionError):self.run_pack()
    def test_changed_source_metadata_fails(self):
        self.source['actions'][0]['sequences'][0][0]['anchor']=[0,0]
        restore.save_json(self.out/'source.json',self.source)
        self.report['source_sha256']=restore.digest((self.out/'source.json').read_bytes());self.save_report()
        with self.assertRaises(AssertionError):self.run_pack()
    def test_opaque_replacement_fails(self):
        Image.new('RGBA',(12,12),(60,32,12,255)).save(self.out/'frames/idle.png')
        self.report['frames'][0]['sha256']=restore.digest((self.out/'frames/idle.png').read_bytes());self.save_report()
        with self.assertRaises(AssertionError):self.run_pack()
    def test_clipped_replacement_fails(self):
        im=Image.open(self.out/'frames/idle.png');im.putpixel((0,6),(60,32,12,255));im.save(self.out/'frames/idle.png')
        self.report['frames'][0]['sha256']=restore.digest((self.out/'frames/idle.png').read_bytes());self.save_report()
        with self.assertRaises(AssertionError):self.run_pack()
    def test_identical_inputs_package_identically(self):
        self.run_pack()
        restore.package(self.base,self.base,'imp',self.base/'candidate-again.zip')
        self.assertEqual((self.base/'candidate.zip').read_bytes(),(self.base/'candidate-again.zip').read_bytes())
    def test_ghost_rejected(self):
        with self.assertRaises(ValueError):restore.package(self.base,self.base,'ghost',self.base/'candidate.zip')

if __name__=='__main__':unittest.main()
