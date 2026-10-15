# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression fixture for redirected controls and finished custom-buffer bindings."""
from pathlib import Path
import os,shlex,subprocess,tempfile
src=(Path(__file__).resolve().parents[2]/"src/bflib_sndlib.cpp").read_text()
def function(marker):
 start=src.index(marker);brace=src.index("{",start);depth=0
 for i in range(brace,len(src)):
  if src[i]=="{":depth+=1
  elif src[i]=="}":
   depth-=1
   if depth==0:return src[start:i+1]
 raise AssertionError(marker)
preamble=r"""#include <cassert>
#include <cstdio>
#include <vector>
#include <unordered_map>
#include <stdexcept>
using SoundSmplTblID=int;using SoundEmitterID=int;using SoundVolume=int;using SoundPan=int;using SoundPitch=int;
using ALuint=unsigned;using ALint=int;using ALenum=int;
constexpr int AL_BUFFER=1,AL_NO_ERROR=0,bb_king_mode=1;
static int errors=0;
#define ERRORLOG(...) (++errors)
struct openal_error:std::runtime_error {openal_error(const char*s,int):std::runtime_error(s){}};
struct Buffer {ALuint id;};struct Sample {Buffer buffer;};
struct Source {
 ALuint id=0;int smptbl_id=0,mss_id=0,emit_id=0,flags=0,base_gain=0;
 int volume=-1,panning=-1,frequency=-1;bool stopped=false;
 void gain(int x){volume=x;}void pan(int x){panning=x;}void pitch(int x){frequency=x;}void stop(){stopped=true;}
};
static std::vector<Source> g_sources;
static std::vector<Sample> g_custom_bank;
static std::unordered_map<int,int> g_id_redirects,bindings;
static int g_speech_offset=1068;
void alGetSourcei(ALuint id,int,ALint*x){*x=bindings[id];}
int alGetError(){return 0;}
void alSourcei(ALuint id,int,int value){bindings[id]=value;}
"""
markers=["static void detach_custom_sound_sources(","static SoundSmplTblID resolve_sample_id_redirect(",
'extern "C" void SetSampleVolume(','extern "C" void SetSamplePan(','extern "C" void SetSamplePitch(','extern "C" void stop_sample(']
body="\n\n".join(function(x) for x in markers)
tests=r"""
int main() {
 g_id_redirects={{72,1200},{1100,1201},{-1,1202}};
 assert(resolve_sample_id_redirect(72)==1200);
 assert(resolve_sample_id_redirect(73)==73);
 assert(resolve_sample_id_redirect(1100)==1100);
 assert(resolve_sample_id_redirect(-1)==-1);
 assert(resolve_sample_id_redirect(1200)==1200);
 g_sources.resize(3);g_sources[0].id=1;g_sources[0].emit_id=7;g_sources[0].smptbl_id=1200;g_sources[0].mss_id=1;
 g_sources[1].id=2;g_sources[1].emit_id=8;g_sources[1].smptbl_id=73;g_sources[1].mss_id=2;
 g_sources[2].id=3;g_sources[2].emit_id=9;g_sources[2].smptbl_id=74;g_sources[2].mss_id=3;
 for(int value:{0,64,127}) {SetSampleVolume(7,72,value);assert(g_sources[0].volume==value);assert(g_sources[1].volume==-1);}
 for(int value:{0,64,128}) {SetSamplePan(7,72,value);assert(g_sources[0].panning==value);}
 for(int value:{50,100,150}) {SetSamplePitch(7,72,value);assert(g_sources[0].frequency==value);}
 SetSampleVolume(8,73,33);assert(g_sources[1].volume==33);
 SetSamplePan(7,1200,21);assert(g_sources[0].panning==21);
 SetSamplePitch(8,73,80);assert(g_sources[1].frequency==80);
 stop_sample(7,72);assert(g_sources[0].stopped);assert(g_sources[0].emit_id==0);assert(g_sources[0].smptbl_id==0);assert(g_sources[0].mss_id==1);
 // Finished sources retain real bindings even after their bookkeeping IDs clear.
 g_custom_bank={{{11}},{{12}}};bindings={{1,11},{2,12},{3,13}};
 g_sources[0].stopped=false;g_sources[0].smptbl_id=0;g_sources[0].emit_id=0;
 g_sources[1].stopped=false;g_sources[1].smptbl_id=0;g_sources[1].emit_id=0;
 detach_custom_sound_sources(1);
 assert(bindings[1]==11);assert(bindings[2]==0);assert(bindings[3]==13);
 assert(!g_sources[0].stopped);assert(g_sources[1].stopped);assert(g_sources[1].mss_id==2);
 detach_custom_sound_sources(0);
 assert(bindings[1]==0);assert(bindings[3]==13);assert(g_sources[0].mss_id==1);assert(g_sources[0].stopped);
 assert(!g_sources[2].stopped);assert(errors==0);
 std::puts("PASS: redirected controls, native/direct IDs, finished-source detachment and retained channel identities");
}
"""
with tempfile.TemporaryDirectory(prefix="keeperfx-audio-controls-") as directory:
    source=Path(directory)/"controls.cpp"
    binary=Path(directory)/"controls"
    source.write_text(preamble+body+tests)
    compiler=shlex.split(os.environ.get("CXX","c++"))
    subprocess.run(compiler+["-std=c++17","-Wall","-Wextra","-Werror",str(source),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
