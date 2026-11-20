from pathlib import Path
import json, struct, collections, hashlib
import numpy as np
from .cmb import Model, U, H, S, F, put, chunk, align
from .packing import repack_gar, literals, validate_materials
from .j3d import J3D
from .textures import decode, encode_etc1a4
from .binary import decompress, entries

SCALE = 21.0
HEIGHT = 6555.463137 / 3986.693367


def rotation(a, b):
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = np.dot(a, b)
    if c > 0.999999:
        return np.eye(3)
    if c < -0.999999:
        axis = np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0])
        axis /= np.linalg.norm(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def desc(start=0, typ=0x1406, constant=None):
    return struct.pack(
        "<IfHH4f",
        start,
        1.0,
        typ,
        1 if constant is not None else 0,
        *(constant or [0, 0, 0, 0])
    )


def build(source, TP, dest, master=False):
    TP = Path(TP)
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    MAP = [
        1,
        9,
        10,
        11,
        11,
        22,
        14,
        15,
        16,
        17,
        17,
        18,
        19,
        20,
        21,
        21,
        2,
        3,
        3,
        4,
        5,
        5,
        6,
        6,
        7,
        8,
        8,
        2,
        3,
        2,
        6,
        2,
        3,
        2,
        6,
    ]
    texture_meta = []
    gar = decompress(source)
    m = Model(dict((n, p) for n, _, p in entries(gar))["child/model/link_child.cmb"])
    body = J3D(TP / "al.bmd")
    head = J3D(TP / "al_head.bmd")
    face = J3D(TP / "al_face.bmd")
    hands = J3D(TP / "al_hands.bmd")
    transforms = []
    for i, bone in enumerate(MAP):
        src = body.world[i]
        dst = m.world[bone]
        orient = 16 if bone == 17 else 20 if bone == 21 else bone
        R = rotation(src[:3, 0], m.world[orient][:3, 0])
        t = np.eye(4)
        t[:3, :3] = R * SCALE
        t[:3, 3] = dst[:3, 3] - t[:3, :3] @ src[:3, 3]
        transforms.append(t)
    # Attachments and secondary bones share their parent deformation in the prototype.
    transforms[3] = transforms[4]
    for i in range(27, 35):
        transforms[i] = transforms[16]
        MAP[i] = 2
    for i, parent in [(10, 9), (15, 14), (21, 20), (26, 25)]:
        transforms[i] = transforms[parent]
        MAP[i] = MAP[parent]
    # Preserve the original flat boot soles instead of aligning their toe axis.
    for source_foot, native_foot, source_knee, native_knee in [
        (20, 5, 19, 4),
        (25, 8, 24, 7),
    ]:
        src = body.world[source_foot]
        dst = m.world[native_foot]
        t = np.eye(4)
        t[:3, :3] *= SCALE
        t[:3, 3] = dst[:3, 3] - src[:3, 3] * SCALE
        t[1, 3] = 0.0
        transforms[source_foot] = t
        toe = source_foot + 1
        transforms[toe] = t
        MAP[toe] = native_foot
        src_knee = body.world[source_knee][:3, 3]
        dst_knee = m.world[native_knee][:3, 3]
        src_delta = src[:3, 3] - src_knee
        dst_delta = (t @ np.r_[src[:3, 3], 1])[:3] - dst_knee
        r = rotation(src_delta, dst_delta)
        axis = src_delta / np.linalg.norm(src_delta)
        stretch = np.eye(3) + (
            np.linalg.norm(dst_delta) / (np.linalg.norm(src_delta) * SCALE) - 1
        ) * np.outer(axis, axis)
        k = np.eye(4)
        k[:3, :3] = r @ stretch * SCALE
        k[:3, 3] = dst_knee - k[:3, :3] @ src_knee
        transforms[source_knee] = k

    buffers = [bytearray(x) for x in m.buffers]
    indices = bytearray(m.chunks["indices"])
    materials = list(m.materials)
    tex = list(m.tex)
    texdata = bytearray(m.chunks["textures"])
    shapes = list(m.shapes)
    templates = {me[3]: me for me in m.meshes}
    basehand = {}
    removed = set()
    for group in sorted(templates):
        pts = []
        ids = []
        bones = set()
        for i, me in enumerate(m.meshes):
            if me[3] != group or me[2] not in (2, 3):
                continue
            bs = {b for _, _, _, bb, _ in m.primitives(m.shapes[H(me, 0)]) for b in bb}
            if not (bs <= {16, 17} or bs <= {20, 21}):
                continue
            pts.extend(m.world_geometry(H(me, 0))[0])
            ids.append(i)
            bones |= bs
        if pts:
            basehand[group] = (
                np.unique(np.array(pts), axis=0),
                "left" if bones <= {16, 17} else "right",
            )
            removed.update(ids)
    sword_groups = {5, 6, 7, 12, 13, 14, 15, 16, 17}
    meshes = [
        me
        for i, me in enumerate(m.meshes)
        if me[3] not in range(28, 34)
        and i not in removed
        and not (master and me[3] in sword_groups and me[2] not in (2, 3))
    ]
    for si in sorted({H(me, 0) for me in meshes}):
        pos, _, _, _, _, pr = m.geometry(si)
        assert all(mode in (0, 1) for _, _, mode, _, _ in pr)
        sh = bytearray(shapes[si])
        at = align(buffers[0])
        buffers[0].extend((pos / HEIGHT).astype("<f4").tobytes())
        sh[0x24:0x40] = desc(at)
        shapes[si] = sh
    texmap = {}
    matmap = {}
    preview = []
    summaries = []
    candidates = []

    def converted(model, geo, part):
        verts = []
        for v in geo["vertices"]:
            weights = collections.defaultdict(float)
            p = np.zeros(3)
            n = np.zeros(3)
            for bone, w in v["weights"]:
                if w <= 0:
                    continue
                bind = model.invbind[bone] if v["smooth"] else np.eye(4)
                if part == "body":
                    T = transforms[bone] @ model.world[bone]
                    dst = MAP[bone]
                elif part in ("head", "face"):
                    T = transforms[4] @ body.world[4]
                    if part == "head" and bone >= 6:
                        pivot = head.world[6][:3, 3]
                        angle = np.deg2rad(-50)
                        c, s = np.cos(angle), np.sin(angle)
                        bend = np.eye(4)
                        bend[:3, :3] = [[c, -s, 0], [s, c, 0], [0, 0, 1]]
                        bend[:3, 3] = pivot - bend[:3, :3] @ pivot
                        T = T @ bend
                    T = T @ model.world[bone]
                    dst = 11
                else:
                    left = bone == 1
                    srcbone = 9 if left else 14
                    dst = 17 if left else 21
                    T = transforms[srcbone] @ body.world[srcbone] @ model.world[bone]
                M = T @ bind
                p += (M @ np.r_[v["pos"], 1])[:3] * w
                n += np.linalg.inv(M[:3, :3]).T @ v["normal"] * w
                weights[dst] += w
            ws = sorted(weights.items(), key=lambda v: -v[1])[:3]
            total = sum(w for _, w in ws)
            assert total > 0
            ws = [(b, w / total) for b, w in ws]
            n /= max(np.linalg.norm(n), 1e-12)
            assert np.all(np.isfinite(p)) and np.all(np.isfinite(n))
            verts.append({"pos": p, "normal": n, "uv": v["uv"], "weights": ws})
        return verts

    def material(model, mi):
        key = (model.path.name, mi)
        if key in matmap:
            return matmap[key]
        mat = model.materials[mi]
        ti = mat["textures"][0]
        tk = (model.path.name, ti)
        if tk not in texmap:
            t = model.textures[ti]
            image = decode(t)
            raw = encode_etc1a4(image[::-1].copy())
            from .hashes import dolphin_hash

            file = "texture-sources/" + hashlib.sha256(raw).hexdigest() + ".bin"
            target = dest / file
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(raw)
            texture_meta.append(
                dict(
                    file=file,
                    width=t["width"],
                    height=t["height"],
                    format=13,
                    source_hash=dolphin_hash(t),
                )
            )
            out = bytearray(m.tex[2])
            put(out, 0, len(raw))
            put(out, 4, 1)
            put(out, 6, 1, "H")
            put(out, 8, image.shape[1], "H")
            put(out, 10, image.shape[0], "H")
            put(out, 12, 0x675B)
            put(out, 16, align(texdata, 128))
            out[20:36] = ("tp_" + str(len(tex))).encode().ljust(16, b"\0")
            texdata.extend(raw)
            texmap[tk] = len(tex)
            tex.append(out)
        ma = bytearray(m.materials[2])
        put(ma, 16, texmap[tk], "h")
        # Native single-texture skin shading and alpha testing, with independent hashes.
        put(ma, 0x168, 0x54500000 + len(materials))
        ma[0x130] = 1
        ma[0x131] = 8
        put(ma, 0x132, 0x204, "H")
        matmap[key] = len(materials)
        materials.append(ma)
        return matmap[key]

    def append_shape(model, geo, verts, groups):
        # Split packets at the native ten-bone palette limit.
        batches = []
        tris = []
        palette = set()
        for tri in geo["triangles"]:
            bs = {b for vi in tri for b, w in verts[vi]["weights"] if w > 0}
            if tris and len(palette | bs) > 10:
                batches.append(tris)
                tris = []
                palette = set()
            tris.append(tri)
            palette |= bs
        if tris:
            batches.append(tris)
        mi = material(model, geo["material"])
        for tris in batches:
            oldids = sorted({i for tri in tris for i in tri})
            vmap = {v: i for i, v in enumerate(oldids)}
            vv = [verts[i] for i in oldids]
            bones = sorted({b for v in vv for b, w in v["weights"] if w > 0})
            pmap = {b: i for i, b in enumerate(bones)}
            rigid = True
            if rigid:
                vv = [
                    {
                        **v,
                        "pos": (
                            np.linalg.inv(m.world[v["weights"][0][0]])
                            @ np.r_[v["pos"], 1]
                        )[:3],
                        "normal": np.linalg.inv(m.world[v["weights"][0][0]][:3, :3])
                        @ v["normal"],
                    }
                    for v in vv
                ]
            rows = {
                0: [v["pos"] for v in vv],
                1: [v["normal"] for v in vv],
                4: [v["uv"] for v in vv],
                7: [
                    [pmap[b] for b, w in v["weights"]] + [0] * (3 - len(v["weights"]))
                    for v in vv
                ],
                8: [
                    [w for b, w in v["weights"]] + [0] * (3 - len(v["weights"]))
                    for v in vv
                ],
            }
            sh = bytearray(m.shapes[82][:0x124])
            put(sh, 8, 1, "H")
            put(sh, 10, H(sh, 10) & ~12, "H")
            points = np.array(rows[0])
            sh[12:24] = struct.pack("<3f", *points.mean(0))
            put(sh, 0x120, 1 if rigid else 3, "H")
            if rigid:
                put(sh, 10, 0x93, "H")
                rows[7] = [[pmap[v["weights"][0][0]]] for v in vv]
                rows[8] = [[1.0] for v in vv]
            for attr in range(9):
                if attr in rows:
                    at = align(buffers[attr])
                    typ = 0x1401 if attr in (7, 8) else 0x1406
                    dt = "u1" if attr in (7, 8) else "<f4"
                    values = np.array(rows[attr])
                    values = np.rint(values * 100) if attr == 8 else values
                    buffers[attr].extend(values.astype(dt).tobytes())
                    sh[0x24 + attr * 28 : 0x40 + attr * 28] = desc(at, typ)
                    put(sh, 0x24 + attr * 28 + 4, 0.01 if attr == 8 else 1.0, "f")
                else:
                    sh[0x24 + attr * 28 : 0x40 + attr * 28] = desc(
                        constant=[1, 1, 1, 1] if attr == 3 else [0, 0, 0, 0]
                    )
            sh.extend(struct.pack("<H", 0x128))
            align(sh)
            bs = bytearray(struct.pack("<" + "H" * len(bones), *bones))
            align(bs)
            prm = bytearray(m.primitives(m.shapes[82])[0][1][-24:])
            assert prm[:4] == b"prm "
            start = align(indices, 2) // 2
            flat = [vmap[i] for tri in tris for i in (tri[0], tri[2], tri[1])]
            assert start < 65536 and len(flat) < 65536
            indices.extend(np.array(flat, dtype="<u2").tobytes())
            put(prm, 16, 0x1403, "H")
            put(prm, 20, len(flat), "H")
            put(prm, 22, start, "H")
            pr = bytearray(b"prms" + bytes(20))
            put(pr, 8, 1)
            put(pr, 12, 1 if rigid else 2, "H")
            put(pr, 14, len(bones), "H")
            put(pr, 16, 24)
            put(pr, 20, 24 + len(bs))
            pr.extend(bs)
            pr.extend(prm)
            put(pr, 4, len(pr))
            sh.extend(pr)
            put(sh, 4, len(sh))
            si = len(shapes)
            shapes.append(sh)
            for group in groups:
                mesh = bytearray(templates[group])
                put(mesh, 0, si, "H")
                mesh[2] = mi
                meshes.append(mesh)
        summaries.append(
            {
                "part": model.path.name,
                "material": model.materials[geo["material"]]["name"],
                "triangles": len(geo["triangles"]),
                "batches": len(batches),
                "groups": groups,
            }
        )
        preview.append((verts, geo["triangles"], model, geo["material"], groups))

    for model, part in [
        (body, "body"),
        (head, "head"),
        (face, "face"),
        (hands, "hands"),
    ]:
        for geo in model.geometry:
            verts = converted(model, geo, part)
            for v in verts:
                v["weights"] = [(max(v["weights"], key=lambda item: item[1])[0], 1.0)]
            name = model.materials[geo["material"]]["name"]
            if part == "hands" or (
                part == "body" and name in ("al_handLA_m", "al_handRA_m")
            ):
                left = "handL" in name
                candidates.append((model, geo, verts, "left" if left else "right"))
                continue
            append_shape(model, geo, verts, [28, 29] if part == "body" else [33])
    hand_plan = []
    for group, (pts, side) in basehand.items():

        def score(c):
            cp = np.array([v["pos"] for v in c[2]])
            dist = ((pts[:, None, :] - cp[None, :, :]) ** 2).sum(2)
            return dist.min(0).mean() + dist.min(1).mean()

        best = min((c for c in candidates if c[3] == side), key=score)
        model, geo, verts, _ = best
        append_shape(model, geo, verts, [group])
        hand_plan.append(
            {
                "group": group,
                "side": side,
                "source_material": model.materials[geo["material"]]["name"],
                "fit_rms": float(np.sqrt(score(best) / 2)),
            }
        )
    if master:
        sword = J3D(TP.parents[1] / "bmwe/al_swm.bmd")
        sheath = J3D(TP.parents[1] / "bmwe/al_podm.bmd")
        rx = np.array([[1.0, 0, 0], [0, 0, -1.0], [0, 1.0, 0]])
        axis = np.array([0.99652664, 0.05886157, 0.05890653])
        held = rotation(np.array([1.0, 0, 0]), axis) @ rx
        weapon_scale = SCALE / HEIGHT

        def equipment(model, geo, bone, orient, offset):
            verts = []
            local = np.eye(4)
            local[:3, :3] = orient * weapon_scale
            local[:3, 3] = np.array(offset) / HEIGHT
            for v in geo["vertices"]:
                M = m.world[bone] @ local @ model.world[v["weights"][0][0]]
                pos = (M @ np.r_[v["pos"], 1])[:3]
                norm = np.linalg.inv(M[:3, :3]).T @ v["normal"]
                norm /= max(np.linalg.norm(norm), 1e-12)
                verts.append(
                    {
                        "pos": pos,
                        "normal": norm,
                        "uv": v["uv"],
                        "weights": [(bone, 1.0)],
                    }
                )
            return verts

        for geo in sword.geometry:
            append_shape(
                sword, geo, equipment(sword, geo, 17, held, [0, 290, -30]), [12, 14, 16]
            )
            if sword.materials[geo["material"]]["name"] == "al_SWgripM_m":
                append_shape(
                    sword, geo, equipment(sword, geo, 22, rx, [-350, 0, -45]), [5, 6, 7]
                )
        for geo in sheath.geometry:
            append_shape(
                sheath,
                geo,
                equipment(sheath, geo, 22, rx, [0, 0, -45]),
                [5, 6, 7, 13, 15, 17],
            )
    transparent = {(H(me, 0), me[2]) for me in m.meshes[H(m.mesh_header, 12) :]}
    meshes.sort(key=lambda me: ((H(me, 0), me[2]) in transparent, me[2]))
    msh = bytearray(m.mesh_header)
    put(msh, 8, len(meshes))
    put(msh, 12, sum((H(me, 0), me[2]) not in transparent for me in meshes), "H")
    msh.extend(b"".join(meshes))
    put(msh, 4, len(msh))
    shp = bytearray(m.shape_header)
    put(shp, 8, len(shapes))
    shp.extend(bytes(len(shapes) * 2))
    align(shp)
    for i, sh in enumerate(shapes):
        assert len(shp) < 65536
        put(shp, 16 + i * 2, len(shp), "H")
        shp.extend(sh)
        align(shp)
    put(shp, 4, len(shp))
    sklm = chunk(b"sklm", struct.pack("<II", 16, 16 + len(msh)) + msh + shp)
    vatr = bytearray(b"vatr" + bytes(80))
    put(vatr, 8, len(buffers[0]) // 12)
    for i, b in enumerate(buffers):
        align(vatr)
        put(vatr, 12 + i * 8, len(b))
        put(vatr, 16 + i * 8, len(vatr))
        vatr.extend(b)
    put(vatr, 4, len(vatr))
    mats = bytearray(
        chunk(
            b"mats",
            struct.pack("<I", len(materials)) + b"".join(materials) + m.combiners,
        )
    )
    put(mats, 4, 12 + len(materials) * (0x16C + 0x24))
    chunks = {
        **m.chunks,
        "sklm": sklm,
        "vatr": vatr,
        "mats": mats,
        "tex": chunk(b"tex ", struct.pack("<I", len(tex)) + b"".join(tex)),
        "indices": indices,
        "textures": texdata,
    }
    out = bytearray(m.data[:0x4C])
    put(out, 0x20, len(indices) // 2)
    for i, key in enumerate(
        ["skl", "qtrs", "mats", "tex", "sklm", "luts", "vatr", "indices", "textures"]
    ):
        align(out, 128 if key == "textures" else 4)
        put(out, 0x24 + i * 4, len(out))
        out.extend(chunks[key])
    put(out, 4, len(out))
    test = Model(out)
    validate_materials(test)
    assert test.chunks["skl"] == m.chunks["skl"]
    for si in range(len(test.shapes)):
        p, n, uv, bi, bw, pr = test.geometry(si)
        assert np.all(np.isfinite(p))
        for _, _, mode, bones, idx in pr:
            assert len(bones) <= 10 and np.max(idx) < len(p)
    packed = literals(repack_gar(gar, "child/model/link_child.cmb", out), source)
    assert len(decompress(packed)) > 0
    (dest / "zelda2_link_child_new.gar.lzs").write_bytes(packed)
    (dest / "link_child.cmb").write_bytes(out)
    report = {
        "master_sword": master,
        "cap_bend_degrees": 50,
        "prototype": True,
        "actor_bytes": len(packed),
        "model_bytes": len(out),
        "new_shapes": len(shapes) - len(m.shapes),
        "shape_table_bytes": len(shp),
        "index_entries": len(indices) // 2,
        "scale": SCALE,
        "hands": hand_plan,
        "parts": summaries,
    }
    (dest / "conversion.json").write_text(json.dumps(report, indent=2))
    return packed, texture_meta
