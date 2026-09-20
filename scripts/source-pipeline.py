"""Fail-closed build staging. Source files remain owned by their locked origin."""
import argparse
import hashlib
import io
import json
import os
import runpy
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

OWNED = ('src/host', 'src/hooks', 'src/vr', 'deps/openxr')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def files(root):
    return sorted(p for p in root.rglob('*') if p.is_file())


def manifest(root):
    lock = read_json(root / 'upstream.lock.json')
    patches = read_json(root / 'patches/manifest.json')
    if patches['upstreamCommit'] != lock['commit']:
        raise RuntimeError('Patch manifest does not match upstream lock')
    return lock, patches


def inputs(root):
    lock, patches = manifest(root)
    paths = [root / 'upstream.lock.json', root / 'patches/manifest.json',
             root / 'scripts/source-pipeline.py', root / 'scripts/CMakeLists.txt']
    ownership = root / 'scripts/check-source-ownership.py'
    if ownership.exists():
        paths.append(ownership)
    for prefix in OWNED:
        paths.extend(files(root / prefix))
    for entry in patches['patches']:
        if set(entry['before']) != set(entry['after']) or set(entry['before']) != set(entry['inputBlobs']):
            raise RuntimeError('Patch must declare matching input/output paths')
        path = root / 'patches' / entry['file']
        if sha(path.read_bytes()) != entry['sha256']:
            raise RuntimeError(f'Patch digest mismatch: {path}')
        paths.append(path)
    return sha(json.dumps([(p.relative_to(root).as_posix(), sha(p.read_bytes()))
                           for p in sorted(set(paths))], separators=(',', ':')).encode())


def outputs(stage):
    return {p.relative_to(stage).as_posix(): sha(p.read_bytes()) for p in files(stage)}


def verify_upstream(root):
    lock, _ = manifest(root)
    checkout = lock.get('sourceLayout', 'in-tree') == 'checkout'
    upstream = root / '.upstream/localify' if checkout else root
    def git(*args):
        return subprocess.check_output(['git', '-c', 'core.safecrlf=false', '-C', str(upstream), *args], text=True).strip()
    if git('rev-parse', lock['commit'] + '^{tree}') != lock['tree']:
        raise RuntimeError('Locked upstream tree mismatch')
    if checkout and git('rev-parse', 'HEAD') != lock['commit']:
        raise RuntimeError('Upstream checkout commit mismatch')
    paths = set(git('ls-tree', '-r', '--name-only', lock['commit']).splitlines())
    paths -= set(lock.get('omittedPaths', []))
    changed = set(git('diff', '--name-only', lock['commit'], '--').splitlines())
    if paths & changed:
        raise RuntimeError('Modified upstream files: ' + ', '.join(sorted(paths & changed)))
    if checkout and git('status', '--porcelain=v1', '--untracked-files=all', '--ignored'):
        raise RuntimeError('Upstream checkout is not clean')
    ownership = root / 'scripts/check-source-ownership.py'
    if ownership.exists():
        runpy.run_path(str(ownership))['check'](root)


def verify(root):
    verify_upstream(root)
    receipt = read_json(root / 'build/source-receipt.json')
    if receipt['inputs'] != inputs(root):
        raise RuntimeError('Build inputs changed; run build.ps1 to prepare sources')
    if receipt['files'] != outputs(root / 'build/source'):
        raise RuntimeError('Staged source mismatch; run build.ps1 to prepare sources')
    return receipt


def stage_sources(root):
    receipt_path = root / 'build/source-receipt.json'
    try:
        verify_upstream(root)
        lock, patches = manifest(root)
        fingerprint = inputs(root)
    except Exception:
        receipt_path.unlink(missing_ok=True)
        raise
    if receipt_path.exists():
        try:
            verify(root)
            print('Staged source is current')
            return
        except (RuntimeError, OSError, ValueError):
            pass
    receipt_path.unlink(missing_ok=True)
    stage = root / 'build/source'
    if stage.resolve().parent != (root / 'build').resolve():
        raise RuntimeError('Unsafe staging directory')
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    upstream = root if lock.get('sourceLayout', 'in-tree') == 'in-tree' else root / '.upstream/localify'
    actual_tree = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse',
                                           lock['commit'] + '^{tree}'], text=True).strip()
    if actual_tree != lock['tree']:
        raise RuntimeError('Locked upstream tree mismatch')
    archive = subprocess.check_output(['git', '-c', 'core.autocrlf=false', '-C', str(upstream), 'archive', lock['commit'], 'src', 'deps'])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            destination = (stage / member.name).resolve()
            if not destination.is_relative_to(stage.resolve()) or member.issym() or member.islnk():
                raise RuntimeError('Unsafe upstream archive entry')
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(tar.extractfile(member).read())
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(stage.parent))
    for entry in patches['patches']:
        previous = outputs(stage)
        for path, expected in entry['before'].items():
            target = (stage / path).resolve()
            if not target.is_relative_to(stage.resolve()) or sha(target.read_bytes()) != expected:
                raise RuntimeError(f'Patch preimage mismatch: {entry["file"]}: {path}')
            data = target.read_bytes()
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            if blob != entry['inputBlobs'][path]:
                raise RuntimeError(f'Patch input blob mismatch: {entry["file"]}: {path}')
        patch = root / 'patches' / entry['file']
        for check in (True, False):
            command = ['git', '-c', 'core.autocrlf=false', 'apply', '--whitespace=nowarn']
            if check:
                command.append('--check')
            subprocess.run(command + [str(patch)], cwd=stage, env=env, check=True)
        for path, expected in entry['after'].items():
            if sha((stage / path).read_bytes()) != expected:
                raise RuntimeError(f'Patch output mismatch: {entry["file"]}: {path}')
        current = outputs(stage)
        changed = {p for p in previous.keys() | current.keys() if previous.get(p) != current.get(p)}
        if changed != set(entry['before']):
            raise RuntimeError(f'Patch changed undeclared paths: {entry["file"]}')
    for prefix in OWNED:
        target = stage / prefix
        if target.exists():
            raise RuntimeError(f'Upstream/local ownership collision: {prefix}')
        shutil.copytree(root / prefix, target)
    receipt_path.write_text(json.dumps({'inputs': fingerprint, 'files': outputs(stage)}, indent=2) + '\n', encoding='utf-8')
    print('Prepared verified upstream and local sources')


def binary_receipt(root, binary, record):
    receipt = verify(root)
    path = binary.with_suffix('.build.json')
    expected = {'inputs': receipt['inputs'], 'binary': sha(binary.read_bytes())}
    if record:
        path.write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8')
    elif read_json(path) != expected:
        raise RuntimeError('Binary is stale or has no matching build receipt')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['stage', 'verify', 'record-binary', 'verify-binary'])
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--binary', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.action == 'stage':
        stage_sources(root)
    elif args.action == 'verify':
        verify(root)
    else:
        binary_receipt(root, args.binary.resolve(), args.action == 'record-binary')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'Source pipeline failed: {error}', file=sys.stderr)
        sys.exit(1)
