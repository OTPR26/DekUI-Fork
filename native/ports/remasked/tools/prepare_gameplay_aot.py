#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add guarded ReMasked operand boundaries to a private MM3D AOT product.

Preserves the original code image, every original function and instruction,
zero residual execution, and the original public product contract. Outputs
are private, contain owner-generated title code, and must never be uploaded.
"""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT),str(ROOT/'tools/oot3d/native_a32_runtime')]
from tools.triaevum_release.whole_aot_source_backend import load_default_frontend
from whole_aot_cpp import generate

def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def document(path,data):path.write_text(json.dumps(data,indent=2)+'\n')
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True);p.add_argument('--baseline-aot',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();inputs=a.inputs.resolve();previous=a.baseline_aot.resolve();out=a.output.resolve()
    if out.is_relative_to(ROOT) or out==previous or out.is_relative_to(previous) or out==inputs or out.is_relative_to(inputs):p.error('Use a separate private output outside the checkout and baseline')
    out.mkdir(parents=True,exist_ok=True,mode=0o700)
    code=inputs/'code.bin';exheader=inputs/'exheader.bin';front=load_default_frontend('mm3d')
    contract=json.loads(front.product_manifest.read_text());baseline=json.loads((previous/'ir-receipt.json').read_text());old_program=Path(baseline['program'])
    if digest(code)!=contract['source_image']['code']['sha256'] or code.stat().st_size!=contract['source_image']['code']['bytes']:p.error('Wrong owned MM3D code image')
    if len(exheader.read_bytes())!=2048:p.error('Wrong ExHeader size')
    # Native relocations are covered by the exact function guards/fixtures.
    sites=[0x1f2c4c,0x4aab64,0x1828d8,0x182938,0x182954,0x18296c,0x1fbcd4,0x1fbcf0,0x1fbd08,0x1e5188,0x448500,0x44850c,0x448894,0x4488ac,0x448c98,0x449154,0x448cc8,0x448cf0,0x3e460c,0x3e49c0]
    boundaries=set(sites)|{pc+4 for pc in sites};boundaries_path=out/'gameplay-boundaries.txt'
    boundaries_path.write_text(''.join(f'0x{pc:08X}\n' for pc in sorted(boundaries)))
    supplemental=out/'supplemental.csv';inventory=out/'inventory.csv';program=out/'aot_program.json';selection=out/'selection.json';manifest=out/'local-product-contract.json'
    subprocess.run([sys.executable,str(ROOT/'titles/mm3d/a32/guest_patch_entries.py'),'--inventory',str(front.inventory),'--merge',str(front.supplemental_entries),'--boundaries',str(boundaries_path),'--output',str(supplemental)],check=True)
    front.add_local_entry_intervals(front.inventory,supplemental,inventory,contract['source_image']['entrypoint'])
    print('Extracting private AOT with ReMasked boundaries',flush=True)
    result=front.extract_program(['--code',str(code),'--inventory',str(inventory),'--boundary-audit',str(front.boundary_audit),'--base',hex(front.default_base),'--executable-size',hex(contract['source_image']['executable_size']),*[x for flag,path in front.extraction_inputs for x in (flag,str(path))],'--output',str(program)])
    if result:raise RuntimeError('Structural extraction failed')
    # Every previous function stays, and only the requested operand boundaries
    # may become additional functions. Instruction coverage cannot change.
    old=json.loads(old_program.read_text());new=json.loads(program.read_text())
    old_entries={f['entry'] for f in old['functions']};new_entries={f['entry'] for f in new['functions']}
    if not old_entries<=new_entries or not (new_entries-old_entries)<=boundaries:raise RuntimeError('Unexpected function inventory change')
    if old['counts']['instructions']!=new['counts']['instructions']:raise RuntimeError('Original instruction coverage changed')
    del old,new
    subprocess.run([sys.executable,str(ROOT/'tools/oot3d/native_a32_runtime/select_profile_whole_aot.py'),'--program',str(program),'--profile',str(ROOT/'titles/mm3d/a32/empty_profile.json'),'--selection',str(front.selection),'--all-lowerable','--code',str(code),'--output',str(selection)],check=True)
    contract['source_image']['exheader']['sha256']=digest(exheader)
    contract['frontend']['inventory_sha256']=digest(inventory)
    contract['selection']['path']=str(selection);contract['selection']['sha256']=digest(selection)
    contract['notes']='Private ReMasked operand-boundary extension. Same exact code image and complete original instruction/function coverage; only the 40 verified operand/continuation PCs may add entries. No public contract overwritten.'
    document(manifest,contract)
    preliminary=front.audit_product(manifest,program,code_path=code,exheader_path=exheader,selection_path=selection)
    count_changes={'function count mismatch','selected count mismatch','block count mismatch','dispatch alias count mismatch','dispatcher entry count mismatch','emitted instruction count mismatch'}
    if set(preliminary.errors)-count_changes:raise RuntimeError('AOT safety audit: '+'; '.join(preliminary.errors))
    if not preliminary.summary:raise RuntimeError('No structural audit summary')
    for key in contract['closure']['counts']:
        if key in preliminary.summary:contract['closure']['counts'][key]=preliminary.summary[key]
    contract["closure"]["counts"]["dispatch_aliases"]=len(json.loads(selection.read_text()).get("selection_conflicts",[]))
    document(manifest,contract)
    audit=front.audit_product(manifest,program,code_path=code,exheader_path=exheader,selection_path=selection)
    if not audit.ok:raise RuntimeError('Strict extended contract failed: '+'; '.join(audit.errors))
    title=out/'title';print('Generating private title C++',flush=True)
    generated=generate(program,selection,code,title,shard_count=256,shard_strategy='affinity')
    audit=front.audit_product(manifest,program,code_path=code,exheader_path=exheader,selection_path=selection,generated_manifest_path=title/'whole_aot_cpp_manifest.json')
    if not audit.ok:raise RuntimeError('Generated title audit failed: '+'; '.join(audit.errors))
    entries=set()
    for f in generated['functions']:entries.add(f['entry']);entries.update(f['dispatch_entries']);entries.update(f['resume_entries'])
    if not boundaries<=entries:raise RuntimeError('Generated module omits a required callback/continuation')
    document(title/'TITLE_SOURCE_MANIFEST.json',{'format':'triaevum_translated_title_source_v1','title':'mm3d','code_sha256':digest(code),'exheader_sha256':digest(exheader),'local_only':True,'files':generated['files']})
    document(out/'receipt.json',{'local_only':True,'baseline_program_sha256':digest(old_program),'program_sha256':digest(program),'required_boundaries':sorted(boundaries),'only_verified_boundaries_added':True,'original_instruction_coverage_unchanged':True,'audit':audit.summary})
    print('Private ReMasked AOT generated and audited; nothing uploaded.',flush=True)
if __name__=='__main__':main()
