#!/usr/bin/env python3
"""Generate JAX CPU LLVM IR for a cross-product/self-division example.

The operation sequence is documented at https://github.com/jax-ml/jax/issues/38602.
It records observed outputs without asserting which result is correct.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['eager', 'jit_f', 'jit_cross'], default='jit_f')
    parser.add_argument('--output', type=Path, required=True, help='New output directory.')
    args = parser.parse_args()
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    # A relative dump directory avoids XLA_FLAGS parsing absolute paths with spaces.
    os.chdir(dest)
    if os.environ.get('XLA_FLAGS'):
        raise RuntimeError('Run in a fresh process with XLA_FLAGS unset for a recorded, controlled dump')
    os.environ['XLA_FLAGS'] = '--xla_dump_to=dump --xla_dump_hlo_as_text'
    os.environ['JAX_PLATFORMS'] = 'cpu'
    import jax
    import jax.numpy as jnp
    import jaxlib
    import numpy as np

    def f(values):
        cross = jnp.cross(values, values)
        return cross / cross

    values = jnp.asarray([[0.1, 0.9]], dtype=jnp.float32)
    if args.mode == 'jit_f':
        result = jax.jit(f)(values)
    elif args.mode == 'jit_cross':
        cross = jax.jit(lambda values: jnp.cross(values, values))(values)
        result = cross / cross
    else:
        result = f(values)
    observed = np.asarray(result.block_until_ready())
    ll_files = sorted((dest / 'dump').glob('*.ll'))
    if not ll_files:
        raise RuntimeError('This jaxlib emitted no LLVM .ll files; inspect its XLA dump options')
    record = {
        'schema_version': 1, 'kind': 'jax-cpu-llvm-generation',
        'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'case_reference': 'https://github.com/jax-ml/jax/issues/38602',
        'mode': args.mode, 'input': np.asarray(values).tolist(),
        'output': [float(v) if math.isfinite(float(v)) else str(float(v)) for v in observed.flat],
        'output_shape': list(observed.shape), 'output_dtype': str(observed.dtype),
        'versions': {'python': platform.python_version(), 'jax': jax.__version__,
                     'jaxlib': jaxlib.__version__, 'numpy': np.__version__,
                     'platform': platform.platform()},
        'xla_flags': os.environ['XLA_FLAGS'],
        'devices': [str(device) for device in jax.devices()],
        'artifacts': [{'file': file.relative_to(dest).as_posix(),
                       'sha256': hashlib.sha256(file.read_bytes()).hexdigest()}
                      for file in ll_files],
        'limitation': 'Runtime outputs are external observations, not inferred from IREZ; '
                      'compiler behavior and dump names vary by jaxlib and CPU.',
    }
    (dest / 'generation.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'mode': args.mode, 'output': record['output'], 'llvm_files': len(ll_files)}))


if __name__ == '__main__':
    main()
