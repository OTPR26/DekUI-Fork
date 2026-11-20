#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate private MM3D AOT sources from an already extracted, owned input.

Outputs must stay outside the source checkout. No network or device writes.
The USA metadata contract is distinct from the upstream EUR contract; sharing
code does not qualify regional content or gameplay compatibility.
"""
import argparse
import dataclasses
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.triaevum_release.whole_aot_source_backend import (
    SourceImage, load_default_frontend, prepare_whole_aot_ir,
)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    inputs, output = args.inputs.resolve(), args.output.resolve()
    if output.is_relative_to(ROOT) or inputs.is_relative_to(ROOT):
        parser.error('Private inputs and generated output must be outside the source checkout')
    upstream = load_default_frontend('mm3d')
    contract = json.loads(upstream.product_manifest.read_text())
    code, exheader = inputs / 'code.bin', inputs / 'exheader.bin'
    expected = contract['source_image']['code']
    if code.stat().st_size != expected['bytes'] or digest(code) != expected['sha256']:
        parser.error('MM3D code does not match the pinned AOT inventory')
    data = exheader.read_bytes()
    if len(data) != 2048:
        parser.error('Invalid ExHeader size')
    source = contract['source_image']
    if (int.from_bytes(data[0x10:0x14], 'little') != source['entrypoint'] or
            int.from_bytes(data[0x14:0x18], 'little') * 4096 != source['executable_size']):
        parser.error('ExHeader executable layout does not match the AOT inventory')
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    (output / '.gitignore').write_text('*\n')
    # Never alter the upstream EUR reference or pretend the USA ExHeader matches.
    contract['source_image']['exheader']['sha256'] = digest(exheader)
    contract['notes'] = 'Private local input contract; code/layout verified, regional content unqualified.'
    local_contract = output / 'local-product-contract.json'
    local_contract.write_text(json.dumps(contract, indent=2) + '\n')
    frontend = dataclasses.replace(upstream, product_manifest=local_contract,
        identity_files=(*upstream.identity_files, ('private-input-contract', local_contract)))
    result = prepare_whole_aot_ir(recipe='remasked-mm3d-local-v1',
        code=SourceImage(code, code.stat().st_size, digest(code)),
        exheader=SourceImage(exheader, len(data), digest(exheader)),
        cache_root=output / 'ir', frontend=frontend, title='mm3d')
    (output / 'ir-receipt.json').write_text(json.dumps(result, indent=2) + '\n')
    sys.path.insert(0, str(ROOT / 'tools/oot3d/native_a32_runtime'))
    from whole_aot_cpp import generate
    generated = output / 'title'
    manifest = generate(Path(result['program']), upstream.selection, code, generated,
        shard_count=256, shard_strategy='affinity')
    audit = frontend.audit_product(local_contract, Path(result['program']),
        code_path=code, exheader_path=exheader, selection_path=upstream.selection,
        generated_manifest_path=generated / 'whole_aot_cpp_manifest.json')
    if not audit.ok:
        raise RuntimeError('Generated AOT audit failed: ' + '; '.join(audit.errors))
    (output / 'generated-audit.json').write_text(json.dumps(audit.summary, indent=2) + '\n')
    inventory = {'format': 'triaevum_translated_title_source_v1', 'title': 'mm3d',
        'code_sha256': digest(code), 'exheader_sha256': digest(exheader),
        'local_only': True, 'files': manifest['files']}
    (generated / 'TITLE_SOURCE_MANIFEST.json').write_text(json.dumps(inventory, indent=2) + '\n')
    print(json.dumps({'generated_files': len(manifest['files']),
        'functions': len(manifest['functions']), 'local_only': True}))


if __name__ == '__main__':
    main()
