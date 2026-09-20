import copy
import hashlib
import difflib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('pipeline', ROOT / 'scripts/source-pipeline.py')
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


class StagingTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='pipeline-test-')
        self.root = Path(self.temp.name)
        self.upstream = self.root / '.upstream/localify'
        self.upstream.mkdir(parents=True)
        self.git('init', '-q')
        self.git('config', 'core.autocrlf', 'false')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        for path, content in [('src/sample.cpp', 'int value() { return 1; }\n'), ('deps/fixture.txt', 'dep\n')]:
            target = self.upstream / path
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(content.encode('utf-8'))
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.lock = {'commit': self.git('rev-parse', 'HEAD'), 'tree': self.git('rev-parse', 'HEAD^{tree}'), 'sourceLayout': 'checkout'}
        self.write('upstream.lock.json', self.lock)
        for prefix in pipeline.OWNED:
            (self.root / prefix).mkdir(parents=True)
        (self.root / 'src/vr/owned.cpp').write_text('int owned = 1;\n')
        (self.root / 'scripts').mkdir()
        for name in ['source-pipeline.py', 'CMakeLists.txt']:
            (self.root / 'scripts' / name).write_text('fixture\n')
        (self.root / 'patches').mkdir()
        self.before = b'int value() { return 1; }\n'
        self.after = b'int value() { return 2; }\n'
        patch = ''.join(difflib.unified_diff(self.before.decode().splitlines(True), self.after.decode().splitlines(True), fromfile='a/src/sample.cpp', tofile='b/src/sample.cpp')).encode()
        (self.root / 'patches/test.patch').write_bytes(patch)
        self.entry = {'file': 'test.patch', 'sha256': pipeline.sha(patch), 'before': {'src/sample.cpp': pipeline.sha(self.before)}, 'after': {'src/sample.cpp': pipeline.sha(self.after)}}
        self.entry['inputBlobs'] = {'src/sample.cpp': hashlib.sha1(b'blob ' + str(len(self.before)).encode() + b'\0' + self.before).hexdigest()}
        self.manifest = {'upstreamCommit': self.lock['commit'], 'patches': [self.entry]}
        self.write('patches/manifest.json', self.manifest)

    def tearDown(self):
        # TemporaryDirectory is created beneath the resolved build directory.
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.upstream), *args], text=True).strip()

    def write(self, path, value):
        (self.root / path).write_text(json.dumps(value), encoding='utf-8')

    def test_stage_and_binary_receipt(self):
        pipeline.stage_sources(self.root)
        pipeline.verify(self.root)
        self.assertEqual((self.root / 'build/source/src/sample.cpp').read_bytes(), self.after)
        binary = self.root / 'build/version.dll'
        binary.write_bytes(b'fixture DLL')
        pipeline.binary_receipt(self.root, binary, True)
        pipeline.binary_receipt(self.root, binary, False)
        binary.write_bytes(b'changed DLL')
        with self.assertRaises(RuntimeError): pipeline.binary_receipt(self.root, binary, False)

    def test_lock_and_tree(self):
        for key in ['commit', 'tree']:
            changed = dict(self.lock, **{key: '0' * 40})
            self.write('upstream.lock.json', changed)
            with self.assertRaises((RuntimeError, subprocess.CalledProcessError)):
                pipeline.stage_sources(self.root)

    def test_missing_and_changed_patch(self):
        patch = self.root / 'patches/test.patch'
        patch.write_bytes(b'bad patch')
        with self.assertRaises(RuntimeError): pipeline.stage_sources(self.root)
        patch.unlink()
        with self.assertRaises(FileNotFoundError): pipeline.stage_sources(self.root)

    def test_preimage_and_output(self):
        for side in ['before', 'after']:
            broken = copy.deepcopy(self.manifest)
            broken['patches'][0][side]['src/sample.cpp'] = '0' * 64
            self.write('patches/manifest.json', broken)
            with self.assertRaises(RuntimeError): pipeline.stage_sources(self.root)
            self.assertFalse((self.root / 'build/source-receipt.json').exists())

    def test_context_mismatch(self):
        path = self.root / 'patches/test.patch'
        patch = path.read_bytes().replace(b'-int value() { return 1; }', b'-int missing() { return 1; }')
        path.write_bytes(patch)
        self.entry['sha256'] = pipeline.sha(patch)
        self.write('patches/manifest.json', self.manifest)
        with self.assertRaises(subprocess.CalledProcessError): pipeline.stage_sources(self.root)
        self.assertFalse((self.root / 'build/source-receipt.json').exists())

    def test_input_blob_mismatch(self):
        self.entry['inputBlobs']['src/sample.cpp'] = '0' * 40
        self.write('patches/manifest.json', self.manifest)
        with self.assertRaises(RuntimeError): pipeline.stage_sources(self.root)
        self.assertFalse((self.root / 'build/source-receipt.json').exists())

    def test_stale_and_tampered_sources(self):
        pipeline.stage_sources(self.root)
        (self.root / 'src/vr/owned.cpp').write_text('int owned = 2;\n')
        with self.assertRaises(RuntimeError): pipeline.verify(self.root)
        pipeline.stage_sources(self.root)
        (self.root / 'build/source/src/sample.cpp').write_text('tampered\n')
        with self.assertRaises(RuntimeError): pipeline.verify(self.root)

    def test_modified_upstream_rejects_stage_and_binary(self):
        pipeline.stage_sources(self.root)
        binary = self.root / 'build/version.dll'
        binary.write_bytes(b'old binary')
        pipeline.binary_receipt(self.root, binary, True)
        (self.upstream / 'src/sample.cpp').write_bytes(b'changed upstream\n')
        with self.assertRaises(RuntimeError): pipeline.verify(self.root)
        with self.assertRaises(RuntimeError): pipeline.stage_sources(self.root)
        self.assertFalse((self.root / 'build/source-receipt.json').exists())
        with self.assertRaises((RuntimeError, FileNotFoundError)):
            pipeline.binary_receipt(self.root, binary, False)

    def test_failed_patch_invalidates_previous_receipt(self):
        pipeline.stage_sources(self.root)
        (self.root / 'patches/test.patch').unlink()
        with self.assertRaises(FileNotFoundError): pipeline.stage_sources(self.root)
        self.assertFalse((self.root / 'build/source-receipt.json').exists())


if __name__ == '__main__':
    unittest.main()
