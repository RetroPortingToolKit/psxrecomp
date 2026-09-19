#!/usr/bin/env python3
"""Compile the actual launcher adapter and check its mounted CUE track map."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root / 'src/main.cpp').read_text(encoding='utf-8')
    start = text.index('static std::filesystem::path normalize_disc_path_for_launch(')
    end = text.index('\n}\n', start) + 2
    adapter = text[start:end]
    with tempfile.TemporaryDirectory(prefix='launch disc path ') as tmp:
        work = Path(tmp)
        source = work / 'probe.cpp'
        source.write_text('''#include <filesystem>
#include <string>
#include <cctype>
#include <iostream>
#include "disc_path.h"
#include "cue_sheet.h"
static std::string uppercase_ascii(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
''' + adapter + '''
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const auto mount = normalize_disc_path_for_launch(argv[i]);
        const auto cue = PSXRecompV4::parse_cue_sheet(mount);
        std::cout << mount.filename().generic_string() << "|" << cue.tracks.size();
        for (const auto& track : cue.tracks)
            std::cout << ":" << track.number << "," << track.is_audio << "," << track.index01;
        std::cout << "\\n";
    }
}
''', encoding='utf-8')
        program = work / ('probe.exe' if os.name == 'nt' else 'probe')
        env = os.environ.copy()
        compiler = shutil.which(args.compiler) or args.compiler
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        subprocess.run([compiler, '-std=c++17', '-O2', '-I' + str(root/'include'), str(source),
                        str(root/'src/disc_path.cpp'), str(root/'src/cue_sheet.cpp'), '-o', str(program)],
                       check=True, env=env)
        for stem, tracks in [('disc', 12), ('album', 8)]:
            (work/(stem+'.bin')).write_bytes(bytes(2352*40))
            cue = 'FILE "'+stem+'.bin" BINARY\n'
            for n in range(1, tracks+1):
                kind = 'MODE2/2352' if stem == 'disc' and n == 1 else 'AUDIO'
                cue += f' TRACK {n:02d} {kind}\n INDEX 01 00:00:{n*2:02d}\n'
            (work/(stem+'.cue')).write_text(cue)
        (work/'plain.bin').write_bytes(bytes(2352*4))
        (work/'portable.chd').write_bytes(b'synthetic placeholder; parser must not read CHD')
        (work/'broken.cue').write_text('FILE "missing.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
        (work/'broken.bin').write_bytes(bytes(2352*4))
        cases = [('disc.cue','disc.cue|12'), ('disc.bin','disc.cue|12'),
                 ('album.cue','album.cue|8'), ('album.bin','album.cue|8'),
                 ('plain.bin','plain.bin|0'), ('portable.chd','portable.chd|0'),
                 ('broken.cue','broken.bin|0'), ('missing.cue','missing.cue|0')]
        result = subprocess.run([str(program)] + [str(work/name) for name,_ in cases],
                                check=True, env=env, text=True, capture_output=True)
        got = result.stdout.splitlines()
        wanted = []
        for name, value in cases:
            if name.startswith(('disc.', 'album.')):
                count = 12 if name.startswith('disc.') else 8
                value += ''.join(f':{n},{int(name.startswith("album.") or n != 1)},{n*2}'
                                 for n in range(1, count+1))
            wanted.append(value)
        assert got == wanted, (got, wanted)
        print('launch adapter: 8 cases passed; mixed and audio-only CUE track maps retained')


if __name__ == '__main__':
    main()
