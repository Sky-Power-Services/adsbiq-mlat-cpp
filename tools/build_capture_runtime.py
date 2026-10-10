# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build an isolated capture runtime. Never installs or edits a service.

Linux only. Output must be new. Optional prebuilt Cython modules require a
separate exact hash manifest; otherwise the supplied Cython sources are built.
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import sysconfig
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def digest(path):
    if path.is_symlink() or path.stat().st_size > 8*1024*1024:
        raise ValueError('source size/alias bounds')
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(65536), b''): result.update(chunk)
    return result.hexdigest()


def build(source, output, *, tracker=None, native_dir=None, native_manifest=None):
    from capture.instrument import instrument_coordinator, instrument_jsonclient, instrument_mlattrack, instrument_output
    from capture_entrypoint import verify_bundle
    if sys.platform != 'linux': raise ValueError('Linux build required')
    source = Path(source).resolve(strict=True); output = Path(output).absolute()
    if output.exists() or output.is_relative_to(source): raise ValueError('new separate output required')
    if bool(native_dir) != bool(native_manifest): raise ValueError('prebuilt modules require exact manifest')
    paths = [source/'setup.py']
    for package in ('mlat', 'modes_cython'):
        paths += list((source/package).glob('*.py')) + list((source/package).glob('*.pyx'))
    if len(paths) > 64 or sum(p.stat().st_size for p in paths) > 2*1024*1024:
        raise ValueError('upstream source bounds')
    inputs = {p.relative_to(source).as_posix(): digest(p) for p in paths}
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', OPENBLAS_NUM_THREADS='1', OMP_NUM_THREADS='1')
    with tempfile.TemporaryDirectory(prefix='mlat-runtime-') as tmp:
        stage = Path(tmp)/'service'; stage.mkdir()
        for path in paths:
            target = stage/path.relative_to(source); target.parent.mkdir(exist_ok=True)
            shutil.copyfile(path, target)
        if tracker is not None:
            tracker = Path(tracker)
            if digest(tracker) != '427437076c73aaac1282a166b81fd929cbef41a3e58e2b2361c4ecde9fe7ec1b':
                raise ValueError('deployed tracker drift')
            shutil.copyfile(tracker, stage/'mlat/tracker.py')
            inputs['mlat/tracker.py'] = digest(tracker)
        for name, transform in (('coordinator', instrument_coordinator), ('jsonclient', instrument_jsonclient),
                                ('mlattrack', instrument_mlattrack), ('output', instrument_output)):
            path = stage/'mlat'/(name+'.py'); path.write_text(transform(path.read_bytes()), encoding='utf-8')
        if native_dir:
            manifest = Path(native_manifest)
            if manifest.stat().st_size > 65536: raise ValueError('native manifest bounds')
            rows = json.loads(manifest.read_text())['files']
            suffix = sysconfig.get_config_var('EXT_SUFFIX')
            expected = {name+suffix for name in ('mlat/clocktrack', 'mlat/geodesy', 'modes_cython/message')}
            if len(rows) != 3 or {row['path'] for row in rows} != expected:
                raise ValueError('native module ABI/path scope')
            for row in rows:
                original = Path(native_dir)/row['path']
                if original.stat().st_size != row['bytes'] or digest(original) != row['sha256']:
                    raise ValueError('native module drift')
                shutil.copyfile(original, stage/row['path'])
        else:
            result = subprocess.run([sys.executable, '-B', 'setup.py', 'build_ext', '--inplace'], cwd=stage,
                env=dict(env, CFLAGS='-O2 -ffp-contract=off'), capture_output=True, text=True, timeout=180)
            if result.returncode: raise RuntimeError('Cython build failed: '+result.stderr[-2000:])
        compile_command = ['gcc', '-O3', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
            '-I'+sysconfig.get_path('include'), str(ROOT/'capture/native_ingress.c'),
            '-o', str(stage/'_mlat_capture_native.so')]
        result = subprocess.run(compile_command, env=env, capture_output=True, text=True, timeout=30)
        if result.returncode: raise RuntimeError('capture extension build failed: '+result.stderr[-2000:])
        output.mkdir(mode=0o755)
        selected = list(stage.glob('*.so'))
        for package in ('mlat', 'modes_cython'):
            selected += list((stage/package).glob('*.py')) + list((stage/package).glob('*.so'))
        for path in selected:
            target = output/'service'/path.relative_to(stage); target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        (output/'capture').mkdir()
        for path in (ROOT/'capture').glob('*.py'): shutil.copyfile(path, output/'capture'/path.name)
        shutil.copyfile(ROOT/'capture_entrypoint.py', output/'capture_entrypoint.py')
        runtime = [output/'capture_entrypoint.py', *(output/'capture').glob('*.py')]
        runtime += [output/'service'/p.relative_to(stage) for p in selected]
        rows = {p.relative_to(output).as_posix(): digest(p) for p in runtime}
        (output/'runtime-manifest.json').write_text(json.dumps(rows, indent=2))
        verify_bundle(output)
        versions = {name: importlib.metadata.version(name) for name in
                    ('numpy', 'scipy', 'ujson', 'uvloop', 'Cython', 'pykalman', 'python-graph-core')}
        report = dict(kind='private_runtime_build_NOT_deployment', files=len(rows),
                      runtime_bytes=sum(p.stat().st_size for p in runtime),
                      runtime_manifest_sha256=digest(output/'runtime-manifest.json'),
                      source_hashes=inputs, python=sys.version, libc=platform.libc_ver(), packages=versions,
                      cython_modules='hash_verified_prebuilt' if native_dir else 'rebuilt_from_source',
                      capture_native_source_sha256=digest(ROOT/'capture/native_ingress.c'),
                      capture_compile_flags=['-O3', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC'])
        (output/'build-info.json').write_text(json.dumps(report, indent=2))
        return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'python-reference')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tracker', type=Path)
    parser.add_argument('--native-dir', type=Path)
    parser.add_argument('--native-manifest', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.output, tracker=args.tracker,
                           native_dir=args.native_dir, native_manifest=args.native_manifest)))
