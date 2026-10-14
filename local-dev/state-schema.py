#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Generate the reviewed fixed-width schema in keeperfx-dev (libclang-18).

Unknown pointer and scalar types fail closed. Legacy gameplay unions are frozen
subrecords: every alias is checked for pointers and gets layout assertions.
The generated source is checked in; libclang is not a runtime dependency.
"""
import argparse
import hashlib
import json
from pathlib import Path
from clang import cindex as c

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--check', action='store_true')
parser.add_argument('--deps-dir', type=Path, default=Path('/opt/keeperfx-build/RelWithDebInfo/deps'),
                    help='Prepared SDL dependency directory (archives include i686 headers).')
options = parser.parse_args()
c.Config.set_library_file('/usr/lib/llvm-18/lib/libclang.so.1')
args = ['-x', 'c', '-std=c11', '--target=i686-w64-windows-gnu',
        '-resource-dir=/usr/lib/llvm-18/lib/clang/18',
        '-I' + str(repo / 'src'), '-I/usr/i686-w64-mingw32/include']
for package, prefix in [('sdl3', 'SDL3'), ('sdl3_mixer', 'SDL3_mixer')]:
    includes = sorted(options.deps_dir.glob(f'{package}/{prefix}-*/i686-w64-mingw32/include'))
    if not includes:
        raise SystemExit(f'Missing {package} headers in {options.deps_dir}; prepare dependencies first.')
    args += ['-I' + str(includes[0])]
source = ''.join(f'#include "{h}"\n' for h in ['game_legacy.h', 'game_saves.h', 'replay.h', 'state_navigation.h'])
tu = c.Index.create().parse(str(repo / 'local-dev/state-schema-input.c'), args=args,
                          unsaved_files=[(str(repo / 'local-dev/state-schema-input.c'), source)])
errors = [str(d) for d in tu.diagnostics if d.severity >= c.Diagnostic.Error]
if errors:
    raise SystemExit('\n'.join(errors))
root_names = ['Game', 'IntralevelData', 'CatalogueEntry', 'PacketSaveHead', 'ContinueData', 'NavigationSnapshot']
roots = {n.spelling: n for n in tu.cursor.walk_preorder()
         if n._kind_id == c.CursorKind.STRUCT_DECL.value and n.is_definition()
         and n.spelling in root_names}
records, manifest = {}, []
signed = {c.TypeKind.CHAR_S, c.TypeKind.SCHAR, c.TypeKind.SHORT, c.TypeKind.INT, c.TypeKind.LONG, c.TypeKind.LONGLONG}
unsigned = {c.TypeKind.BOOL, c.TypeKind.CHAR_U, c.TypeKind.UCHAR, c.TypeKind.USHORT, c.TypeKind.UINT, c.TypeKind.ULONG, c.TypeKind.ULONGLONG}

def anonymous(f):
    # clang marks a *named field of an unnamed type* anonymous too; that field
    # still needs its name. Only implicit promoted fields omit the accessor.
    return not f.spelling or f.spelling.startswith(('union ', 'struct '))

def member(path, f):
    return path if anonymous(f) else path + '.' + f.spelling

def scalar(t):
    if t.kind == c.TypeKind.ENUM:
        return scalar(t.get_declaration().enum_type.get_canonical())
    if t.kind in signed | unsigned:
        return t.get_size(), t.kind in signed
    if t.kind in {c.TypeKind.FLOAT, c.TypeKind.DOUBLE}:
        return t.get_size(), False
    raise ValueError('Unsupported scalar: ' + t.spelling)

def opaque_layout(t, suffix, alias, checks, base_offset):
    t = t.get_canonical()
    expr = f'std::declval<{alias}&>()' + suffix
    if t.kind == c.TypeKind.CONSTANTARRAY:
        checks.append(f'static_assert(sizeof({expr}) == {t.get_size()});')
        opaque_layout(t.element_type, suffix + '[0]', alias, checks, base_offset)
    elif t.kind == c.TypeKind.RECORD:
        # A promoted anonymous union cannot be named with sizeof, but all its
        # actual member types, widths and offsets are checked recursively.
        if not t.get_declaration().is_anonymous():
            checks.append(f'static_assert(sizeof({expr}) == {t.get_size()});')
        for f in t.get_fields():
            if f.is_bitfield():
                # Coordinate aliases are the sole bitfields in legacy unions.
                if f.spelling not in {'pos', 'num'}:
                    raise ValueError('Unreviewed union bitfield: ' + f.spelling)
                manifest.append([suffix, f.spelling, 'union-bitfield', f.get_bitfield_width(), f.get_field_offsetof()])
                continue
            path = member(suffix, f)
            offset = base_offset + f.get_field_offsetof() // 8
            if not anonymous(f):
                checks.append(f'static_assert(offsetof({alias}, {path[1:]}) == {offset});')
            opaque_layout(f.type, path, alias, checks, offset)
    else:
        scalar(t)  # rejects pointers, long double, and other unknown types
        checks.append(f'static_assert(sizeof({expr}) == {t.get_size()});')

def register(t, expression):
    n = t.get_declaration()
    key = (str(n.location.file), n.location.line, n.location.column)
    if key in records:
        return records[key]
    rec = {'id': len(records), 'expr': expression, 'lines': [], 'checks': [], 'size': 0}
    records[key] = rec
    rec['size'] = emit(t, 'v', expression, rec, top=True)
    return rec

def emit(t, path, expression, rec, top=False, offset=0):
    t = t.get_canonical()
    out = rec['lines']
    if t.kind == c.TypeKind.CONSTANTARRAY:
        count, base = 1, t
        while base.kind == c.TypeKind.CONSTANTARRAY:
            count *= base.element_count
            base = base.element_type.get_canonical()
        if base.kind not in {c.TypeKind.RECORD, c.TypeKind.POINTER, c.TypeKind.LONGDOUBLE}:
            width, sign = scalar(base)
            out.append(f'c.scalars(&{path}, sizeof({path}) / {count}, {count}, {width}, {str(sign).lower()});')
            manifest.append([expression, 'array', count, width, sign])
            return count * width
        loop = 'i' + str(path.count('['))
        manifest.append([expression, 'record-array', t.element_count])
        out.append(f'for (size_t {loop} = 0; {loop} < {t.element_count}; ++{loop}) {{')
        size = emit(t.element_type, path + f'[{loop}]', expression + '[0]', rec)
        out.append('}')
        return size * t.element_count
    if t.kind == c.TypeKind.RECORD:
        fs = list(t.get_fields())
        if t.get_declaration().kind == c.CursorKind.UNION_DECL:
            if {f.spelling for f in fs} == {'val', 'stl'}:
                value = next(f for f in fs if f.spelling == 'val')
                parts = list(next(f for f in fs if f.spelling == 'stl').type.get_fields())
                if t.get_size() != 4 or scalar(value.type.get_canonical()) != (4, True) or [
                    (f.spelling, f.get_bitfield_width(), f.get_field_offsetof()) for f in parts
                ] != [('pos', 8, 0), ('num', 24, 8)]:
                    raise ValueError('Unreviewed coordinate union: ' + expression)
                return emit(next(f.type for f in fs if f.spelling == 'val'), path + '.val', expression + '.val', rec)
            # Freeze pointer-free legacy variants without guessing an active tag.
            out.append(f'c.bytes(&{member(path, fs[0])}, {t.get_size()});')
            opaque_layout(t, path[1:], f'StateType{rec["id"]}', rec['checks'], offset)
            manifest.append([expression, 'frozen-union', t.get_size()])
            return t.get_size()
        if not top:
            child = register(t, expression)
            manifest.append([expression, 'record', child['id']])
            out.append(f'visit{child["id"]}(c, {path});')
            return child['size']
        total = 0
        for f in fs:
            if f.is_bitfield():
                raise ValueError('Unexpected bitfield: ' + expression + '.' + f.spelling)
            total += emit(f.type, member(path, f), member(expression, f), rec,
                          offset=offset + f.get_field_offsetof() // 8)
        return total
    if t.kind == c.TypeKind.POINTER:
        if t.spelling == 'struct Dungeon *' and expression == 'std::declval<Game&>().computer[0].dungeon':
            out.append(f'c.dungeon(&{path});')
            manifest.append([expression, 'dungeon-index', 4])
            return 4
        if t.spelling == 'char *' and expression in {
            'std::declval<Game&>().sound_settings.sound_data_path',
            'std::declval<Game&>().sound_settings.music_data_path',
            'std::declval<Game&>().sound_settings.dir3',
        }:
            manifest.append([expression, 'local-path-omitted'])
            return 0
        raise ValueError('Unreviewed pointer: ' + expression + ' ' + t.spelling)
    if t.kind == c.TypeKind.LONGDOUBLE:
        if expression == 'std::declval<Game&>().process_turn_time':
            manifest.append([expression, 'local-clock-omitted'])
            return 0
        raise ValueError('Unreviewed long double: ' + expression)
    width, sign = scalar(t)
    out.append(f'c.scalars(&{path}, sizeof({path}), 1, {width}, {str(sign).lower()});')
    manifest.append([expression, 'scalar', width, sign])
    return width

root_records = [register(roots[name].type, f'std::declval<{name}&>()') for name in root_names]
lines = ['// SPDX-License-Identifier: GPL-2.0-or-later', '// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.', '// Generated by local-dev/state-schema.py. Do not edit by hand.', '']
for rec in records.values():
    lines.append(f'using StateType{rec["id"]} = std::remove_reference_t<decltype({rec["expr"]})>;')
    lines.extend(dict.fromkeys(rec['checks']))
    lines.append(f'static void visit{rec["id"]}(Codec& c, StateType{rec["id"]}& v);')
for rec in records.values():
    lines += [f'static void visit{rec["id"]}(Codec& c, StateType{rec["id"]}& v)', '{']
    lines.extend('    ' + line for line in rec['lines'])
    lines += ['}', '']
canonical = json.dumps(manifest, separators=(',', ':')) + '\n'.join(line for line in lines if line.startswith('static_assert'))
fingerprint = int.from_bytes(hashlib.sha256(canonical.encode()).digest()[:8], 'little')
lines.append(f'static constexpr uint64_t schemaFingerprint = UINT64_C(0x{fingerprint:016x});')
lines.append('static const Schema schemas[] = {')
lines.append('    {},')
for rec, name in zip(root_records, root_names):
    lines.append(f'    {{{rec["size"]}, sizeof({name}), [](Codec& c, void* p) {{ visit{rec["id"]}(c, *static_cast<{name}*>(p)); }}}},')
lines += ['};', '']
output = '\n'.join(lines)
target = repo / 'src/state_schema.inc'
if options.check:
    if not target.exists() or target.read_text() != output:
        raise SystemExit('State schema is stale. Regenerate and review the format/version change.')
else:
    target.write_text(output)
print(json.dumps({'records': len(records), 'fingerprint': f'{fingerprint:016x}', 'payload_bytes': dict(zip(root_names, [r['size'] for r in root_records]))}, indent=2))
