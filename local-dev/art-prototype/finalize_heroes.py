# SPDX-License-Identifier: GPL-2.0-or-later
"""Bind completed local hero validation to the exact packaged files."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'artifacts/art-prototype/heroes'


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def receipt(path):
    return {'path': path.relative_to(ROOT).as_posix(), 'sha256': sha(path)}


def main():
    registry_path = Path(__file__).with_name('prototype.json')
    registry = read(registry_path)
    acceptance = registry.get('heroes', {})
    acceptance_record = {key: acceptance.get(key) for key in
                         ('userVisualAcceptance', 'userVisualAcceptanceDate', 'userVisualAcceptanceQuote',
                          'userVisualAcceptanceManifestSHA256')}
    package = read(OUT / 'combined-package-report.json')
    accepted = (acceptance.get('userVisualAcceptance') == 'roster_visual_review_accepted'
                and acceptance.get('userVisualAcceptanceManifestSHA256') == package['package_manifest_sha256'])
    acceptance_status = 'roster_visual_review_accepted' if accepted else 'pending'
    runtime = read(OUT / 'combined-runtime-report.json')
    assert runtime['packageManifestSHA256'] == package['package_manifest_sha256']
    assert runtime['executableSHA256Before'] == runtime['executableSHA256After']
    assert sha(ROOT / 'runtime/keeperfx-x64/keeperfx-dx12.exe') == runtime['executableSHA256After']
    assert runtime['comparison']['passed'] and runtime['comparison']['matchedInitialScene']
    assert runtime['comparison']['creatures'] == 168
    assert sha(Path(runtime['comparison']['path'])) == runtime['comparison']['sha256']
    combined = OUT / 'mod/heroes_art_prototype'
    for entry in package['packages']:
        assert sha(combined / 'fxdata' / entry['zip']) == entry['zip_sha256']
        assert sha(combined / 'creatrs' / entry['config']) == entry['config_sha256']
    for run in runtime['evidence'].values():
        assert run['passed'] and run['settingsRestored'] and run['apiOwnerVerified']
        assert run['dx12']['rejected'] == 0
        assert sha(Path(run['path']) / 'run-report.json') == run['runReportSHA256']
        assert sha(Path(run['path']) / 'fixture-report.json') == run['fixtureReportSHA256']

    individual = []
    for row in read(OUT / 'roster-status.json'):
        folder = OUT / row['slug']
        assert row['packed'] and row['test'], row['slug']
        report = read(folder / 'package-report.json')
        run_path = folder / row['test']
        run = read(run_path / 'run-report.json')
        fixture = read(run_path / 'fixture-report.json')
        assert run['passed'] and run['settingsRestored'] and run['apiOwnerVerified']
        assert run['packageSHA256'].lower() == report['zip_sha256']
        assert run['dx12']['rejected'] == 0 and fixture['passed']
        individual.append({'hero': row['slug'], 'zip_sha256': report['zip_sha256'],
                           'overhead_frames': report['overhead_frames'], 'held_frames': 6,
                           'run': receipt(run_path / 'run-report.json'),
                           'fixture': receipt(run_path / 'fixture-report.json'),
                           'review_data': receipt(folder / 'review-data.json')})
    assert len(individual) == 13
    visual_paths = [OUT / 'fairy/visual-audit/findings.json',
                    OUT / 'final-visual-audit/findings.json',
                    OUT / 'final-visual-audit/corrections-closed.json']
    browser = OUT / 'browser-review-report.json'
    assert read(browser)['controls_passed']
    launcher_check = OUT / 'launcher-validation.json'
    assert read(launcher_check)['passed']
    package['status'] = acceptance_status if accepted else 'validated_local_preview_user_visual_acceptance_pending'
    package['user_visual_acceptance'] = acceptance_record
    package['integration_report'] = 'integration-report.json'
    (OUT / 'combined-package-report.json').write_text(json.dumps(package, indent=2) + '\n', encoding='utf-8')
    report = {
        'schema': 1, 'date': '2026-10-15',
        'status': acceptance_status if accepted else 'local_hero_batch_complete_user_visual_acceptance_pending',
        'local_only': True, 'user_visual_acceptance': acceptance_record,
        'scope': 'All 13 original heroes: unique overhead actions and six FP PowerGrab frames each at 4x texture dimensions. Original world size, anchors, counts and timing retained. Other possession views and shared effects remain original.',
        'hero_overhead_frames': 1491, 'hero_held_frames': 78,
        'combined_overhead_frames': 1688, 'combined_held_frames': 84,
        'combined_animation_entries': 158,
        'package_manifest_sha256': package['package_manifest_sha256'],
        'combined_package': receipt(OUT / 'combined-package-report.json'),
        'individual_validation': individual,
        'static_visual_audits': [receipt(p) for p in visual_paths],
        'browser_review': receipt(browser),
        'combined_runtime': receipt(OUT / 'combined-runtime-report.json'),
        'current_executable_sha256': runtime['executableSHA256After'],
        'executable_scope': 'Combined diagnostic and paired benchmarks bind the current executable. Historical individual runs bind their exact packages; do not retrospectively assign the current executable hash to those older runs.',
        'review_url': 'http://127.0.0.1:57580/heroes/review.html',
        'launcher': 'local-dev/art-prototype/play_heroes.ps1',
        'launcher_validation': receipt(launcher_check),
        'launcher_source': receipt(ROOT / 'keeperfx/local-dev/art-prototype/play_heroes.ps1'),
        'workflow': 'keeperfx/local-dev/art-prototype/WORKFLOW.md',
        'performance': runtime['comparison'],
        'limits': ['Ordinary campaign play remains pending; visual acceptance is recorded separately.',
                   'The benchmark measures one matched 168-creature scene at 1920x1080, not scanout FPS or every campaign.',
                   'Both benchmark variants retain HD archives; cold loading and total process/VRAM savings are not measured.',
                   'Broad OpenGL crowd performance and natural combat/hauling are outside the fixture claims.',
                   'Commercial-derived artwork, packages and captures must remain local.'],
        'next_step': 'Play the combined local preview during ordinary campaign play. Preserve the recorded visual decision and apply only requested corrections.'
    }
    (OUT / 'integration-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    registry['status'] = acceptance_status if accepted else 'imp_complete_original_heroes_ready_for_user_review'
    registry['heroes'].update(status=acceptance_status if accepted else 'ready_for_user_review', packedHeroes=13, passedGameFixtures=13,
                              pauseCheckpoint='local-dev/art-prototype/HEROES-CHECKPOINT.md',
                              heroOverheadFrames=1491, heroHeldFrames=78,
                              combinedValidation='passed',
                              combinedManifestSHA256=package['package_manifest_sha256'],
                              integrationReport='artifacts/art-prototype/heroes/integration-report.json',
                              interactiveReview='artifacts/art-prototype/heroes/review.html',
                              playtestLauncher='local-dev/art-prototype/play_heroes.ps1',
                              currentPackageVisualAcceptance=acceptance_status, ordinaryCampaignPlay='pending',
                              resume='Original hero batch packaged and validated. Preserve recorded visual acceptance; ordinary campaign play remains pending. Repair only requested defects. Other creatures require a new user instruction.')
    registry_path.write_text(json.dumps(registry, indent=2) + '\n', encoding='utf-8')
    print('13 heroes complete with matching package, visual, runtime and performance evidence; visual review: ' + acceptance_status + '; ordinary campaign play pending.')


if __name__ == '__main__':
    main()
