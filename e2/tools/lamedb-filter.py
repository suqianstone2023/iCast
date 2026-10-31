#!/usr/bin/env python3
# Keep only real TV services (type 1, name not starting with "ad").
# Handles both lamedb (v3/v4) and lamedb5 formats.
import sys

for path in sys.argv[1:]:
    src = open(path).read().splitlines()
    out, removed = [], []
    if src and src[0].startswith('eDVB services /5/'):
        # lamedb5: t:... transponder lines, s:... service lines
        for line in src:
            if line.startswith('s:'):
                toks = line.split(':')
                stype = toks[5] if len(toks) > 5 else '0'
                if '"' in line:
                    name = line.split('"')[1]
                else:
                    name = ''
                if stype == '1' and not name.lower().startswith('ad'):
                    out.append(line)
                else:
                    removed.append((stype, name))
            else:
                out.append(line)
    else:
        i, n = 0, len(src)
        while i < n:
            if src[i] == 'services':
                out.append(src[i]); i += 1
                while i < n and src[i] != 'end':
                    key = src[i]
                    name = src[i + 1] if i + 1 < n else ''
                    opts = src[i + 2] if i + 2 < n else ''
                    f = key.split(':')
                    stype = int(f[4]) if len(f) > 4 and f[4].isdigit() else 0
                    if stype == 1 and not name.lower().startswith('ad'):
                        out += [key, name, opts]
                    else:
                        removed.append((stype, name))
                    i += 3
                if i < n:
                    out.append(src[i]); i += 1
            else:
                out.append(src[i]); i += 1
    open(path + '.filtered', 'w').write('\n'.join(out) + '\n')
    print('%s: removed %d' % (path, len(removed)))
