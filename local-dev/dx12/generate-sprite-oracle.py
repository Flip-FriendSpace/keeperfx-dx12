# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Generate two explicitly corrected diagnostic routines from original C source.

The original renderer is linked separately and remains unchanged. This reference
changes only the mirrored solid/remap row duplication's extra background pixel.
"""
import argparse
import pathlib
import re


def corrected_function(path, name):
    source = path.read_text(encoding="utf-8")
    signature = "TbResult " + name + "("
    if source.count(signature) != 1:
        raise RuntimeError(f"Expected exactly one definition of {name}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if end >= len(source):
            raise RuntimeError(f"Unbalanced function {name}")
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    function = source[start:end]
    # Match the exact two statements responsible for the replicated stray
    # background pixel. Fail the build if upstream changes their structure.
    pattern = r"out_start = out_end;\n(?P<indent> +)solid_len\+\+;"
    if len(re.findall(pattern, function)) != 1:
        raise RuntimeError(f"Expected one known row-copy defect in {name}")
    function = re.sub(pattern, "out_start = out_end + 1;", function)
    return function.replace(signature, "TbResult Corrected" + name + "(", 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    directory = args.repo / "src/kfx/renderer/software"
    normal = directory / "bflib_vidraw_spr_norm.c"
    remap = directory / "bflib_vidraw_spr_remp.c"
    # Retain the original source copyright/license comment in generated output.
    license_comment = normal.read_text(encoding="utf-8").split('#include "pre_inc.h"')[0]
    output = license_comment + '''
// BUILD-GENERATED DIAGNOSTIC REFERENCE. The original renderer is linked too.
// Only the mirrored solid/remap duplicated-row off-by-one is corrected here.
#include "bflib_vidraw.h"
#include <stdlib.h>
void LbPixelBlockCopyForward(TbPixel* dst, const TbPixel* src, long len);
'''
    output += corrected_function(normal, "LbSpriteDrawUsingScalingUpDataSolidRL") + "\n"
    output += corrected_function(remap, "LbSpriteDrawRemapUsingScalingUpDataSolidRL") + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")


if __name__ == "__main__":
    main()
