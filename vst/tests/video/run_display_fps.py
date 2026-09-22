#!/usr/bin/env python3
"""Replay real VideoScrollRenderCore history at 60 Hz and render at 10..60 FPS.
The APVTS owner is replaced with a small parameter map; capture ring, history,
warp and blit are production code. No audio backend/session is started.
Usage: python3 vst/tests/video/run_display_fps.py [build-directory]
"""
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[3]
build = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root/'build-mac'
flags = (build/'CMakeFiles/Sp3ctraVST.dir/flags.make').read_text().splitlines()
def options(key):
    return shlex.split(next(line.split(' = ',1)[1] for line in flags if line.startswith(key+' = ')))
common = options('CXX_DEFINES')+options('CXX_INCLUDES')+[
    '-I'+str(root/'vst/source/video'),'-std=c++17','-O3','-UNDEBUG']
link = shlex.split((build/'CMakeFiles/Sp3ctraVST_Standalone.dir/link.txt').read_text())
tail = link[link.index('Sp3ctraVST_artefacts/Release/libSp3ctra_SharedCode.a'):]
with tempfile.TemporaryDirectory(prefix='sp3ctra-display-fps-') as temp:
    out = Path(temp)
    (out/'Stub.h').write_text('''#include <juce_graphics/juce_graphics.h>
#include <map>
#include <atomic>
inline juce::String vsParam(int s,const char* p){return "videoScroll"+juce::String(s)+"_"+p;}
class Sp3ctraAudioProcessor {public:
std::map<juce::String,std::atomic<float>> params;
Sp3ctraAudioProcessor& getAPVTS(){return *this;}
std::atomic<float>* getRawParameterValue(const juce::String& id){auto i=params.find(id);return i==params.end()?nullptr:&i->second;}
};
''')
    (out/'Core.cpp').write_text((root/'vst/source/video/VideoScrollRenderCore.cpp').read_text().replace('#include "../PluginProcessor.h"','#include "Stub.h"'))
    (out/'display_fps.cpp').write_text(Path(__file__).with_name('display_fps.cpp').read_text())
    for name in ['Core','display_fps']:
        subprocess.run(['clang++',*common,'-c',str(out/(name+'.cpp')),'-o',str(out/(name+'.o'))],check=True)
    executable = out/'display-fps'
    subprocess.run(['clang++',str(out/'Core.o'),str(out/'display_fps.o'),'-o',str(executable),*tail],cwd=build,check=True)
    subprocess.run([str(executable)],check=True)
