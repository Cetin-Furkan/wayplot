#!/usr/bin/env python3
"""
Khoros Experiment Deck Generator
Generates reproducible # khoros-run v1 experiment decks for arbitrary shapes and N counts.
"""

import math
import random
import os
import sys

def make_header(dt=1.0/60.0, gx=0.0, gy=-9.80665, gz=0.0, floor_y=-2.5, seed=1, ticks=600):
    return (
        f"# khoros-run v1\n"
        f"dt_s {dt:.8f}\n"
        f"gravity {gx:.5f} {gy:.5f} {gz:.5f}\n"
        f"floor_y {floor_y:.2f}\n"
        f"seed {seed}\n"
        f"ticks {ticks}\n"
        f"integrator symplectic_euler\n"
    )

def generate_sphere_n1():
    out = make_header(ticks=600)
    out += "# Single falling sphere (high altitude drop)\n"
    out += "body id=0 shape=sphere r=0.5 mass=2.0 e=0.85 mu=0.3 x=0.0 y=8.0 z=0.0 vx=0.0 vy=0.0 vz=0.0\n"
    return out

def generate_box_n1():
    out = make_header(ticks=600)
    out += "# Single falling AABB box\n"
    out += "body id=0 shape=aabb hx=0.5 hy=0.5 hz=0.5 mass=2.5 e=0.4 mu=0.6 x=0.0 y=6.0 z=0.0 vx=0.0 vy=0.0 vz=0.0\n"
    return out

def generate_spheres_n8():
    out = make_header(ticks=600)
    out += "# 8 Spheres dropping in a line with colliding lateral trajectories\n"
    for i in range(8):
        x = -3.5 + i * 1.0
        y = 4.0 + (i % 3) * 1.2
        z = 0.0
        vx = -1.5 if (i % 2 == 0) else 1.5
        vy = 0.0
        vz = 0.0
        out += f"body id={i} shape=sphere r=0.4 mass=1.5 e=0.8 mu=0.3 x={x:.2f} y={y:.2f} z={z:.2f} vx={vx:.2f} vy={vy:.2f} vz={vz:.2f}\n"
    return out

def generate_boxes_n8():
    out = make_header(ticks=600)
    out += "# 8 AABB boxes in a vertical stack drop\n"
    for i in range(8):
        x = 0.0
        y = -1.5 + i * 1.1
        z = 0.0
        out += f"body id={i} shape=aabb hx=0.45 hy=0.45 hz=0.45 mass=1.0 e=0.2 mu=0.7 x={x:.2f} y={y:.2f} z={z:.2f} vx=0.0 vy=0.0 vz=0.0\n"
    return out

def generate_mixed_n64():
    out = make_header(ticks=600)
    out += "# 64 Mixed Bodies (32 Spheres + 32 AABBs)\n"
    rng = random.Random(42)
    for i in range(64):
        x = rng.uniform(-6.0, 6.0)
        y = rng.uniform(1.0, 10.0)
        z = rng.uniform(-6.0, 6.0)
        vx = rng.uniform(-1.0, 1.0)
        vy = rng.uniform(-0.5, 0.5)
        vz = rng.uniform(-1.0, 1.0)
        if i % 2 == 0:
            r = rng.uniform(0.3, 0.5)
            m = rng.uniform(1.0, 2.0)
            out += f"body id={i} shape=sphere r={r:.3f} mass={m:.2f} e=0.75 mu=0.4 x={x:.2f} y={y:.2f} z={z:.2f} vx={vx:.2f} vy={vy:.2f} vz={vz:.2f}\n"
        else:
            hx = rng.uniform(0.25, 0.45)
            hy = rng.uniform(0.25, 0.45)
            hz = rng.uniform(0.25, 0.45)
            m = rng.uniform(1.0, 3.0)
            out += f"body id={i} shape=aabb hx={hx:.3f} hy={hy:.3f} hz={hz:.3f} mass={m:.2f} e=0.5 mu=0.6 x={x:.2f} y={y:.2f} z={z:.2f} vx={vx:.2f} vy={vy:.2f} vz={vz:.2f}\n"
    return out

def generate_spheres_n256():
    out = make_header(ticks=600)
    out += "# 256 Spheres Cloud\n"
    rng = random.Random(1337)
    for i in range(256):
        x = rng.uniform(-10.0, 10.0)
        y = rng.uniform(2.0, 16.0)
        z = rng.uniform(-10.0, 10.0)
        vx = rng.uniform(-1.5, 1.5)
        vy = rng.uniform(-1.0, 0.0)
        vz = rng.uniform(-1.5, 1.5)
        r = rng.uniform(0.25, 0.4)
        m = rng.uniform(0.8, 1.8)
        out += f"body id={i} shape=sphere r={r:.3f} mass={m:.2f} e=0.7 mu=0.4 x={x:.2f} y={y:.2f} z={z:.2f} vx={vx:.2f} vy={vy:.2f} vz={vz:.2f}\n"
    return out

def generate_massive_n1024():
    out = make_header(ticks=600)
    out += "# 1024 Rigid Bodies (Max Engine Capacity)\n"
    rng = random.Random(2026)
    for i in range(1023): # 1023 dynamic + 1 floor plane = 1024 total bodies
        x = rng.uniform(-14.0, 14.0)
        y = rng.uniform(1.0, 24.0)
        z = rng.uniform(-14.0, 14.0)
        vx = rng.uniform(-1.0, 1.0)
        vy = rng.uniform(-1.0, 0.0)
        vz = rng.uniform(-1.0, 1.0)
        if i % 3 == 0:
            hx = 0.3
            hy = 0.3
            hz = 0.3
            out += f"body id={i} shape=aabb hx={hx:.2f} hy={hy:.2f} hz={hz:.2f} mass=1.0 e=0.5 mu=0.5 x={x:.2f} y={y:.2f} z={z:.2f} vx={vx:.2f} vy={vy:.2f} vz={vz:.2f}\n"
        else:
            r = 0.3
            out += f"body id={i} shape=sphere r={r:.2f} mass=1.0 e=0.7 mu=0.4 x={x:.2f} y={y:.2f} z={z:.2f} vx={vx:.2f} vy={vy:.2f} vz={vz:.2f}\n"
    return out

def main():
    decks = {
        "sphere_n1.deck": generate_sphere_n1(),
        "box_n1.deck": generate_box_n1(),
        "spheres_n8.deck": generate_spheres_n8(),
        "boxes_n8.deck": generate_boxes_n8(),
        "mixed_n64.deck": generate_mixed_n64(),
        "spheres_n256.deck": generate_spheres_n256(),
        "massive_n1024.deck": generate_massive_n1024(),
    }
    target_dir = os.path.dirname(os.path.abspath(__file__))
    for fname, content in decks.items():
        p = os.path.join(target_dir, fname)
        with open(p, "w") as f:
            f.write(content)
        print(f"Generated: {p}")

if __name__ == "__main__":
    main()
