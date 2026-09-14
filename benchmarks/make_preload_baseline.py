#!/usr/bin/env python3
"""Generate a traversal-only preload ablation from the current service source."""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
from pathlib import Path


def generate(source: str) -> str:
    start = source.index('  // Backend-bounded traversal:')
    queue = source.index('  std::deque<std::pair<std::string, std::int64_t>> queue;', start)
    preload = '''  // BENCHMARK ABLATION: preload entities and relations before traversal.
  // Generated from the current service; all other query behavior is retained.
  const std::string frontier_column = direction == "backward" ? "src_id" : "dst_id";
  std::vector<Row> all_relations;
  if (function_id)
    all_relations = query_all(db,
        "SELECT * FROM relations WHERE function_id=? ORDER BY kind,ordinal,src_id,dst_id",
        {*function_id});
  std::map<std::string, llvm::json::Object> entities;
  for (const Row &row : query_all(db, "SELECT * FROM entities WHERE artifact_id=?",
                                {(*target)["artifact_id"].as_string()}))
    entities[row.at("id").as_string()] = entity_json(row);

'''
    source = source[:start] + preload + source[queue:]
    loop = '    for (const Row &edge : query_all(db, adjacency_sql, {*function_id, node})) {'
    if source.count(loop) != 1:
        raise ValueError('source layout changed: adjacency loop is not unique')
    source = source.replace(loop, '''    for (const Row &edge : all_relations) {
      if (edge.at(frontier_column).as_string() != node ||
          !kinds.count(edge.at("kind").as_string()))
        continue;''')
    start = source.index('  // Fetch entities for the visited ids only, in bounded IN(...) batches.')
    end = source.index('  llvm::json::Array nodes;', start)
    return source[:start] + source[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'src/service.cpp')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    original = args.source.read_text(encoding='utf-8')
    modified = generate(original)
    (args.output / 'service.cpp').write_text(modified, encoding='utf-8')
    (args.output / 'preload.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), modified.splitlines(True), fromfile='src/service.cpp',
        tofile='preload-ablation/service.cpp')), encoding='utf-8')
    metadata = {'kind': 'traversal-only-preload-ablation',
                'source_sha256': hashlib.sha256(original.encode()).hexdigest(),
                'generated_sha256': hashlib.sha256(modified.encode()).hexdigest(),
                'scope': 'preload all artifact entities and all target-function relations before BFS; '
                         'keep current connection reuse, envelope, projection and traversal semantics'}
    (args.output / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
