"""Compile actual staged detour bodies against observable seam/trampoline doubles.

The temporary fragments are test inputs only, not production source copies.
"""
from pathlib import Path
import re
import subprocess
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
root = parser.parse_args().root
subprocess.run(['python', str(root / 'scripts/source-pipeline.py'), 'verify'], check=True)
stage = root / 'build/source/src'
out = root / 'build/shared-entry-tests'
out.mkdir(exist_ok=True)

def extract(text, marker):
    start = text.index(marker)
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"', lambda m: ' ' * len(m[0]), text)
    brace = masked.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return text[start:end] + '\n'

hook = (stage / 'GakumasLocalify/Hook.cpp').read_text(encoding='utf-8')
runtime = (stage / 'vr/unity/CameraRuntime.inc.cpp').read_text(encoding='utf-8')
fragments = ''.join(extract(runtime, 'bool ' + name + '(') for name in (
    'IsVrUnityRuntimeEnabled', 'AreVrUnityCameraDiagnosticsEnabled', 'IsLocalifyFreeCameraEnabled'))
fragments += (stage / 'vr/unity/EndCameraCallback.inc.cpp').read_text(encoding='utf-8')
fragments += (stage / 'vr/unity/MainCameraCallback.inc.cpp').read_text(encoding='utf-8')
fragments += extract(hook, 'DEFINE_HOOK(UnityResolve::UnityType::Camera*, Camera_get_main,')
for name, ret in [('CinemachineBrain_PushStateToUnityCamera', 'void'), ('EndCameraRendering', 'void'), ('VLDOF_IsActive', 'bool')]:
    fragments += extract(hook, f'DEFINE_HOOK({ret}, {name},')
(out / 'actual_hooks.inc').write_text(fragments, encoding='utf-8')
