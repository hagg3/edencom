#!/usr/bin/env python3
"""FT.15a: merge power-sampler CSVs (+ optional [eden-power] logs) into a per-leg markdown table.
Usage: power-report.py [--settle=120] LEG.csv[:LEG.log] ...   (legs in run order; dW is against the mean of the
neighbouring legs whose name starts with the first leg's baseline prefix 'E100', or the floor-subtracted W if none).
W = -mA*mV, deduped on the gauge's UpdateTime (it refreshes every ~20 s); the first --settle seconds are dropped."""
import csv, os, re, statistics as st, sys

def load(path, settle):
    rows = list(csv.DictReader(open(path)))
    if not rows: return None
    t0 = int(rows[0]['t']); seen = set(); w = []; temps = []; gpu = []; cpu = []; ext = False
    for r in rows:
        if int(r['t']) - t0 < settle: continue
        try:
            a = int(r['amp_mA']); v = int(r['mV'])
        except ValueError: continue
        if a >= 2**63: a -= 2**64
        if r['extConn'].strip() == 'Yes': ext = True
        for lst, key in ((gpu, 'gpuDev'), (cpu, 'cpu' if 'cpu' in r else 'cpuEmod')):
            try: lst.append(float(r[key]))
            except ValueError: pass
        try: temps.append((int(r['t']), int(r['battC']) / 100.0))
        except ValueError: pass
        if r['updateTime'] in seen: continue
        seen.add(r['updateTime']); w.append(-a * v / 1e6)
    slope = 0.0
    if len(temps) > 1 and temps[-1][0] > temps[0][0]:
        slope = (temps[-1][1] - temps[0][1]) / ((temps[-1][0] - temps[0][0]) / 60.0)
    return dict(w=w, slope=slope, gpu=st.mean(gpu) if gpu else 0, cpu=st.mean(cpu) if cpu else 0, ext=ext)

def powerlog(path):
    fps, p50, th = [], [], 0
    for line in open(path, errors='replace'):
        if '[eden-power]' not in line or 'mode=play' not in line: continue
        m = re.search(r'fps=([\d.]+) p50_ms=([\d.]+)', line); t = re.search(r'thermal=(-?\d+)', line)
        if m: fps.append(float(m.group(1))); p50.append(float(m.group(2)))
        if t: th = max(th, int(t.group(1)))
    return (st.median(fps) if fps else 0, st.median(p50) if p50 else 0, th)

def main():
    settle = 120; args = []
    for a in sys.argv[1:]:
        if a.startswith('--settle='): settle = int(a[9:])
        else: args.append(a)
    legs = []
    for a in args:
        csvp, _, logp = a.partition(':')
        name = os.path.splitext(os.path.basename(csvp))[0]
        d = load(csvp, settle)
        if d is None: continue
        d['name'] = name; d['pl'] = powerlog(logp) if logp and os.path.exists(logp) else None
        legs.append(d)
    base = [i for i, l in enumerate(legs) if l['name'].startswith('E100')]
    print('| Leg | W mean ± sd | n | dW vs bracketing E100 | batt °C/min | GPU % | CPU % | fps p50 | max thermal |')
    print('|---|---|---|---|---|---|---|---|---|')
    for i, l in enumerate(legs):
        m = st.mean(l['w']) if l['w'] else 0; sd = st.pstdev(l['w']) if len(l['w']) > 1 else 0
        nb = [st.mean(legs[j]['w']) for j in base if j != i and abs(j - i) <= 2 and legs[j]['w']]
        dw = '%+.2f' % (m - st.mean(nb)) if nb and i not in base else '-'
        pl = l['pl']
        print('| %s%s | %.2f ± %.2f | %d | %s | %+.2f | %.0f | %.0f | %s | %s |' % (
            l['name'], ' **PLUGGED**' if l['ext'] else '', m, sd, len(l['w']), dw, l['slope'], l['gpu'], l['cpu'],
            '%.1f (%.1f ms)' % (pl[0], pl[1]) if pl else '-', pl[2] if pl else '-'))
main()
