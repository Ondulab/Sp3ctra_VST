#!/usr/bin/env python3
"""macOS integration test against a built production library (Unix Makefiles).
Usage: python3 vst/tests/video/run_native.py [build-directory]
Opens a temporary native window; does not load the processor or a session.
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
with tempfile.TemporaryDirectory(prefix='sp3ctra-native-test-') as temp:
    obj, executable = Path(temp) / 'native.o', Path(temp) / 'native-test'
    subprocess.run(['clang++', *common, '-c', str(Path(__file__).with_name('native_presentation.mm')),
                    '-o', str(obj)], check=True)
    subprocess.run(['clang++', str(obj), '-o', str(executable), *tail], cwd=build, check=True)
    subprocess.run([str(executable)], check=True)
