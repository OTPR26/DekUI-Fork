#!/usr/bin/env python3
"""Build guarded French/Spanish classic dialogue from the supported USA resources.
Only the owl and Song of Time entries get extra storage; native text is retained.
"""
from dialogue_recipe import recipe
import argparse
import hashlib
import json
import struct
from pathlib import Path
import single_screen_text as text
from double_prices import decode, encode

ROOT = Path(__file__).resolve().parents[2]
SPEC = {
    'usf': {
        'language': 4, 'choices': ('Oui', 'Non'),
        'pages': [
            'Créez une sauvegarde temporaire\net quittez le jeu.',
            'En la chargeant, vous retrouvez\nvotre lieu, votre heure et\nvotre inventaire.',
            '{Color:1}Sauvegardez régulièrement.{Color:11}\nLa sauvegarde temporaire est\neffacée lorsque vous reprenez.',
            'Sans nouvelle sauvegarde, vous\nreprenez à la dernière sauvegarde\npermanente du Chant du Temps.',
            '{Color:1}Sauvegarder et quitter le jeu?{Color:11}',
        ],
        'return': [
            'Votre sauvegarde temporaire\nest chargée. L’aventure continue.',
            'Sauvegardez de nouveau avant\nde quitter le jeu pour conserver\nvotre progression.',
        ],
        'lead': 'Passer à la',
        'destinations': ('nuit du premier jour', 'aube du deuxième jour',
                         'nuit du deuxième jour', 'aube du dernier jour', 'nuit du dernier jour'),
    },
    'uss': {
        'language': 8, 'choices': ('Sí', 'No'),
        'pages': [
            'Crea aquí una partida temporal\ny sal del juego.',
            'Al cargarla, recuperarás tu lugar,\nla hora y tu inventario.',
            '{Color:1}Sigue guardando mientras juegas.{Color:11}\nLa partida temporal se consume\ncuando retomas la aventura.',
            'Si no guardas de nuevo, volverás\na la última partida permanente\nde la Canción del Tiempo.',
            '{Color:1}¿Guardar y salir del juego?{Color:11}',
        ],
        'return': [
            'Tu partida temporal se ha cargado.\nLa aventura continúa aquí.',
            'Guarda de nuevo antes de salir\npara conservar tu progreso.',
        ],
        'lead': '¿Avanzar hasta el',
        'destinations': ('anochecer del primer día', 'amanecer del segundo día',
                         'anochecer del segundo día', 'amanecer del último día', 'anochecer del último día'),
    },
}

def entries(data):
    assert data[:8] == b'GMSG\x01\0\0\0'
    count, language = struct.unpack_from('<II', data, 8)
    assert count <= 20000 and language in (4, 8)
    result = {}
    for i in range(count):
        at = 16 + i*20
        mid = struct.unpack_from('<H', data, at)[0]
        offset, size = struct.unpack_from('<II', data, at+12)
        assert offset >= 16+count*20 and offset+size <= len(data) and offset%2 == 0
        result[mid] = (at, data[offset:offset+size])
    return result

def tokens(spec):
    pages = spec['pages']
    out = []
    for page in pages[:-1]:
        out += [(0x27,)] + text.markup(page) + [(0x28,), (0x31,1), (2,), (0x3a,11)]
    out += text.markup(pages[-1])
    out += [(1,), (0x3a,2), (0x2f,2), spec['choices'][0], (1,), spec['choices'][1], (0x3a,11), (0,)]
    return out

def build(source):
    rows = []
    assets = {}
    for name, spec in SPEC.items():
        data = (source/(name+'.gmsg')).read_bytes()
        assert struct.unpack_from('<I', data, 12)[0] == spec['language']
        table = entries(data)
        owl = table[0xc01][1]
        song = table[0x1b8a][1]
        for raw in (owl, song):
            native = decode(raw)
            assert (0x2f,2) in native
            assert spec['choices'][0] in native and spec['choices'][1] in native
        classic = encode(tokens(spec))
        returned = encode([(0x27,)] + text.markup(spec['return'][0]) +
                          [(0x28,), (0x31,1), (2,), (0x27,)] +
                          text.markup(spec['return'][1]) + [(0x28,), (0x31,0), (0,)])
        capacity = 1536
        assert len(classic) <= capacity and len(returned) <= capacity
        questions = []
        for dest in spec['destinations']:
            lead = "Passer à l'" if name == 'usf' and dest.startswith('aube') else spec['lead']
            question = encode([lead, (1,), (58,1), dest, (58,11), '?', (1,),
                               (58,2), (47,2), spec['choices'][0], (1,), spec['choices'][1], (0,)])
            assert len(question) <= 128
            questions.append(question.ljust(128,b'\0'))
        # Validate native metadata; reject unsupported game resources.
        assert struct.unpack_from('<I', data, table[0xc01][0]+8)[0] == 0xff0001
        assert struct.unpack_from('<I', data, table[0x1b8a][0]+4)[0] == 0x3fffffff
        out = bytearray(data)
        padded_owl = owl.ljust(capacity,b'\0')
        padded_song = song.ljust(128,b'\0')
        for mid, raw in [(0xc01,padded_owl),(0x1b8a,padded_song)]:
            if len(out)%2: out.append(0)
            struct.pack_into('<II',out,table[mid][0]+12,len(out),len(raw))
            out.extend(raw)
        after = entries(out)
        assert all(after[mid][1] == raw for mid, (_, raw) in table.items() if mid not in (0xc01,0x1b8a))
        for raw in [classic, returned, *questions]:
            assert encode(decode(raw)).rstrip(b'\0') == raw.rstrip(b'\0')
        rows.append((spec['language'],padded_owl,classic,returned,padded_song,questions))
        assets[name+'.gmsg'] = bytes(out)
    header = ('// SPDX-License-Identifier: GPL-2.0-or-later\n'
              '// Generated by tools/mm3d/classic_localized_text.py.\n#pragma once\n'
              '#include <array>\n#include "mm3d/dialogue_recipe.h"\n#include <cstdint>\n'
              'namespace MM3D::ClassicLocalizedText {\n'
              'struct Dialogue {unsigned language; DialogueText::Recipe original, classic, returned; '
              'std::uint64_t original_hash, classic_hash, return_hash; '
              'DialogueText::Recipe song; std::array<DialogueText::Recipe,5> questions;};\n'
              'inline constexpr std::array<Dialogue,2> Dialogues{{\n')
    for lang, owl, classic, returned, song, questions in rows:
        hashes = [text.fnv(raw.ljust(len(owl),b'\0')) for raw in (owl,classic,returned)]
        header += '    {'+str(lang)+','+','.join(recipe(owl,r) for r in (owl,classic,returned))+','
        header += ','.join(f'0x{h:016x}ULL' for h in hashes)+','+recipe(song)+',{{'
        header += ','.join(recipe(song,q) for q in questions)+'}}},\n'
    header += '}};\n}\n'
    return assets, header

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path,help='Original romfs/message/us directory')
    args = parser.parse_args()
    assets, header = build(args.source)
    root = ROOT/'resources/mm3d/project-restoration'
    manifest = json.loads((root/'manifest.json').read_text())
    for name, data in assets.items():
        rel = 'romfs/message/us/'+name
        (root/rel).write_bytes(data)
        manifest['sha256'][rel] = hashlib.sha256(data).hexdigest()
    (root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (ROOT/'src/mm3d/classic_localized_text.h').write_text(header)
    print('Generated French/Spanish classic dialogue and expanded native message storage.')
