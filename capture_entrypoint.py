# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run a prebuilt, verified private Python service bundle, optionally recording.

No installer, running-process injection, service edits, or implicit capture.
The bundle directory contains service/, capture/, and runtime-manifest.json.
"""
import argparse
import hashlib
import json
import logging
from pathlib import Path, PurePosixPath
import re
import sys


def verify_bundle(root):
    root = Path(root).resolve(strict=True)
    manifest = root / 'runtime-manifest.json'
    if manifest.is_symlink() or manifest.stat().st_size > 65536:
        raise ValueError('runtime manifest bounds')
    rows = json.loads(manifest.read_text(encoding='utf-8'))
    if not isinstance(rows, dict) or not 1 <= len(rows) <= 128:
        raise ValueError('runtime manifest shape')
    total = 0
    for name, expected in rows.items():
        relative = PurePosixPath(name)
        if (not relative.parts or not isinstance(expected, str)
                or not re.fullmatch('[0-9a-f]{64}', expected)
                or relative.is_absolute() or '..' in relative.parts
                or '\\' in name or ':' in name
                or relative.parts[0] not in ('service', 'capture', 'capture_entrypoint.py')):
            raise ValueError('runtime manifest path/hash')
        path = root.joinpath(*relative.parts)
        if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root):
            raise ValueError('runtime file alias')
        size = path.stat().st_size
        total += size
        if size > 8*1024*1024 or total > 32*1024*1024:
            raise ValueError('runtime source bounds')
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(65536), b''):
                digest.update(chunk)
        if digest.hexdigest() != expected:
            raise ValueError('runtime file drift')
    required = {'service/mlat/main.py', 'service/mlat/coordinator.py',
                'service/mlat/jsonclient.py', 'service/mlat/mlattrack.py',
                'service/mlat/output.py', 'service/mlat/tracker.py',
                'capture/startup.py', 'capture_entrypoint.py'}
    if not required <= rows.keys():
        raise ValueError('runtime required sources absent')
    return root


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--enable-capture', action='store_true')
    parser.add_argument('--capture-directory')
    parser.add_argument('--capture-seconds', type=int, default=120)
    parser.add_argument('server_arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not 1 <= args.capture_seconds <= 120:
        parser.error('capture duration must be 1..120 seconds')
    if args.enable_capture and not args.capture_directory:
        parser.error('explicit private capture directory required')
    if args.server_arguments[:1] != ['--']:
        parser.error('separate upstream server arguments with --')
    return args


def main(argv=None):
    args = arguments(sys.argv[1:] if argv is None else argv)
    root = verify_bundle(Path(__file__).resolve().parent)
    sys.path.insert(0, str(root / 'service'))
    sys.path.insert(0, str(root))
    from mlat import main as upstream, coordinator, tracker
    from capture.startup import server_class
    # Match the pinned executable's logging configuration exactly.
    logging.basicConfig(level=logging.DEBUG, style='{',
                        format='{asctime}.{msecs:03.0f} {name:12} {message}',
                        datefmt='%Y%m%d %H:%M:%S')
    for handler in logging.getLogger().handlers:
        handler.setLevel(logging.WARNING)
    sys.argv = ['mlat-server', *args.server_arguments[1:]]
    server_class(upstream, coordinator, tracker, args.capture_directory,
                 enabled=args.enable_capture, duration=args.capture_seconds)().run()


if __name__ == '__main__':
    main()
