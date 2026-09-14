#!/usr/bin/env python3
"""Measure bounded LLVM queries on synthetic modules, optionally against a preload ablation."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import sqlite3
import statistics
import subprocess
import time
from pathlib import Path


def invoke(binary: Path, state: Path, *args: str, timeout: int = 900):
    start = time.perf_counter_ns()
    proc = subprocess.run([str(binary), "--state-dir", str(state), *args],
                          capture_output=True, timeout=timeout)
    elapsed = (time.perf_counter_ns() - start) / 1_000_000
    if proc.returncode:
        raise RuntimeError(proc.stderr.decode("utf-8", "replace"))
    return json.loads(proc.stdout), elapsed


def workload(functions: int, operations: int = 40) -> str:
    # f0: 40 adds; other functions: one direct call + 39 adds.
    # With the current adapter this yields 83*N+1 entities, 125*N-1 relations.
    lines = ['; Synthetic module for bounded-query measurements.',
             'source_filename = "bounded_queries.synthetic"', '']
    for fn in range(functions):
        lines += [f'define i32 @f{fn}(i32 %x) {{', 'entry:']
        previous = '%x'
        first = 0
        if fn:
            lines += [f'  %v0 = call i32 @f{fn - 1}(i32 %x)']
            previous, first = '%v0', 1
        for step in range(first, operations):
            lines += [f'  %v{step} = add i32 {previous}, {step + 1}']
            previous = f'%v{step}'
        lines += [f'  ret i32 {previous}', '}', '']
    return '\n'.join(lines)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cpu_model() -> str:
    cpuinfo = Path('/proc/cpuinfo')
    if cpuinfo.is_file():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith('model name'):
                return line.split(':', 1)[1].strip()
    return platform.processor()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--baseline', type=Path,
                        help='Optional separately built traversal-only preload ablation CLI.')
    parser.add_argument('--sizes', type=int, nargs='+', default=[1, 100, 1500])
    parser.add_argument('--repeats', type=int, default=7)
    parser.add_argument('--warmups', type=int, default=2)
    parser.add_argument('--output', type=Path, required=True,
                        help='New directory; existing output is refused.')
    args = parser.parse_args()
    if min(args.sizes) < 1 or args.repeats < 2 or args.warmups < 1:
        parser.error('sizes >= 1, repeats >= 2, and warmups >= 1 are required')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    binaries = {'indexed': args.binary.resolve()}
    if args.baseline:
        binaries['preload_ablation'] = args.baseline.resolve()
    report = {
        'schema_version': 1,
        'experiment': 'synthetic-bounded-query-benchmark',
        'harness_sha256': digest(Path(__file__)),
        'environment': {'platform': platform.platform(), 'machine': platform.machine(),
                        'processor': cpu_model(), 'python': platform.python_version(),
                        'cpu_count': os.cpu_count(), 'sqlite_python': sqlite3.sqlite_version},
        'workload_storage': 'Linux /mnt mounted drive' if args.output.as_posix().startswith('/mnt/') else 'local output directory',
        'binaries': {name: {'sha256': digest(path)} for name, path in binaries.items()},
        'method': {'clock': 'perf_counter_ns', 'unit': 'ms', 'warmups': args.warmups,
                   'repeats': args.repeats, 'cache': 'warm OS/file cache; fresh CLI process per sample',
                   'includes': 'CLI startup, SQLite reads, traversal, serialization, stdout capture',
                   'excludes': 'ingest, materialization, Python JSON decoding, MCP transport',
                   'order': 'alternate candidate/baseline order by repetition',
                   'budget_nodes': 50, 'budget_depth': 8},
        'cases': [],
    }
    for size in args.sizes:
        case_dir = args.output / str(size)
        case_dir.mkdir()
        ir, state = case_dir / 'module.ll', case_dir / 'state'
        ir.write_bytes(workload(size).encode('utf-8'))
        invoke(args.binary.resolve(), state, 'init', '--name', 'bounded-benchmark')
        print(f'ingesting {size} functions ...', flush=True)
        ingested, ingest_ms = invoke(args.binary.resolve(), state, 'ingest', 'llvm', str(ir), '--index', 'full')
        artifact = ingested['result']['artifact']
        catalog, _ = invoke(args.binary.resolve(), state, 'functions', '--artifact', artifact,
                            '--match', f'^f{size - 1}$')
        handle = catalog['result'][0]['handle']
        children, _ = invoke(args.binary.resolve(), state, 'show', handle, '--view', 'children',
                             '--kind', 'return', '--budget-nodes', '1')
        return_handle = children['result']['items'][0]['handle']
        with sqlite3.connect(state / 'investigation.sqlite') as db:
            entities = db.execute('SELECT count(*) FROM entities').fetchone()[0]
            relations = db.execute('SELECT count(*) FROM relations').fetchone()[0]
            plans = list(db.execute("EXPLAIN QUERY PLAN SELECT * FROM relations WHERE function_id=? "
                                   "AND src_id=? AND kind IN ('llvm.operand') "
                                   "ORDER BY kind,ordinal,src_id,dst_id", (handle, return_handle)))
        if (entities, relations) != (83 * size + 1, 125 * size - 1):
            raise RuntimeError(f'unexpected workload counts: {entities}, {relations}')
        case = {'functions': size, 'entities': entities, 'relations': relations,
                'artifact_sha256': digest(ir), 'ingest_ms_excluded': ingest_ms,
                'query_plan': [row[3] for row in plans], 'queries': {}}
        for name, binary in binaries.items():
            status, _ = invoke(binary, state, 'status')
            version = {k: v for k, v in status['result'].items()
                       if k.endswith('version') or k == 'build_revision'}
            report['binaries'][name]['versions'] = version
        commands = {
            'trace-return': ['trace-return', handle, '--budget-nodes', '50', '--budget-depth', '8', '--detail', 'graph'],
            'slice': ['slice', return_handle, '--relations', 'operand', '--budget-nodes', '50', '--budget-depth', '8'],
        }
        for name, command in commands.items():
            expected = None
            samples = {label: [] for label in binaries}
            for repetition in range(args.warmups + args.repeats):
                order = list(binaries.items())
                if repetition % 2:
                    order.reverse()
                for label, binary in order:
                    response, ms = invoke(binary, state, *command)
                    if expected is None:
                        expected = response
                    elif response != expected:
                        raise RuntimeError(f'{size}/{name}/{label}: response changed; timings are invalid')
                    if response['truncation']['visited_nodes'] != 16:
                        raise RuntimeError('expected a 16-node bounded query')
                    if repetition >= args.warmups:
                        samples[label].append(ms)
            record = {'argv': [part.replace(handle, '<function>').replace(return_handle, '<return>')
                               for part in command],
                      'responses_equal': True, 'truncation': expected['truncation'],
                      'unknowns': expected['unknowns'], 'capabilities_used': expected['capabilities_used'],
                      'timings': {label: {'samples_ms': values, 'median_ms': statistics.median(values),
                                          'min_ms': min(values), 'max_ms': max(values)}
                                  for label, values in samples.items()}}
            if args.baseline:
                record['preload_over_indexed_ratio'] = (record['timings']['preload_ablation']['median_ms'] /
                                                        record['timings']['indexed']['median_ms'])
            case['queries'][name] = record
            print(f'{size} {name}: ' + ', '.join(f'{k}={statistics.median(v):.3f} ms' for k, v in samples.items()), flush=True)
        report['cases'].append(case)
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(args.output / 'results.json', flush=True)


if __name__ == '__main__':
    main()
