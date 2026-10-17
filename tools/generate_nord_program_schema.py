"""Developer-only generator for ns4decode 2.0's MIT-licensed field maps.

Usage: python3 tools/generate_nord_program_schema.py /path/to/ns4decode_pkg
The app uses the generated C++ tables and does not need Python.
"""
import json
import ast
from pathlib import Path
import sys

sys.path.insert(0, sys.argv[1])
import ns4maps
import ns4names

params, groups = ns4maps.getMaps('', 0)
names, sample_names = ns4names.getNames('', 0)
lines = ['// Generated from ns4decode 2.0, Copyright (c) 2024 Randy, MIT License.',
         '// See THIRD_PARTY_NORD_PROGRAM_LICENSE.txt. Do not edit by hand.',
         '#pragma once', '#include <cstdint>', 'namespace nord_program_schema {',
         'struct Field { const char* group; const char* name; int first[3]; int last[3]; };',
         'inline constexpr Field fields[] {']
for group, keys in groups.items():
    for key in keys:
        p = params[key]
        first = [p[i]['begloc'] if i < p['numlayers'] else -1 for i in range(3)]
        last = [p[i]['endloc'] if i < p['numlayers'] else -1 for i in range(3)]
        lines.append('    {' + json.dumps(group) + ', ' + json.dumps(p['name']) + ', {'
                     + ','.join(map(str, first)) + '}, {' + ','.join(map(str, last)) + '}},')
lines += ['};', 'struct Piano { std::uint32_t id; const char* name; };', 'inline constexpr Piano pianos[] {']
for key, name in names.items():
    lines.append('    {' + str(int(key)) + 'u, ' + json.dumps(name) + '},')
lines += ['};', 'inline constexpr Piano samples[] {']
for key, name in sample_names.items():
    lines.append('    {' + str(int(key)) + 'u, ' + json.dumps(name) + '},')
lines += ['};']
tree = ast.parse((Path(sys.argv[1]) / 'ns4decode.py').read_text())
for function, table in [('interpretAttack','attackMs'), ('interpretRelease','releaseMs'), ('interpretFilterFreq','filterHz')]:
    fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == function)
    lists = [ast.literal_eval(n) for n in ast.walk(fn) if isinstance(n, ast.List)]
    values = [float(v) for v in lists[0]] + [1000 * float(v) for v in lists[1]]
    assert len(values) == 128
    lines += ['inline constexpr float ' + table + '[] {', '    ' + ','.join(str(v)+'f' for v in values), '};']
lines += ['}']
root = Path(__file__).resolve().parent.parent
(root / 'Source' / 'NordProgramSchema.h').write_text('\n'.join(lines) + '\n')
print('Generated fields:', len(params), 'piano identities:', len(names))
