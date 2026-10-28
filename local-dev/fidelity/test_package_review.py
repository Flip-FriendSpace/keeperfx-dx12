# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused immutable review-builder guards using retained synthetic fixtures only."""
from pathlib import Path
import copy, hashlib, importlib.util, json, os, tempfile, unittest

SPEC = importlib.util.spec_from_file_location('package_review', Path(__file__).with_name('package-review.py'))
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def put(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)
    return {'file': path.name, 'sha256': M.digest(path), 'bytes': len(value)}


class ReviewBuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = os.environ.get('KEEPERFX_TEST_OUTPUT')
        if parent:
            parent = Path(parent).resolve()
            parent.mkdir(parents=True, exist_ok=True)
        cls.run_root = Path(tempfile.mkdtemp(prefix='keeperfx-review-tests-', dir=parent))
        print('Retained synthetic fixture root:', cls.run_root)

    def setUp(self):
        self.w = self.run_root / self._testMethodName
        self.w.mkdir()
        self.integration = self.w / 'artifacts/remaster-integration'
        self.runtime = self.w / 'runtime/keeperfx-x64'
        self.manifest = 'a' * 64
        self.base_root = self.integration / 'combined' / self.manifest
        self.build_root = self.integration / 'builds/base-v1'
        self.groups = {'HD': ['fxdata/imp.zip', 'fxdata/unchanged.cfg'], 'Original': ['fxdata/control.zip'], 'native-frontend': ['data/loading64.raw'], 'build': ['keeperfx-dx12.exe', 'keeperfx-local.exe', 'shaders/world.dxil']}
        self.entries = {}
        self.palette = self.runtime / 'ldata/front.pal'
        put(self.palette, b'original-palette')
        palette = {'path': str(self.palette), 'expected_sha256': M.digest(self.palette), 'current_sha256': M.digest(self.palette), 'replace_palette': False}
        for variant, names in self.groups.items():
            root = self.build_root if variant == 'build' else self.base_root / variant
            entries = []
            for name in names:
                item = put(root / name, (variant + ':' + name).encode())
                item['file'] = name
                if variant == 'build':
                    put(self.runtime / name, (variant + ':' + name).encode())
                    item['base_sha256'] = M.digest(self.runtime / name)
                if variant == 'native-frontend':
                    put(self.runtime / name, b'native-original')
                    item.update(original_sha256=M.digest(self.runtime / name), palette=palette)
                entries.append(item)
            self.entries[variant] = entries
            if variant in ('HD', 'Original'):
                save(root / 'integration-owner.json', {'owner': 'remaster-integration', 'manifest_sha256': self.manifest, 'variant': variant, 'files': entries})
        build = {x['file']: x for x in self.entries['build']}
        self.base = {'schema': 1, 'local_only': True, 'kind': 'synthetic', 'manifest_sha256': self.manifest, 'root': str(self.base_root / 'HD'), 'mod_name': 'remaster_core_' + self.manifest[:12], 'files': self.entries['HD'], 'original_control_spec': self.entries['Original'], 'native_overlay_spec': self.entries['native-frontend'], 'original_files': [dict(e, source=str(self.base_root / 'Original' / e['file'])) for e in self.entries['Original']], 'native_overlays': [dict(e, source=str(self.base_root / 'native-frontend' / e['file'])) for e in self.entries['native-frontend']], 'review_scope': {'normal_api': False, 'build_artifact_dir': str(self.build_root), 'build_files': self.entries['build'], 'required_dx12_executable_sha256': build['keeperfx-dx12.exe']['sha256'], 'default_executable_sha256': build['keeperfx-local.exe']['sha256'], 'shader_files': [{'file': 'world.dxil', 'sha256': build['shaders/world.dxil']['sha256'], 'bytes': build['shaders/world.dxil']['bytes']}]}}
        self.base.update(coverage={'frontend_native_resources': 1}, excluded_pending=['stale pending'], preserve={'active_Imp_candidate_sha256': 'old'}, )
        self.base['review_scope']['runtime'] = 'old art package unchanged'
        self.base_path = self.integration / 'combined-full-review-v1.json'
        save(self.base_path, self.base)
        self.defaults = [self.integration / 'combined-full-review.json', self.integration / 'combined-full-runtime-selection.json']
        for default in self.defaults:
            put(default, b'DEFAULT-DO-NOT-CHANGE')
        for name in ('imp.zip', 'cta.zip', 'loading64.raw', 'rgmap00.raw'):
            put(self.w / 'inputs' / name, ('replacement:' + name).encode())
        put(self.runtime / 'campgns/keeporig_lnd/rgmap00.raw', b'original-landview')
        evidence = self.w / 'inputs/review.json'
        save(evidence, {'passed': True, 'user_appearance_accepted': False})
        self.plan = {'schema': 1, 'version': 'v2', 'base_report': {'source': self.base_path.relative_to(self.w).as_posix(), 'sha256': M.digest(self.base_path), 'manifest_sha256': self.manifest}, 'changes': [self.change('HD', 'fxdata/imp.zip', 'imp.zip', self.entries['HD'][0]['sha256']), self.change('HD', 'fxdata/cta.zip', 'cta.zip', None), self.change('native-frontend', 'data/loading64.raw', 'loading64.raw', self.entries['native-frontend'][0]['sha256']), dict(self.change('native-frontend', 'campgns/keeporig_lnd/rgmap00.raw', 'rgmap00.raw', None), native_control={'original_sha256': M.digest(self.runtime / 'campgns/keeporig_lnd/rgmap00.raw'), 'palette': palette})], 'evidence': [{'source': 'inputs/review.json', 'sha256': M.digest(evidence)}], 'review': {'summary': 'Synthetic review only', 'included': ['declared assets'], 'pending': ['whole bundle runtime', 'user appearance'], 'user_appearance_accepted': False}}
        self.plan_path = self.w / 'plan.json'
        save(self.plan_path, self.plan)

    def change(self, variant, name, source, old):
        return {'variant': variant, 'file': name, 'source': 'inputs/' + source, 'sha256': M.digest(self.w / 'inputs' / source), 'before_sha256': old}

    def verify(self):
        save(self.plan_path, self.plan)
        return M.verify_plan(self.w, self.plan_path)

    def snapshot(self):
        return {p.relative_to(self.w).as_posix(): M.digest(p) for p in self.w.rglob('*') if p.is_file()}

    def test_verify_is_read_only_and_stable(self):
        before = self.snapshot()
        state = M.verify_plan(self.w, self.plan_path)
        self.assertEqual(before, self.snapshot())
        self.assertEqual(state['report']['manifest_sha256'], M.manifest_digest(state['report']))
        self.assertEqual(state['report']['manifest_sha256'], M.verify_plan(self.w, self.plan_path)['report']['manifest_sha256'])
        self.assertTrue(all(not p.exists() for p in state['outputs']))

    def test_package_exact_subset_and_owners(self):
        before = self.snapshot()
        state = self.verify()
        proof = M.build_package(state)
        after = self.snapshot()
        for name, digest in before.items():
            self.assertEqual(digest, after[name], name)
        self.assertEqual(proof['counts'], {'HD': 3, 'Original': 1, 'native-frontend': 2, 'build': 3})
        self.assertFalse(proof['runtime_changed'])
        self.assertEqual(M.read_json(self.integration / 'combined-full-review-v2.json'), state['report'])
        for variant in M.GROUPS:
            root = state['build_dest'] if variant == 'build' else state['dest'] / variant
            M.checked_tree(self.w, root, list(state['maps'][variant].values()), state['report']['manifest_sha256'], variant, True)
        self.assertEqual(state['report']['excluded_pending'], self.plan['review']['pending'])
        self.assertEqual(state['report']['coverage']['frontend_native_resources'], 2)
        self.assertEqual(state['report']['historical_base_descriptors']['preserve'], self.base['preserve'])
        self.assertNotIn('active_Imp_candidate_sha256', state['report']['preserve'])
        self.assertNotIn('old art package unchanged', state['report']['review_scope']['runtime'])
        self.assertEqual(state['maps']['Original'], state['base_maps']['Original'])
        self.assertEqual(state['maps']['build'], state['base_maps']['build'])
        for name, item in state['maps']['HD'].items():
            self.assertEqual(M.digest(state['dest'] / 'HD' / name), item['sha256'])
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.verify()

    def test_existing_output_refused(self):
        put(self.integration / 'combined-full-review-v2.json', b'existing')
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.verify()

    def test_protected_variants_refused(self):
        for variant in ('Original', 'build'):
            self.plan['changes'][0]['variant'] = variant
            with self.subTest(variant=variant), self.assertRaisesRegex(ValueError, 'forbidden'):
                self.verify()

    def test_hash_drift_refused(self):
        for path in (self.base_path, self.w / 'inputs/imp.zip', self.w / 'inputs/review.json', self.base_root / 'HD/fxdata/imp.zip', self.base_root / 'Original/fxdata/control.zip', self.build_root / 'shaders/world.dxil', self.base_root / 'native-frontend/data/loading64.raw'):
            old = path.read_bytes()
            path.write_bytes(b'changed')
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'hash mismatch'):
                self.verify()
            path.write_bytes(old)

    def test_runtime_control_drift_refused(self):
        for path in (self.runtime / 'keeperfx-dx12.exe', self.runtime / 'data/loading64.raw', self.runtime / 'campgns/keeporig_lnd/rgmap00.raw', self.palette):
            old = path.read_bytes()
            path.write_bytes(b'changed')
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, '(baseline differs|palette changed)'):
                self.verify()
            path.write_bytes(old)

    def test_unlisted_member_and_wrong_owner_refused(self):
        owner_path = self.base_root / 'HD/integration-owner.json'
        owner = M.read_json(owner_path)
        owner['variant'] = 'Original'
        save(owner_path, owner)
        with self.assertRaisesRegex(ValueError, 'Owner/version/variant'):
            self.verify()
        owner['variant'] = 'HD'
        save(owner_path, owner)
        put(self.base_root / 'HD/unlisted.bin', b'extra')
        with self.assertRaisesRegex(ValueError, 'members differ'):
            self.verify()

    def test_unsafe_paths_refused(self):
        for name in ('.', '../escape', '/absolute', 'fxdata/x:ads', 'fxdata/CON.txt', 'fxdata/x.', 'fxdata//x', 'fxdata\\x', 'fxdata/./x'):
            self.plan['changes'][0]['file'] = name
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.verify()

    def test_case_collision_and_duplicate_refused(self):
        self.plan['changes'].append(copy.deepcopy(self.plan['changes'][0]))
        with self.assertRaisesRegex(ValueError, 'Duplicate replacement'):
            self.verify()
        self.plan['changes'].pop()
        self.plan['changes'][0]['file'] = 'FXDATA/imp.zip'
        with self.assertRaisesRegex(ValueError, 'Case-colliding'):
            self.verify()

    def test_wrong_before_hash_refused(self):
        self.plan['changes'][0]['before_sha256'] = 'f' * 64
        with self.assertRaisesRegex(ValueError, 'before hash mismatch'):
            self.verify()

    def test_native_addition_requires_exact_contract(self):
        entry = self.plan['changes'][-1]
        control = entry.pop('native_control')
        with self.assertRaisesRegex(ValueError, 'requires native_control'):
            self.verify()
        entry['native_control'] = control
        control['palette']['replace_palette'] = True
        with self.assertRaisesRegex(ValueError, 'Palette replacement'):
            self.verify()

    def test_existing_native_contract_immutable(self):
        self.plan['changes'][2]['native_control'] = {'original_sha256': 'f' * 64, 'palette': {}}
        with self.assertRaisesRegex(ValueError, 'must remain exact'):
            self.verify()

    def test_native_build_disguise_refused(self):
        self.plan['changes'][-1]['file'] = 'keeperfx-dx12.exe'
        with self.assertRaisesRegex(ValueError, 'Unsupported native frontend'):
            self.verify()

    def test_copy_failure_keeps_identifiable_output_without_activation(self):
        state = self.verify()
        put(self.w / 'inputs/imp.zip', b'changed-after-preflight')
        with self.assertRaisesRegex(ValueError, 'Source changed after preflight'):
            M.build_package(state)
        self.assertTrue(state['dest'].exists())
        journal = [json.loads(line) for line in (state['dest'] / 'package-review-state.jsonl').read_text().splitlines()]
        self.assertEqual(journal[-1]['status'], 'failed_preserved_for_inspection')
        self.assertFalse((self.integration / 'combined-full-review-v2.json').exists())
        for default in self.defaults:
            self.assertEqual(default.read_bytes(), b'DEFAULT-DO-NOT-CHANGE')

    def make_junction(self, source, target):
        import _winapi
        # Only synthetic fixture directories may be relocated; keep every byte.
        self.assertTrue(source.resolve().is_relative_to(self.w.resolve()))
        self.assertTrue(target.resolve().is_relative_to(self.w.resolve()))
        target.parent.mkdir(parents=True, exist_ok=True)
        source.rename(target)
        _winapi.CreateJunction(str(target), str(source))

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_known_runtime_junctions_read_only(self):
        for name in ('campgns', 'data', 'ldata'):
            self.make_junction(self.runtime / name, self.w / 'runtime/keeperfx' / name)
        self.verify()
        copied_source = self.runtime / 'campgns/keeporig_lnd/rgmap00.raw'
        with self.assertRaisesRegex(ValueError, 'Linked/reparse'):
            M.pinned(self.w, {'source': copied_source.relative_to(self.w).as_posix(), 'sha256': M.digest(copied_source)})

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_wrong_runtime_junction_target_refused(self):
        self.make_junction(self.runtime / 'campgns', self.w / 'runtime/wrong/campgns')
        with self.assertRaisesRegex(ValueError, 'unexpected target'):
            self.verify()

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_nested_runtime_junction_refused(self):
        self.make_junction(self.runtime / 'campgns', self.w / 'runtime/keeperfx/campgns')
        self.make_junction(self.w / 'runtime/keeperfx/campgns/keeporig_lnd', self.w / 'runtime/keeperfx/moved_landview')
        with self.assertRaisesRegex(ValueError, 'Linked/reparse'):
            self.verify()

    def test_duplicate_json_keys_refused(self):
        self.plan_path.write_text('{"schema":1,"schema":1}')
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON key'):
            M.verify_plan(self.w, self.plan_path)


if __name__ == '__main__':
    unittest.main(verbosity=2)
