#!/usr/bin/env python3
"""Linux-only benchmark build using an existing LLVM SDK and SQLiteCpp checkout.

Produces two CLIs with the same compiler/options/dependencies. This optional
experiment uses system sqlite3 and shared LLVM; official releases use CMake.
No packages are installed and no production source is edited.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import shlex
import subprocess
from pathlib import Path


def output(*command):
    return subprocess.check_output(command, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-config', default='llvm-config')
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--sqlitecpp', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    sqlite = args.sqlitecpp.resolve()
    subprocess.run(['python3', str(root / 'benchmarks/make_preload_baseline.py'),
                    '--output', str(dest / 'preload')], check=True)
    flags = [flag for flag in shlex.split(output(args.llvm_config, '--cxxflags'))
             if not flag.startswith('-std=') and flag != '-fno-exceptions']
    revision = output('git', '-C', str(root), 'rev-parse', '--short=12', 'HEAD')
    version = json.loads((root / 'contract.json').read_text())['irez_version']
    flags += ['-std=c++20', '-O2', '-DNDEBUG', '-fexceptions', '-pthread',
              '-I' + str(root / 'src'), '-I' + str(sqlite / 'include'),
              f'-DIREZ_VERSION="{version}"', f'-DIREZ_BUILD_REVISION="{revision}"']
    common = [root / 'src' / (name + '.cpp') for name in
              ('adapter', 'db', 'envelope', 'store', 'util', 'cli_main')]
    common += sorted((sqlite / 'src').glob('*.cpp'))
    if not (sqlite / 'include/SQLiteCpp/Database.h').is_file() or len(common) < 10:
        raise RuntimeError('SQLiteCpp checkout is incomplete')
    sources = common + [root / 'src/service.cpp', dest / 'preload/service.cpp']
    records = []

    def compile_one(pair):
        i, source = pair
        obj = dest / f'{i}.o'
        command = [args.compiler, *flags, '-c', str(source), '-o', str(obj)]
        proc = subprocess.run(command, text=True, capture_output=True)
        if proc.returncode:
            raise RuntimeError(proc.stdout + proc.stderr)
        print('compiled ' + source.name, flush=True)
        return obj, {'source': str(source.relative_to(root)) if source.is_relative_to(root) else str(source),
                     'sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'command': command}

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        compiled = list(pool.map(compile_one, enumerate(sources)))
    objects = [pair[0] for pair in compiled]
    records = [pair[1] for pair in compiled]
    libraries = shlex.split(output(args.llvm_config, '--ldflags', '--libs',
                                  'core', 'irreader', 'bitreader', 'analysis', 'support', '--system-libs'))
    for label, service in [('indexed', objects[-2]), ('preload-ablation', objects[-1])]:
        command = [args.compiler, *map(str, objects[:-2]), str(service), *libraries,
                   '-lsqlite3', '-pthread', '-o', str(dest / ('irez-' + label))]
        subprocess.run(command, check=True)
    (dest / 'build.json').write_text(json.dumps({
        'compiler': output(args.compiler, '--version'),
        'llvm': output(args.llvm_config, '--version'), 'source_revision': revision,
        'flags': flags, 'libraries': libraries + ['-lsqlite3', '-pthread'],
        'sources': records,
        'baseline': 'traversal-only preload ablation of the current service',
    }, indent=2) + '\n')
    print(dest, flush=True)


if __name__ == '__main__':
    main()
