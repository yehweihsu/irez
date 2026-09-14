#!/usr/bin/env python3
"""Run bounded return and store queries against a directory of generated LLVM IR."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True,
                        help='LLVM IR directory, such as generate.py output/dump.')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    source = args.artifacts.resolve()
    if not source.is_dir():
        parser.error('--artifacts must be an existing directory')
    files = sorted(source.rglob('*.ll'))
    if not files:
        parser.error('--artifacts contains no LLVM .ll files')
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    state = dest / 'state'

    def run(*parts):
        process = subprocess.run([str(binary), '--state-dir', str(state), *parts],
                                 capture_output=True, timeout=120)
        if process.returncode:
            raise RuntimeError(process.stderr.decode('utf-8', 'replace'))
        return json.loads(process.stdout)

    def portable(value):
        if isinstance(value, dict):
            return {k: portable(v) for k, v in value.items()}
        if isinstance(value, list):
            return [portable(v) for v in value]
        if isinstance(value, str):
            value = re.sub(r'(run|investigation):[0-9a-f-]{36}', r'\1:<local-id>', value)
            for path in (dest, source):
                value = value.replace(str(path), '<local>').replace(path.as_posix(), '<local>')
        return value

    run('init', '--name', 'jax-evidence')
    records = []
    for file in files:
        # Select relevant dump modules before ingest; function discovery still uses returned handles.
        if not any(word in file.read_text(encoding='utf-8') for word in ('fdiv', 'fsub', 'fmul')):
            continue
        ingested = run('ingest', 'llvm', str(file), '--index', 'full')
        artifact = ingested['result']['artifact']
        capabilities = run('capabilities', '--artifact', artifact)
        functions = run('functions', '--artifact', artifact)
        for function in functions['result']:
            if function['status'] == 'declaration_only':
                continue
            handle = function['handle']
            traced = run('trace-stores', handle, '--budget-nodes', '50', '--budget-depth', '8',
                         '--detail', 'graph')
            if not traced['result']['store_count']:
                continue
            returned = run('trace-return', handle, '--budget-nodes', '50', '--budget-depth', '8')
            sites = traced['result']['sites']
            records.append({'file': file.relative_to(source).as_posix(),
                            'sha256': hashlib.sha256(file.read_bytes()).hexdigest(),
                            'function': function['name'], 'handle': handle,
                            'capabilities': capabilities['result'],
                            'trace_return': portable(returned), 'trace_stores': portable(traced)})
            print(json.dumps({'file': file.name, 'function': function['name'],
                              'stores': len(sites), 'nodes': [s['node_count'] for s in sites],
                              'truncated': traced['truncation']['truncated']}), flush=True)
    if not records:
        raise RuntimeError('No store-sink traces found in supplied LLVM IR')
    status = run('status')['result']
    version = {k: v for k, v in status.items() if k.endswith('version') or k == 'build_revision'}
    report = {'schema_version': 1, 'versions': version,
              'scope': 'function-local backward SSA operand evidence; 50 nodes / depth 8 per store',
              'limitations': ['No memory dependence or solved path conditions.',
                              'No runtime value or bug diagnosis follows from these static graphs.',
                              'Run UUIDs are masked in this shareable report; full records remain in its local state DB.'],
              'artifacts': records}
    (dest / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
