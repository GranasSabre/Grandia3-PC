# Replace (or add) one translation line: python tr_set.py <file.tsv> <key> <text>
# A broken entry (text split over several lines by an earlier edit) is repaired as well.
import sys
path, key, text = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(path, encoding='utf-8').read().split('\n')
out, skip, done = [], False, False
for ln in lines:
    if ln.startswith(key + '\t'):
        out.append(key + '\t' + text)
        skip, done = True, True
        continue
    if skip and ln and not ln.startswith('#') and (len(ln) < 17 or ln[16] != '\t'):
        continue  # continuation of a broken entry
    skip = False
    out.append(ln)
if not done:
    out.insert(len(out) - 1 if out and out[-1] == '' else len(out), key + '\t' + text)
open(path, 'w', encoding='utf-8').write('\n'.join(out))
