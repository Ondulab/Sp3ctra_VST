#!/usr/bin/env python3
"""Test the real timbre renderer and shared browser against the built JUCE library.
Usage: python3 vst/tests/timbre/run.py [build-directory] [artifact-directory]
Defaults: vst/build, /tmp/sp3ctra-timbres. No audio device or session is opened.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[3]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'vst/build'
flags = (build / 'CMakeFiles/Sp3ctraVST.dir/flags.make').read_text().splitlines()
def options(key):
    return shlex.split(next(line.split(' = ', 1)[1] for line in flags if line.startswith(key + ' = ')))
common = options('CXX_DEFINES') + options('CXX_INCLUDES') + ['-I' + str(root / 'vst/source/image'), '-std=c++17', '-O2', '-UNDEBUG']
link = shlex.split((build / 'CMakeFiles/Sp3ctraVST_Standalone.dir/link.txt').read_text())
tail = link[link.index('Sp3ctraVST_artefacts/Release/libSp3ctra_SharedCode.a'):]
with tempfile.TemporaryDirectory(prefix='sp3ctra-timbre-test-') as temp:
    objects=[]
    for name, source in [('library', Path(__file__).with_name('library.cpp')),
                         ('noise', Path(__file__).with_name('noise.cpp')),
                         ('renderer', Path(os.environ.get('TIMBRE_RENDERER_SOURCE', root / 'vst/source/image/TimbreGenRenderer.cpp')))]:
        obj=Path(temp)/(name+'.o'); objects.append(str(obj))
        subprocess.run(['clang++', *common, '-c', str(source), '-o', str(obj)], check=True)
    executable=Path(temp)/'timbre-test'
    subprocess.run(['clang++', *objects, '-o', str(executable), *tail], cwd=build, check=True)
    subprocess.run([str(executable), *sys.argv[2:]], check=True)
