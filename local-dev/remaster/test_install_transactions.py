# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic installer transaction regressions. Never uses real game data.

Run: python -B test_install_transactions.py --installer /path/to/install.py
All installer outputs are confined to an automatically created temporary tree.
assemble/assert_closed are replaced; external process creation is forbidden.
Only an explicitly requested --report file may be written outside that tree.
"""
from pathlib import Path
import argparse
import contextlib
import hashlib
import json
import os
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
INSTALLER_SOURCE = b''
INSTALLER_PATH = None


class InjectedFailure(OSError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_installer():
    # Compile the captured bytes, avoiding a read/import race and __pycache__.
    module = types.ModuleType('synthetic_remaster_install')
    module.__file__ = str(INSTALLER_PATH)
    sys.modules[module.__name__] = module
    exec(compile(INSTALLER_SOURCE, str(INSTALLER_PATH), 'exec'), module.__dict__)
    return module


class InstallerTransactions(unittest.TestCase):
    def setUp(self):
        self.module = load_installer()
        self.folder = tempfile.TemporaryDirectory(prefix='keeperfx-synthetic-transaction-')
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name).resolve()
        self.game, self.stage = self.root / 'game', self.root / 'stage'
        for part in ('data', 'fxdata', 'mods', 'ldata', 'remaster'):
            (self.game / part).mkdir(parents=True)
        self.stage.mkdir()
        self.mod = self.module.MOD
        self.selection = self.module.SELECTION
        self.payloads = {
            'fxdata/standard-hd.kfht': b'NEW-TERRAIN-SYNTHETIC',
            'mods/' + self.mod + '/fxdata/example-sprites.zip': b'NEW-SPRITES-SYNTHETIC',
            'remaster/packages/' + self.selection + '/ui/gui1-64.kfui': b'NEW-UI-SYNTHETIC',
        }
        self.new_path = list(self.payloads)[2]
        self.before = {
            list(self.payloads)[0]: b'OLD-TERRAIN-SYNTHETIC',
            list(self.payloads)[1]: b'OLD-SPRITES-SYNTHETIC',
            self.new_path: None,
            'keeperfx.cfg': b'UNCHANGED-USER-SETTINGS',
            'mods/unrelated/user.cfg': b'UNCHANGED-UNRELATED-MOD',
        }
        for name, data in self.before.items():
            if data is not None:
                self.write(self.game / name, data)
        previous = self.profile({name: data for name, data in self.before.items()
                                 if name in self.payloads and data is not None})
        self.write(self.game / 'remaster/default.json', self.json_bytes(previous))
        self.before['remaster/default.json'] = (self.game / 'remaster/default.json').read_bytes()
        self.custom_changes = None
        self.profile_first = False
        self.close_check = mock.Mock(name='assert_closed')
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(self.module, 'assemble', self.assemble))
        self.stack.enter_context(mock.patch.object(self.module, 'assert_closed', self.close_check))
        # Catch future changes that accidentally escape the synthetic boundary.
        self.stack.enter_context(mock.patch.object(self.module.subprocess, 'run',
            side_effect=AssertionError('External subprocess forbidden in transaction tests')))
        self.stack.enter_context(mock.patch.object(self.module.subprocess, 'Popen',
            side_effect=AssertionError('External process forbidden in transaction tests')))

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    @staticmethod
    def json_bytes(value):
        return (json.dumps(value, indent=2) + '\n').encode('utf8')

    def profile(self, payloads):
        return {'schema': 1, 'id': 'keeperfx-remaster-v13', 'selection': self.selection,
                'mod': self.mod, 'files': [
                    {'path': name, 'sha256': digest(data), 'bytes': len(data)}
                    for name, data in sorted(payloads.items())]}

    def assemble(self, game, stage, powershell):
        self.assertEqual(Path(game).resolve(), self.game)
        self.assertEqual(Path(stage).resolve(), self.stage)
        payloads = self.payloads if self.custom_changes is None else self.custom_changes
        profile = self.profile(payloads)
        changes = {}
        # Source names are independent of requested destination paths. Even the
        # deliberately unsafe destination cases never stage outside this tree.
        for i, (name, data) in enumerate(payloads.items()):
            source = self.stage / ('synthetic-' + str(i) + '.dat')
            self.write(source, data)
            changes[name] = source
        source = self.stage / 'synthetic-profile.json'
        self.write(source, self.json_bytes(profile))
        if self.profile_first:
            changes = {'remaster/default.json': source, **changes}
        else:
            changes['remaster/default.json'] = source
        return profile, changes

    def install(self, verify=False):
        return self.module.install(self.game, self.stage, 'forbidden-powershell.exe',
                                   verify_only=verify)

    def assert_before_restored(self):
        for name, expected in self.before.items():
            path = self.game / name
            if expected is None:
                self.assertFalse(path.exists(), name)
            else:
                self.assertEqual(path.read_bytes(), expected, name)

    def assert_no_temps(self, allowed=()):
        expected = {Path(p).resolve() for p in allowed}
        actual = {p.resolve() for p in self.game.rglob('*.tmp')}
        self.assertEqual(actual, expected, 'Only explicitly preexisting temporary files may remain')

    def journals(self):
        return [(p, json.loads(p.read_text(encoding='utf-8-sig')))
                for p in (self.game / 'remaster/backups').glob('*/journal.json')]

    def assert_rolled_back(self):
        self.assert_before_restored()
        journals = self.journals()
        self.assertTrue(journals, 'Failure must retain a readable transaction journal')
        self.assertEqual(journals[-1][1]['state'], 'rolled_back')
        self.assert_no_temps()

    def snapshot_game(self):
        result = {}
        for here, dirs, files in os.walk(self.game, followlinks=False):
            for name in dirs:
                path = Path(here) / name
                result[path.relative_to(self.game).as_posix() + '/'] = None
            for name in files:
                path = Path(here) / name
                result[path.relative_to(self.game).as_posix()] = path.read_bytes()
        return result

    def is_stage_source(self, stream):
        try:
            return Path(stream.name).resolve().is_relative_to(self.stage)
        except (TypeError, OSError, ValueError):
            return False

    def fail_second_stage_copy(self, source, destination, *args, **kwargs):
        if self.is_stage_source(source):
            self.copy_count += 1
            if self.copy_count == 2:
                destination.write(source.read(5))
                raise InjectedFailure('Synthetic partial copy failure')
        return self.real_copy(source, destination, *args, **kwargs)

    def test_old_selection_upgrades_with_recoverable_profile(self):
        profile_path = self.game / 'remaster/default.json'
        previous = json.loads(profile_path.read_text())
        previous['selection'] = 'dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf'
        old_bytes = self.json_bytes(previous)
        profile_path.write_bytes(old_bytes)
        result = self.install()
        self.assertTrue(result['installed'])
        self.assertEqual(json.loads(profile_path.read_text())['selection'], self.selection)
        journal_path, journal = self.journals()[-1]
        entry = next(e for e in journal['entries'] if e['path'] == 'remaster/default.json')
        self.assertEqual((journal_path.parent / entry['backup']).read_bytes(), old_bytes)

    def test_assembly_refuses_mixed_sprite_selection(self):
        probe = load_installer()
        importer = types.SimpleNamespace(build=mock.Mock(return_value={
            'authority_manifest_sha256': '0' * 64}))
        with mock.patch.object(probe, 'module', return_value=importer):
            with self.assertRaisesRegex(ValueError, 'Sprite selection differs'):
                probe.assemble(self.game, self.stage, 'forbidden-powershell.exe')
        self.assertEqual(importer.build.call_count, 1)

    def test_success_and_idempotence_preserve_existing_payloads(self):
        first = self.install()
        self.assertTrue(first['installed'])
        for name, data in self.payloads.items():
            self.assertEqual((self.game / name).read_bytes(), data)
        stamps = {name: (self.game / name).stat().st_mtime_ns for name in self.payloads}
        second = self.install()
        self.assertTrue(second['installed'])
        self.assertEqual(second['changed'], 0)
        self.assertEqual(stamps, {name: (self.game / name).stat().st_mtime_ns
                                  for name in self.payloads})
        self.assertEqual((self.game / 'keeperfx.cfg').read_bytes(), self.before['keeperfx.cfg'])
        self.assertEqual((self.game / 'mods/unrelated/user.cfg').read_bytes(),
                         self.before['mods/unrelated/user.cfg'])
        self.assert_no_temps()
        self.assertGreaterEqual(self.close_check.call_count, 1)

    def test_verify_only_success_is_read_only(self):
        self.install()
        before = self.snapshot_game()
        result = self.install(verify=True)
        self.assertTrue(result['verified'])
        self.assertEqual(before, self.snapshot_game())

    def test_verify_only_mismatch_never_repairs(self):
        self.install()
        first = self.game / next(iter(self.payloads))
        first.write_bytes(b'USER-CHANGED-AFTER-INSTALL')
        before = self.snapshot_game()
        with self.assertRaises(Exception):
            self.install(verify=True)
        self.assertEqual(before, self.snapshot_game())

    def test_verify_only_missing_control_directory_creates_nothing(self):
        control = self.game / 'remaster'
        control.rename(self.root / 'saved-control')
        self.profile_first = True
        before = self.snapshot_game()
        with self.assertRaises(Exception):
            self.install(verify=True)
        self.assertFalse(control.exists(), 'VerifyOnly must not create its control directory')
        self.assertEqual(before, self.snapshot_game())

    def test_partial_copy_failure_rolls_back_and_cleans_temporary(self):
        self.real_copy = self.module.shutil.copyfileobj
        self.copy_count = 0
        with mock.patch.object(self.module.shutil, 'copyfileobj', self.fail_second_stage_copy):
            with self.assertRaises(Exception):
                self.install()
        self.assertEqual(self.copy_count, 2)
        self.assert_rolled_back()

    def test_backup_copy_failure_never_changes_runtime_payload(self):
        real_copy = self.module.shutil.copyfile
        def fail_backup(source, destination, *args, **kwargs):
            if 'backups' in Path(destination).parts:
                Path(destination).write_bytes(b'PARTIAL-BACKUP')
                raise InjectedFailure('Synthetic backup copy failure')
            return real_copy(source, destination, *args, **kwargs)
        with mock.patch.object(self.module.shutil, 'copyfile', fail_backup):
            with self.assertRaises(Exception):
                self.install()
        self.assert_before_restored()
        self.assert_no_temps()

    def test_replace_failure_after_partial_commit_rolls_back(self):
        real_replace = self.module.os.replace
        destinations = {self.game / name for name in self.payloads}
        calls = []
        def fail_second(source, destination):
            if Path(destination) in destinations:
                calls.append(Path(destination))
                if len(calls) == 2:
                    raise InjectedFailure('Synthetic replacement failure')
            return real_replace(source, destination)
        with mock.patch.object(self.module.os, 'replace', fail_second):
            with self.assertRaises(Exception):
                self.install()
        self.assertGreaterEqual(len(calls), 2)
        self.assert_rolled_back()

    def test_exception_after_new_file_replace_still_rolls_back(self):
        real_replace = self.module.os.replace
        new_target = self.game / self.new_path
        injected = []
        def fail_after_replace(source, destination):
            result = real_replace(source, destination)
            if Path(destination) == new_target and not injected:
                injected.append(True)
                raise InjectedFailure('Replacement completed before synthetic exception')
            return result
        with mock.patch.object(self.module.os, 'replace', fail_after_replace):
            with self.assertRaises(Exception):
                self.install()
        self.assertTrue(injected)
        self.assert_rolled_back()

    def test_final_journal_failure_does_not_skip_temporary_cleanup(self):
        if not hasattr(self.module, 'dump'):
            self.skipTest('Installer dump injection helper changed; adapt this focused case')
        real_dump = self.module.dump
        self.real_copy = self.module.shutil.copyfileobj
        self.copy_count = 0
        journal_fault = []
        def fail_final_dump(path, value):
            if isinstance(value, dict) and value.get('state') == 'rolled_back':
                journal_fault.append(True)
                raise InjectedFailure('Synthetic final journal write failure')
            return real_dump(path, value)
        with mock.patch.object(self.module, 'dump', fail_final_dump), \
             mock.patch.object(self.module.shutil, 'copyfileobj', self.fail_second_stage_copy):
            with self.assertRaises(Exception):
                self.install()
        self.assertTrue(journal_fault, 'Focused final journal fault was not reached')
        self.assert_before_restored()
        self.assert_no_temps()
        self.assertTrue(self.journals(), 'Earlier valid journal must remain readable')

    def test_restore_replace_failure_retains_usable_backup(self):
        real_replace = self.module.os.replace
        original_target = self.game / next(iter(self.payloads))
        self.real_copy = self.module.shutil.copyfileobj
        self.copy_count = 0
        restore_fault = []
        def fail_restore(source, destination):
            if Path(destination) == original_target and Path(source).read_bytes() == self.before[next(iter(self.payloads))]:
                restore_fault.append(True)
                raise InjectedFailure('Synthetic rollback replacement failure')
            return real_replace(source, destination)
        with mock.patch.object(self.module.os, 'replace', fail_restore), \
             mock.patch.object(self.module.shutil, 'copyfileobj', self.fail_second_stage_copy):
            with self.assertRaises(Exception):
                self.install()
        self.assertTrue(restore_fault)
        journals = self.journals()
        self.assertTrue(journals)
        path, journal = journals[-1]
        self.assertNotIn(journal['state'], ('installed', 'rolled_back'))
        entry = next(e for e in journal['entries'] if e['path'] == next(iter(self.payloads)))
        self.assertEqual((path.parent / entry['backup']).read_bytes(), self.before[entry['path']])
        self.assertEqual((self.game / 'keeperfx.cfg').read_bytes(), self.before['keeperfx.cfg'])
        self.assert_no_temps()

    def test_preexisting_same_stamp_temporary_is_never_deleted(self):
        stamp = 'a' * 32
        first = self.game / next(iter(self.payloads))
        unowned = first.with_name(first.name + '.' + stamp + '.tmp')
        unowned.write_bytes(b'PREEXISTING-UNOWNED-TEMPORARY')
        with mock.patch.object(self.module.uuid, 'uuid4', return_value=types.SimpleNamespace(hex=stamp)):
            try:
                self.install()
            except Exception:
                pass  # Refusing a name collision is safe; deleting it is not.
        self.assertEqual(unowned.read_bytes(), b'PREEXISTING-UNOWNED-TEMPORARY')
        self.assert_no_temps(allowed=[unowned])

    def test_existing_owned_mod_with_unrecognized_file_is_preserved(self):
        unowned = self.game / 'mods' / self.mod / 'unrecognized-user-file.txt'
        self.write(unowned, b'USER-OWNED')
        with self.assertRaises(Exception):
            self.install()
        self.assert_before_restored()
        self.assertEqual(unowned.read_bytes(), b'USER-OWNED')
        self.assert_no_temps()

    def test_unsafe_destination_paths_never_escape_synthetic_game(self):
        outside = self.root / 'outside'
        outside.mkdir()
        sentinel = outside / 'sentinel.bin'
        sentinel.write_bytes(b'OUTSIDE-SENTINEL')
        bad_paths = ['../outside/escape.bin', 'mods/../../outside/escape.bin',
                     str(outside / 'absolute.bin'), 'mods\\..\\..\\outside\\escape.bin']
        for name in bad_paths:
            with self.subTest(name=name):
                self.custom_changes = {name: b'SHOULD-NOT-BE-INSTALLED'}
                with self.assertRaises(Exception):
                    self.install()
                self.assertEqual(sorted(p.name for p in outside.iterdir()), ['sentinel.bin'])
                self.assertEqual(sentinel.read_bytes(), b'OUTSIDE-SENTINEL')
                self.assert_before_restored()

    def reparse_guard(self, linked, changes):
        linked.mkdir(parents=True, exist_ok=True)
        real_lstat = Path.lstat
        class LinkedStat:
            def __init__(self, original): self.original = original
            @property
            def st_file_attributes(self):
                return getattr(self.original, 'st_file_attributes', 0) | 0x400
            def __getattr__(self, name): return getattr(self.original, name)
        def injected_lstat(path, *args, **kwargs):
            result = real_lstat(path, *args, **kwargs)
            return LinkedStat(result) if path == linked else result
        self.custom_changes = changes
        with mock.patch.object(Path, 'lstat', injected_lstat):
            with self.assertRaises(Exception):
                self.install()
        self.assert_before_restored()
        self.assert_no_temps()

    def test_control_directory_reparse_flag_is_rejected(self):
        self.reparse_guard(self.game / 'remaster', self.payloads)

    def test_descendant_reparse_flag_is_rejected(self):
        self.reparse_guard(self.game / 'fxdata/redirect',
                           {'fxdata/redirect/escape.kfht': b'NO-ESCAPE'})

    def real_symlink(self, link, destination):
        try:
            link.symlink_to(destination, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest('Host cannot create symbolic links: ' + str(error))

    def test_dangling_control_symlink_is_rejected_without_creating_target(self):
        (self.game / 'remaster').rename(self.root / 'saved-control')
        destination = self.root / 'missing-control-target'
        self.real_symlink(self.game / 'remaster', destination)
        self.profile_first = True
        with self.assertRaises(Exception):
            self.install()
        self.assertFalse(destination.exists(), 'Dangling control link must not create its target')
        self.assertTrue((self.game / 'remaster').is_symlink())

    def test_descendant_symlink_cannot_write_outside_selected_top(self):
        outside = self.root / 'linked-outside'
        outside.mkdir()
        sentinel = outside / 'sentinel.bin'
        sentinel.write_bytes(b'OUTSIDE-SENTINEL')
        self.real_symlink(self.game / 'fxdata/redirect', outside)
        self.custom_changes = {'fxdata/redirect/escape.kfht': b'NO-ESCAPE'}
        with self.assertRaises(Exception):
            self.install()
        self.assertEqual([p.name for p in outside.iterdir()], ['sentinel.bin'])
        self.assertEqual(sentinel.read_bytes(), b'OUTSIDE-SENTINEL')
        self.assert_before_restored()

    def test_dangling_descendant_symlink_does_not_create_target(self):
        destination = self.root / 'missing-descendant-target'
        self.real_symlink(self.game / 'fxdata/redirect', destination)
        self.custom_changes = {'fxdata/redirect/escape.kfht': b'NO-ESCAPE'}
        with self.assertRaises(Exception):
            self.install()
        self.assertFalse(destination.exists())
        self.assert_before_restored()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer', type=Path, default=Path(__file__).with_name('install.py'))
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    global INSTALLER_SOURCE, INSTALLER_PATH
    INSTALLER_PATH = args.installer.resolve(strict=True)
    INSTALLER_SOURCE = INSTALLER_PATH.read_bytes()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InstallerTransactions)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if args.report:
        value = {'installer': str(INSTALLER_PATH), 'installer_sha256': digest(INSTALLER_SOURCE),
                 'test_sha256': digest(Path(__file__).read_bytes()), 'tests_run': result.testsRun,
                 'passed': result.wasSuccessful(), 'failures': len(result.failures),
                 'errors': len(result.errors), 'skipped': [(str(t), reason) for t, reason in result.skipped],
                 'failure_details': [(str(t), message) for t, message in result.failures + result.errors],
                 'scope': 'Synthetic temporary filesystem only; assemble/assert_closed mocked; subprocesses forbidden. No real runtime/assets/builds.'}
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(value, indent=2) + '\n', encoding='utf8')
    return 0 if result.wasSuccessful() else 1

if __name__ == '__main__':
    raise SystemExit(main())
