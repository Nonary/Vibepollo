"""Exercise production PulseAudio capture-sink selection with backend queries stubbed."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='c++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[4]
source = (root / 'src/platform/linux/audio.cpp').read_text()
start = source.index('        auto sink_name = requested_sink;', source.index('// Sink choice priority:'))
selection = source[start:source.index('\n        return ::platf::microphone(', start)]
program = r'''
#include <cassert>
#include <string>
#include <iostream>
namespace config { struct {std::string sink;} audio; }
int default_queries=0;
std::string get_default_sink_name(){++default_queries;return "physical-default";}
std::string select_sink(const std::string &requested_sink) {
@SELECTION@
 return sink_name;
}
int main(){
 config::audio.sink="configured-physical";
 assert(select_sink("managed-virtual")=="managed-virtual");
 assert(select_sink("resumed-virtual")=="resumed-virtual");
 assert(default_queries==0);
 assert(select_sink("")=="configured-physical");assert(default_queries==0);
 config::audio.sink.clear();
 assert(select_sink("managed-virtual")=="managed-virtual");assert(default_queries==0);
 assert(select_sink("")=="physical-default");assert(default_queries==1);
 std::cout<<"managed/resumed sink beats configured/default physical capture; fallback preserved\n";
}
'''.replace('@SELECTION@', selection)
with tempfile.TemporaryDirectory(prefix='managed-audio-') as directory:
    cpp=Path(directory)/'test.cpp'
    exe=Path(directory)/'test'
    cpp.write_text(program)
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
