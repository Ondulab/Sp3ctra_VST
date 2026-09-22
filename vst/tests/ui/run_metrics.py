#!/usr/bin/env python3
"""Footer integration test against a built production library (macOS Unix Makefiles).
Usage: python3 vst/tests/ui/run_metrics.py [build-directory] [output.png]
Renders the real footer component without loading a processor/session.
"""
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build-mac'
flags = (build / 'CMakeFiles/Sp3ctraVST.dir/flags.make').read_text().splitlines()

def options(key):
    return shlex.split(next(line.split(' = ', 1)[1]
                            for line in flags if line.startswith(key + ' = ')))

common = options('CXX_DEFINES') + options('CXX_INCLUDES') + [
    '-I' + str(root / 'vst/source'), '-std=c++17', '-O2', '-UNDEBUG']
link = shlex.split((build / 'CMakeFiles/Sp3ctraVST_Standalone.dir/link.txt').read_text())
tail = link[link.index('Sp3ctraVST_artefacts/Release/libSp3ctra_SharedCode.a'):]
with tempfile.TemporaryDirectory(prefix='sp3ctra-metrics-test-') as temp:
    obj, executable = Path(temp) / 'metrics.o', Path(temp) / 'metrics-test'
    subprocess.run(['clang++', *common, '-c', str(Path(__file__).with_name('pipeline_metrics_bar.cpp')),
                    '-o', str(obj)], check=True)
    subprocess.run(['clang++', str(obj), '-o', str(executable), *tail], cwd=build, check=True)
    subprocess.run([str(executable), *sys.argv[2:]], check=True)
