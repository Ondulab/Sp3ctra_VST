#!/usr/bin/env python3
"""Compare real C render kernels against a pre-change source snapshot (argument)."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
here = Path(__file__).resolve().parent
current = here.parent.parent / 'source'
baseline = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='sp3ctra-parity-') as directory:
    temp = Path(directory)
    for kernel in ('luxwave', 'luxstral', 'spectral'):
        outputs = []
        for name, root in [('before', baseline), ('after', current)]:
            executable = temp / f'{kernel}-{name}'
            if kernel == 'luxwave':
                sources = [here/'luxwave_render.c', root/'synthesis/luxwave/synth_luxwave_engine.c']
                flags = []
            elif kernel == 'spectral':
                sources = [here/'spectral_render.c', root/'synthesis/luxsynth/synth_luxsynth_engine.c',
                           root/'synthesis/luxgrain/synth_luxgrain_engine.c', root/'synthesis/common/voice_manager.c']
                flags = []
            else:
                original = (root/'synthesis/luxstral/synth_luxstral_threading.c').read_text()
                prefix = original[:original.index('int synth_init_thread_pool(')]
                start = original.index('void synth_process_worker_range(')
                end = original.index('\n}\n', start) + 3
                extracted = temp/f'worker-{name}.c'
                extracted.write_text(prefix + original[start:end])
                sources = [here/'luxstral_worker_render.c', extracted,
                           root/'synthesis/luxstral/synth_luxstral_math.c',
                           root/'synthesis/luxstral/synth_luxstral_algorithms.c', next(root.rglob('pow_approx.c'))]
                flags = ['-DVST_MODE=1', '-DNO_SFML=1', '-Wl,-dead_strip']
                if 'thread_maxVolumeBuffer' in (root/'synthesis/luxstral/synth_luxstral_threading.h').read_text():
                    flags += ['-DBASELINE=1']
            includes = [root, root/'synthesis/luxstral', root/'synthesis/common', root/'config',
                        root/'utils', root/'core', root/'processing', root/'audio/buffers']
            includes += sorted({p.parent for p in root.rglob('*.h') if 'compat' not in p.parts} - set(includes))
            command = ['clang', '-O2', '-std=c11', *flags]
            for path in includes: command += ['-I', str(path)]
            command += [str(s) for s in sources] + ['-lm', '-lpthread', '-o', str(executable)]
            subprocess.run(command, check=True)
            data = subprocess.check_output([str(executable)])
            outputs.append(data)
            print(kernel, name, len(data), hashlib.sha256(data).hexdigest(), flush=True)
        assert outputs[0] == outputs[1], f'{kernel}: audio/state differs'
        print('PASS:', kernel, 'bit-identical output', flush=True)
