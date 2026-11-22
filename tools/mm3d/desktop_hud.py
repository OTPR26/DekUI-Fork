"""Install the Android fork's authored HUD from guarded, ROM-derived copy operations."""
import base64
import hashlib
import json
import struct
from pathlib import Path
from restore_action_flips import entries, u16, u32
from upgrade_game_resources import atomic_write, checked

ARCHIVE = 'load/mods/0004000000125500/romfs/layout/Joker.Main.New.gar'
CONFIG = 'config/mm3d-local/presentation.json'

def digest(data):
    return hashlib.sha256(data).hexdigest()

def reconstruct(source, spec):
    if digest(source) == spec['sha256']:
        return source
    for variant in spec.get('source_variants', []):
        if digest(source) == variant['sha256']:
            normalized = bytearray(source)
            for op in variant['normalize']:
                at = op['at']
                before = base64.b64decode(op['before'], validate=True)
                after = base64.b64decode(op['after'], validate=True)
                if (len(before) != len(after) or at < 0 or at > len(source) - len(before)
                        or source[at:at+len(before)] != before):
                    raise ValueError('HUD source normalization mismatch')
                normalized[at:at+len(after)] = after
            source = bytes(normalized)
            break
    if digest(source) != spec['source_sha256']:
        raise ValueError('HUD archive has custom changes; retained existing HUD')
    output = bytearray()
    for op in spec['ops']:
        if 'copy' in op:
            at, size = op['copy'], op['size']
            if at < 0 or size < 0 or at + size > len(source):
                raise ValueError('Invalid HUD copy bounds')
            output.extend(source[at:at+size])
        else:
            output.extend(base64.b64decode(op['data'], validate=True))
        if len(output) > spec['size']:
            raise ValueError('HUD output exceeds expected size')
    if len(output) != spec['size'] or digest(output) != spec['sha256']:
        raise ValueError('HUD output checksum mismatch')
    return bytes(output)


def sword_ring_texture(archive, texture_index):
    if not archive.startswith(b'GAR\x02'):
        return archive
    _, address, layout = entries(archive)['BActionIcon.mfl']
    if layout[:4] != b'MFL ' or u32(layout, 4) != 4:
        raise ValueError('Unsupported sword button layout')
    objects, panes = u16(layout, 10), u16(layout, 14)
    geometry, animations, strings = struct.unpack_from('<III', layout, 20)
    if geometry != 32 + objects * 68 or animations != geometry + panes * 48:
        raise ValueError('Unexpected sword button geometry')
    names = layout[strings:].split(b'\0')[:1 + panes]
    pane = names.index(b'a_button_base_pane') - 1
    at = geometry + pane * 48
    # This pane draws the 38×38 B background independently of the sword.
    # Keep its coordinates and palette, and select the same neutral atlas as iOS.
    if (pane < 0 or u32(layout, at) != 0x002c0006 or
            struct.unpack_from('<8f', layout, at + 4) !=
            (-19.0, -19.0, 0.0, 38.0, 38.0, 48/512, 0.0, 38/512) or
            u32(layout, at + 40) not in (0x000f0003, 0x002b0003)):
        raise ValueError('Unexpected sword button artwork')
    output = bytearray(archive)
    struct.pack_into('<H', output, u32(archive, address) + at + 42, texture_index)
    return bytes(output)


def apply(user, resources):
    spec = json.loads((resources/'desktop-hud/layout.json').read_bytes())
    atlas_spec = json.loads((resources/'desktop-hud/upgrade.json').read_bytes())
    texture = (resources/'desktop-hud'/atlas_spec['texture']).read_bytes()
    if digest(texture) != atlas_spec['texture_sha256']:
        raise ValueError('Android HUD atlas checksum mismatch')
    archive = checked(user, ARCHIVE)
    source = archive.read_bytes()
    try:
        updated = reconstruct(source, spec)
    except ValueError as error:
        if digest(source) != spec['source_sha256'] and digest(source) != spec['sha256']:
            print(str(error))
            return  # Never replace another user's authored archive.
        raise
    play_spec = json.loads((resources/'desktop-hud/play.json').read_bytes())
    play = checked(user, 'load/mods/0004000000125500/romfs/layout/Joker.Main.Play.gar')
    play_original = play.read_bytes()
    play_baseline = play_original
    if digest(play_baseline) not in (play_spec['sha256'], play_spec['source_sha256']):
        play_baseline = sword_ring_texture(play_baseline, 15)
    play_updated = sword_ring_texture(reconstruct(play_baseline, play_spec), 43)
    config = checked(user, CONFIG)
    raw = config.read_bytes()
    data = json.loads(raw)
    keys = spec['presentation']
    if any(all(data.get(k) == old.get(k) for k in keys) for old in spec['previous']):
        data.update(keys)
    new_config = (json.dumps(data, indent=2)+'\n').encode()
    backup = 'config/dekui-backups/desktop-hud-'+digest(source)[:12]
    if source != updated:
        saved = checked(user, backup+'.gar')
        if not saved.exists(): atomic_write(saved, source)
    if data != json.loads(raw):
        saved = checked(user, backup+'.json')
        if not saved.exists(): atomic_write(saved, raw)
    # Every byte is checked before publishing; a retry safely completes any partial update.
    texture_target = checked(user, 'load/mm3d-model-textures/0004000000125500/hud/'+atlas_spec['texture'])
    if not texture_target.exists() or digest(texture_target.read_bytes()) != digest(texture):
        atomic_write(texture_target, texture)
    pack = checked(user, 'load/mm3d-model-textures/0004000000125500/pack.json')
    if not pack.exists():
        atomic_write(pack, b'{"options":{"skip_mipmap":true,"flip_png_files":true,"use_new_hash":true}}')
    if play_original != play_updated:
        saved = checked(user, 'config/dekui-backups/desktop-play-'+digest(play_original)[:12]+'.gar')
        if not saved.exists(): atomic_write(saved, play_original)
        atomic_write(play, play_updated)
    if source != updated: atomic_write(archive, updated)
    if data != json.loads(raw): atomic_write(config, new_config)


def apply_controls(user, resources):
    spec = json.loads((resources/'desktop-controls/provenance.json').read_bytes())
    replacement = (resources/'desktop-controls/code.bps').read_bytes()
    if digest(replacement) != spec['patch_sha256']:
        raise ValueError('Desktop gameplay input patch checksum mismatch')
    target = checked(user, 'load/mods/0004000000125500/code.bps')
    original = target.read_bytes()
    if digest(original) == spec['patch_sha256']:
        return
    if digest(original) not in (spec['baseline_patch_sha256'], spec.get('legacy_patch_sha256')):
        raise ValueError('Custom gameplay patch retained; cannot apply the default input patch')
    backup = checked(user, 'config/dekui-backups/code-before-desktop-pc-input.bps')
    if not backup.exists(): atomic_write(backup, original)
    atomic_write(target, replacement)
