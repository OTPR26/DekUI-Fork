#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a synthetic __FILE__ probe with the release compiler prefix maps."""
import argparse,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(dir=a.work,prefix='prefix-map-') as folder:
 source=Path(folder)/'source';build=Path(folder)/'build';source.mkdir()
 (source/'main.cpp').write_text('#include <cstdio>\nint main(){puts(__FILE__);}\n')
 (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.26)\nproject(PrivacyProbe LANGUAGES CXX)\ninclude("'+str(root/'cmake/BuildPrivacy.cmake')+'")\nadd_executable(probe main.cpp)\n')
 subprocess.run(['cmake','-S',str(source),'-B',str(build)],check=True)
 subprocess.run(['cmake','--build',str(build)],check=True)
 output=subprocess.check_output([str(build/'probe')]).decode()
 # The synthetic fixture is under the runner's home; the host path must not leak.
 assert str(source) not in output and not output.startswith(('/Users/','/home/')),output
 assert b'/Users/' not in (build/'probe').read_bytes() and b'/home/' not in (build/'probe').read_bytes()
 print('Compiled source paths are neutral; no home path remains in the probe.')
