"""Binding, drift, preview, apply, idempotency and release-unit isolation tests."""
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('local_version_manager', ROOT / 'scripts/version_manager.py')
vm = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = vm
spec.loader.exec_module(vm)


def fixture(destination):
    config = json.loads((ROOT/'versioning.json').read_text(encoding='utf-8'))
    paths = {'versioning.json'}
    for unit in config['units'].values():
        paths.update(item['path'] for item in [unit['canonical'], *unit.get('mirrors', []), *unit.get('assertions', [])])
    for name in paths:
        target = destination/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT/name, target)
    return paths


def snapshot(root, paths):
    return {name:(root/name).read_bytes() for name in paths}


class RepositoryBindings(unittest.TestCase):
    def test_all_exact_sources_and_mirrors(self):
        manager = vm.Manager(ROOT)
        names = list(manager.config['units'])
        self.assertTrue(names)
        manager.inspect(names)

    def test_each_unit_preview_apply_isolation_and_idempotency(self):
        config = json.loads((ROOT/'versioning.json').read_text(encoding='utf-8'))
        for unit, info in config['units'].items():
            with self.subTest(unit=unit), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                paths = fixture(root)
                before = snapshot(root, paths)
                manager = vm.Manager(root)
                bindings = manager.inspect([unit])
                current = bindings[0].value
                if info.get('read_only'):
                    with self.assertRaises(vm.VersionError):
                        manager.plan(unit, part='patch', expect=current)
                    self.assertEqual(before, snapshot(root, paths))
                    continue
                part = 'prerelease' if bindings[0].version.pre else 'patch'
                with self.assertRaises(vm.VersionError):
                    manager.plan(unit, part=part, expect='999999.0.0')
                with self.assertRaises(vm.VersionError):
                    manager.plan(unit, value='01.2.3', expect=current)
                report, proposed = manager.plan(unit, part=part, expect=current)
                self.assertEqual(before, snapshot(root, paths), 'Preview must not write')
                manager.apply(proposed)
                changed = {p for p in paths if before[p] != (root/p).read_bytes()}
                self.assertEqual(changed, set(proposed))
                final = vm.Manager(root)
                # Prove byte-for-byte unit isolation even when units share a lockfile.
                # Only this unit's prevalidated literal spans may change.
                target = final.inspect([unit])[0].version
                edits = {}
                for binding in bindings:
                    value = target.render(binding.source.get('format', 'semver'))
                    rendered = json.dumps(value, ensure_ascii=False) if binding.encoding == 'json' else value
                    edits.setdefault(binding.source['path'], []).append((binding.start, binding.end, rendered))
                for path in paths:
                    expected = before[path].decode('utf-8')
                    for start, end, rendered in sorted(edits.get(path, []), reverse=True):
                        expected = expected[:start] + rendered + expected[end:]
                    self.assertEqual(expected.encode('utf-8'), (root/path).read_bytes())
                current = final.inspect([unit])[0].value
                _, again = final.plan(unit, value=current, expect=current)
                self.assertEqual({}, again)
                after = snapshot(root, paths)
                final.apply(again)
                self.assertEqual(after, snapshot(root, paths))

    def test_missing_source_and_invalid_manifest_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = fixture(root)
            manager = vm.Manager(root)
            unit = next(iter(manager.config['units']))
            source = manager.config['units'][unit]['canonical']['path']
            (root/source).unlink()
            with self.assertRaises((vm.VersionError, OSError)):
                vm.Manager(root).inspect([unit])

    def test_mirror_drift_is_not_silently_repaired(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = fixture(root)
            manager = vm.Manager(root)
            for unit, info in manager.config['units'].items():
                if not info.get('mirrors'):
                    continue
                binding = manager.inspect([unit])[1]
                path = root/binding.source['path']
                text = path.read_text(encoding='utf-8')
                value = '999.888.777'
                replacement = json.dumps(value) if binding.encoding=='json' else value
                path.write_text(text[:binding.start]+replacement+text[binding.end:],encoding='utf-8')
                before = snapshot(root, paths)
                with self.assertRaises(vm.VersionError):
                    vm.Manager(root).inspect([unit])
                self.assertEqual(before, snapshot(root, paths))
                return


if __name__ == '__main__':
    unittest.main()
