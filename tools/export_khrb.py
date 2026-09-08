#!/usr/bin/env python3
"""Export Blender mesh(es) to a Khoros .khrb blob (self-relative packed mesh).

Does not run inside the engine. The engine only DMA-reads the file.

  blender scene.blend --background --python tools/export_khrb.py -- /tmp/out.khrb

Selected MESH objects, or every mesh in the scene if nothing is selected.
World transform applied. Blender Z-up becomes Y-up (x, z, -y).
Triangles only. Payload must fit the engine hugepage payload (~1984 KiB).
"""

from __future__ import annotations

import struct
import sys

KHRB_MAGIC = 0x4252484B
KHRB_VERSION = 1
MAX_VERTS = 262144
MAX_INDICES = 786432
# Matches KHR_HUGEPAGE_SZ - KHR_HP_UI_RESERVE
PAYLOAD_CAP = 2_097_152 - 65_536


def _argv_rest() -> list[str]:
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def export(path: str) -> None:
    import bpy

    sel = [o for o in bpy.context.selected_objects if o.type == "MESH"]
    objs = sel if sel else [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not objs:
        raise SystemExit("export_khrb: no mesh objects")

    deps = bpy.context.evaluated_depsgraph_get()
    xyz: list[float] = []
    idx: list[int] = []
    for ob in objs:
        ev = ob.evaluated_get(deps)
        mesh = ev.to_mesh()
        mesh.calc_loop_triangles()
        mw = ob.matrix_world
        base = len(xyz) // 3
        for v in mesh.vertices:
            c = mw @ v.co
            xyz.extend((float(c.x), float(c.z), float(-c.y)))
        for tri in mesh.loop_triangles:
            idx.extend((base + int(tri.vertices[0]),
                        base + int(tri.vertices[1]),
                        base + int(tri.vertices[2])))
        ev.to_mesh_clear()

    nv = len(xyz) // 3
    ni = len(idx)
    if nv == 0 or ni < 3 or ni % 3 != 0:
        raise SystemExit("export_khrb: no triangles")
    if nv > MAX_VERTS or ni > MAX_INDICES:
        raise SystemExit(f"export_khrb: too large ({nv} verts, {ni} idx)")
    need = 32 + nv * 12 + ni * 4
    if need > PAYLOAD_CAP:
        raise SystemExit(f"export_khrb: {need} B exceeds payload {PAYLOAD_CAP} B")

    verts_off = 16  # verts sit at byte 32; field is at 16
    indices_off = 32 + nv * 12 - 20
    header = struct.pack(
        "<4I2i2I",
        KHRB_MAGIC,
        KHRB_VERSION,
        nv,
        ni,
        verts_off,
        indices_off,
        0,
        0,
    )
    body = b"".join(struct.pack("<3f", xyz[i], xyz[i + 1], xyz[i + 2])
                    for i in range(0, len(xyz), 3))
    ibody = struct.pack("<" + "I" * ni, *idx)
    blob = header + body + ibody
    with open(path, "wb") as f:
        f.write(blob)
    print(f"export_khrb: wrote {path}  {nv} verts  {ni} idx  {len(blob)} B")


def main() -> None:
    rest = _argv_rest()
    if not rest:
        raise SystemExit(
            "usage:\n"
            "  blender --background --python tools/export_khrb.py -- out.khrb\n"
            "  blender --background --python tools/export_khrb.py -- --monkey out.khrb\n"
            "  blender scene.blend --background --python tools/export_khrb.py -- out.khrb\n"
            "(no .blend = Blender's default cube)"
        )
    if rest[0] == "--monkey":
        if len(rest) < 2:
            raise SystemExit("export_khrb: --monkey needs an output path")
        import bpy
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete()
        bpy.ops.mesh.primitive_monkey_add()
        export(rest[1])
        return
    export(rest[0])


if __name__ == "__main__":
    main()
