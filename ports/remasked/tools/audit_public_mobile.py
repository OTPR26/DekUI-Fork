#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail closed on private content in public APK/IPA candidates.

This supplements (does not replace) source provenance, signing, clean-install,
original-game-content and runtime qualification checks. No private fixtures.
"""
import argparse
import hashlib
import json
import re
import stat
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

HOME = re.compile(rb'(?:/(?:Users|home)/[A-Za-z0-9_. -]+/|[A-Za-z]:\\Users\\[A-Za-z0-9_. -]+\\)')
TOKEN = re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b')
KEY = re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=\r\n]{40,64000}-----END (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')
GAME = re.compile(r'\.(?:3ds|cci|cia|z64|n64|v64|gar(?:\.lzs)?|zar|bch|cmb|zobj|pak|bcsar|bcstm|bclim|ctxb|bcres|mfpj)$',re.I)
PRIVATE_DIRS = {'ownercontent','privategame','preparedmodels','initialsaves','henrikomobile','texture-overrides'}
PRIVATE_NAMES = {'code.bin','exheader.bin','romfs.bin','save0.bin','save1.bin','save2.bin'}


def audit(path):
    issues=[]; count=0; expanded=0
    def fail(member,kind):
        finding={'member':member,'kind':kind}
        if finding not in issues:issues.append(finding)
    def archive(source,prefix='',depth=0):
        nonlocal count,expanded
        if depth>4:fail(prefix,'nested_archive_depth');return
        try:
            with zipfile.ZipFile(source) as z:
                seen=set()
                for item in z.infolist():
                    name=item.filename; member=prefix+name; p=PurePosixPath(name)
                    if name in seen:fail(member,'duplicate_member')
                    seen.add(name)
                    if p.is_absolute() or '..' in p.parts or '\\' in name or '\x00' in name or (p.parts and ':' in p.parts[0]):fail(member,'unsafe_member');continue
                    if stat.S_ISLNK(item.external_attr>>16):fail(member,'symbolic_link');continue
                    if item.is_dir():continue
                    count+=1;expanded+=item.file_size
                    if count>100000 or expanded>4*1024**3 or item.file_size>1024**3:
                        fail(member,'archive_size_limit');return
                    lower=name.lower();parts={s.lower() for s in p.parts}
                    if PRIVATE_DIRS & parts or p.name.lower() in PRIVATE_NAMES or GAME.search(lower):fail(member,'game_or_private_payload')
                    if lower.endswith(('.mobileprovision','.p12','.pfx','.key','.pem','.keystore','.jks')):fail(member,'signing_or_secret_file')
                    with z.open(item) as stream, tempfile.SpooledTemporaryFile(max_size=8*1024**2) as nested:
                        previous=b'';is_zip=False;first=True
                        while True:
                            chunk=stream.read(1024*1024)
                            if not chunk:break
                            if first:is_zip=chunk.startswith((b'PK\x03\x04',b'PK\x05\x06'));first=False
                            if is_zip:nested.write(chunk)
                            block=previous+chunk
                            for candidate in (block,block[::2],block[1::2]):
                                if HOME.search(candidate):fail(member,'embedded_home_path')
                            if TOKEN.search(block) or KEY.search(block):fail(member,'embedded_secret')
                            previous=block[-65536:]
                        if is_zip:
                            nested.seek(0);archive(nested,member+'!',depth+1)
        except (OSError,ValueError,zipfile.BadZipFile,RuntimeError,NotImplementedError):fail(prefix or Path(path).name,'unreadable_archive')
    archive(path)
    with Path(path).open('rb') as stream:sha=hashlib.file_digest(stream,'sha256').hexdigest()
    return {'asset':Path(path).name,'sha256':sha,'files_checked':count,'expanded_bytes':expanded,'issues':issues,'status':'FAIL' if issues else 'PASS'}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('package',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args()
    report=audit(args.package);args.report.write_text(json.dumps(report,indent=2)+'\n');print(report['asset'],report['status'],'files',report['files_checked'],'findings',len(report['issues']))
    raise SystemExit(report['status']!='PASS')
if __name__=='__main__':main()
