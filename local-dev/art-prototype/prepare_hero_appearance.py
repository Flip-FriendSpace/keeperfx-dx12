# SPDX-License-Identifier: GPL-2.0-or-later
"""Component-aware appearance direction references from generated identity art."""
from pathlib import Path
import argparse
from package_hero import primitives
ap=argparse.ArgumentParser();ap.add_argument('slug');ap.add_argument('sheet');args=ap.parse_args()
root=Path(__file__).resolve().parents[3]/'artifacts/art-prototype/heroes'/args.slug
cells=primitives(root)['split'](root/args.sheet,5,1)
for direction,image in enumerate(cells):image.save(root/f'appearance-td-d{direction}.png')
