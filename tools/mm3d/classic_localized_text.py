#!/usr/bin/env python3
"""Build guarded French/Spanish classic dialogue from the supported USA resources.
Only the owl and Song of Time entries get extra storage; native text is retained.
"""
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
            "Vous pouvez {Color:1}sauvegarder{Color:11} et\n{Color:1}quitter{Color:11} ici. Une icône lumineuse\napparaîtra près du nom de votre\nfichier quand vous le choisirez.",
            "En rouvrant ce fichier, vous\nreprendrez votre aventure à cet\n{Color:1}endroit{Color:11}, à cet {Color:1}instant{Color:11}, dans\nvotre {Color:1}état actuel{Color:11}.",
            "{Color:1}Attention:{Color:11} Après avoir repris,\nsi vous réinitialisez sans sauver\nà une statue de hibou, vous perdez\nla progression sauvegardée ici...",
            "Vous reprendrez au premier jour,\ndans l'état où vous étiez lors\nde votre dernière sauvegarde\navec le Chant du Temps.",
            "{Color:1}Sauvegarder{Color:11} votre progression\net {Color:1}quitter{Color:11} l'aventure?",
        ],
        'return': [
            "Vous qui portez l'épée sacrée...\nVous voici de retour\nau lieu convenu.",
            "Pour {Color:1}interrompre{Color:11} encore votre\naventure ({Color:1}jeu{Color:11}), {Color:1}parlez-moi{Color:11},\nà moi que vous avez marqué\nen preuve de notre rencontre.",
        ],
        'lead': 'Passer à la',
        'destinations': ('nuit du premier jour', 'aube du deuxième jour',
                         'nuit du deuxième jour', 'aube du dernier jour', 'nuit du dernier jour'),
    },
    'uss': {
        'language': 8, 'choices': ('Sí', 'No'),
        'pages': [
            "Puedes {Color:1}guardar{Color:11} tu progreso y\n{Color:1}salir{Color:11} aquí. Al elegir tu archivo,\nverás un icono brillante junto\na su nombre.",
            "La próxima vez que abras el\narchivo, volverás a este mismo\n{Color:1}lugar{Color:11} y {Color:1}momento{Color:11},\ncon tu {Color:1}estado actual{Color:11}.",
            "{Color:1}Atención:{Color:11} Si vuelves y reinicias\nsin guardar en una estatua de\nbúho, perderás el progreso\nque has guardado aquí...",
            "Al abrir el archivo de nuevo,\nvolverás al primer día, tal como\nestabas cuando guardaste con\nla Canción del Tiempo.",
            "¿Quieres {Color:1}guardar{Color:11} tu progreso\ny {Color:1}salir{Color:11} de la partida?",
        ],
        'return': [
            "Tú, que portas la espada\nsagrada... Has vuelto\nal lugar señalado.",
            "Si deseas {Color:1}interrumpir{Color:11} tu aventura\n({Color:1}juego{Color:11}) otra vez, {Color:1}habla conmigo{Color:11},\ncon quien marcaste como prueba\nde nuestro encuentro.",
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

def literal(raw):
    return '{"' + ''.join('\\%03o' % b for b in raw) + '",' + str(len(raw)) + '}'

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
              '#include <array>\n#include <string_view>\n#include <cstdint>\n'
              'namespace MM3D::ClassicLocalizedText {\n'
              'struct Dialogue {unsigned language; std::string_view original, classic, returned; '
              'std::uint64_t original_hash, classic_hash, return_hash; '
              'std::string_view song; std::array<std::string_view,5> questions;};\n'
              'inline constexpr std::array<Dialogue,2> Dialogues{{\n')
    for lang, owl, classic, returned, song, questions in rows:
        hashes = [text.fnv(raw.ljust(len(owl),b'\0')) for raw in (owl,classic,returned)]
        header += '    {'+str(lang)+','+','.join(literal(r) for r in (owl,classic,returned))+','
        header += ','.join(f'0x{h:016x}ULL' for h in hashes)+','+literal(song)+',{{'
        header += ','.join(literal(q) for q in questions)+'}}},\n'
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
