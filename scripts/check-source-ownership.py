"""Candidate-tree boundary and conservative long-function copy detector.

Fingerprints are a review aid, not proof of semantic independence. This checks
existing tracked files plus untracked nonignored additions, so deletions can be
reviewed before committing. Generated build inputs are never eligible sources.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

def bodies(text):
    text = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', '', text)
    # Preserve literals in fingerprints but mask braces within them for parsing.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', lambda m: ' ' * len(m[0]), text)
    for match in re.finditer(r'\)\s*(?:const\s*)?(?:noexcept\s*)?\{', masked):
        start = match.end() - 1
        depth, end = 1, start + 1
        while depth and end < len(masked):
            depth += (masked[end] == '{') - (masked[end] == '}')
            end += 1
        tokens = re.findall(r'\w+|[^\s]', text[start:end])
        if len(tokens) >= 160:
            yield hashlib.sha256(' '.join(tokens).encode()).hexdigest(), text.count('\n', 0, start) + 1

def check(root):
    lock = json.loads((root / 'upstream.lock.json').read_text(encoding='utf-8'))
    checkout = lock['sourceLayout'] == 'checkout'
    upstream = root / '.upstream/localify' if checkout else root
    def git(where, *args):
        return subprocess.check_output(['git', '-C', str(where), *args])
    candidates = set(git(root, 'ls-files', '--cached', '--others', '--exclude-standard', '-z').decode().split('\0'))
    candidates = sorted(p for p in candidates if p and (root / p).is_file())
    errors = []
    retired = ('VrLocalifyHook.cpp', 'LocalifyI18n.cpp', 'ImGuiDraw.cpp')
    for path in candidates:
        if path.startswith(('build/', '.upstream/')) or Path(path).name in retired:
            errors.append('forbidden source: ' + path)
        if checkout and path.startswith('src/') and not path.startswith(('src/host/', 'src/hooks/', 'src/vr/')):
            errors.append('public upstream source: ' + path)
    if lock['substitutions']:
        errors.append('whole-file substitutions must be empty')
    originals = ['src/GakumasLocalify/Hook.cpp', 'src/main.cpp', 'src/windowsPlatform.cpp',
                 'src/GakumasLocalify/config/Config.cpp', 'src/gkmsGUI/GUII18n.cpp', 'src/dllproxy/proxy.cpp']
    fingerprints = {}
    for path in originals:
        text = git(upstream, 'show', lock['commit'] + ':' + path).decode('utf-8')
        for digest, line in bodies(text):
            fingerprints[digest] = f'{path}:{line}'
    for path in candidates:
        if (not checkout and not path.startswith(('src/host/', 'src/hooks/', 'src/vr/'))) or Path(path).suffix not in ('.cpp', '.hpp', '.h'):
            continue
        text = (root / path).read_text(encoding='utf-8-sig')
        for digest, line in bodies(text):
            if digest in fingerprints:
                errors.append(f'function copy requires review: {path}:{line} = {fingerprints[digest]} ({digest})')
    if errors:
        raise RuntimeError('\n'.join(errors))
    print(f'Source ownership OK: {len(candidates)} candidate files; no unreviewed long upstream body copies')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    check(parser.parse_args().root)
