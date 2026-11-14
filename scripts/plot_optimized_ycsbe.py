#!/usr/bin/env python3
"""Plot measured values with exactly the requested theta ticks."""
import csv, itertools, json, sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

LABELS = {'record-2pl': 'Record 2PL', 'next-key': 'Next-key', 'gap-lock': 'Gap',
          'prior-key': 'Prior-key', 'hierarchical': 'Hierarchy b64', 'exact-range': 'Exact-range'}


def main():
    out = Path(sys.argv[1]); meta = json.loads((out/'metadata.json').read_text()); cfg = meta['config']
    rows = list(csv.DictReader((out/'results.csv').open()))
    get = lambda a, p, z: next((r for r in rows if r['cc']==a and r['wait_policy']==p and float(r['theta'])==z), None)
    thetas = cfg['thetas']; ticks = [f'{z:g}' for z in thetas]
    missing = [(a,p,z) for a,p,z in itertools.product(cfg['algorithms'],cfg['policies'],thetas) if get(a,p,z) is None]
    def figure(filename, ylabel, value, logarithmic=False):
        fig, axes = plt.subplots(1, len(cfg['policies']), figsize=(19,5.2), sharex=True)
        for ax, policy in zip(axes, cfg['policies']):
            for a in cfg['algorithms']:
                ys = [value(get(a,policy,z)) if get(a,policy,z) else float('nan') for z in thetas]
                ax.plot(thetas, ys, marker='o', markersize=4, linewidth=1.6, label=LABELS.get(a,a))
            ax.set(title=policy, xlabel='Zipf theta', ylabel=ylabel)
            ax.set_xticks(thetas, ticks); ax.tick_params(axis='x',labelrotation=40)
            ax.grid(True,alpha=.25)
            if logarithmic: ax.set_yscale('symlog',linthresh=1)
        axes[-1].legend(fontsize=8)
        fig.suptitle(f"YCSB-E | {cfg['threads']} threads | {cfg['keys']:,} keys | {cfg['ops']} ops/txn | warmup {cfg['warmup']}s + measure {cfg['duration']}s | one trial/case",fontsize=11)
        fig.tight_layout(); fig.savefig(out/(filename+'.png'),dpi=180); fig.savefig(out/(filename+'.pdf')); plt.close(fig)
    figure('throughput', 'Committed transactions/s', lambda r: float(r['throughput_committed_txn_s']))
    figure('write_throughput','Committed transactions containing writes/s',lambda r:int(r['committed_with_writes'])/(int(r['elapsed_ns'])/1e9))
    figure('retry_exhausted','Logical transactions exhausting retry limit',lambda r:int(r['retry_exhausted']),True)
    figure('unfinished','Unfinished logical transactions (includes deadline)',lambda r:int(r['logical_unfinished']),True)
    figure('retries_per_commit','Retries / committed transaction',lambda r:int(r['retries'])/max(1,int(r['committed'])),True)
    lines=['# Optimized local YCSB-E sweep','',f'Configuration: `{cfg}`','',
           'Each configuration runs once. Values are measurements, with no interpolation or repeat-based confidence intervals. All figures use the actual theta values as ticks.',
           '',f'Valid cases: {len(rows)}. Missing cases: {missing}.',
           '', 'Optional latency/row statistics are disabled in this performance sweep. Zero histogram or row fields do not mean no waiting or no reads. Core retry, backoff and completion counters remain enabled.',
           '', 'Record 2PL does not protect gaps or guarantee phantom avoidance. Compare its throughput with that semantic difference in mind. A fixed generated trace is replayed during both windows; inserts can become overwrites after their first successful commit. This is a closed replay experiment, not a growing-table insert benchmark.']
    for p in cfg['policies']:
        lines+=['',f'## {p}: committed TPS','', '| Theta | '+' | '.join(LABELS[a] for a in cfg['algorithms'])+' |', '|---|'+'---:|'*len(cfg['algorithms'])]
        for z in thetas:
            lines+=['| '+f'{z:g}'+' | '+' | '.join(f"{float(get(a,p,z)['throughput_committed_txn_s']):,.1f}" if get(a,p,z) else 'MISSING' for a in cfg['algorithms'])+' |']
    lines+=['','## Theta 1.5: completion and retries','',
            '| Algorithm | Policy | TPS | Write txns/s | Aborts/commit | Retries/commit | Exhausted | Unfinished | Max retries | Backoff ms/commit |',
            '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|']
    for a,p in itertools.product(cfg['algorithms'],cfg['policies']):
        r=get(a,p,1.5)
        if not r:continue
        commits=max(1,int(r['committed']));seconds=int(r['elapsed_ns'])/1e9
        lines += [f"| {LABELS[a]} | {p} | {float(r['throughput_committed_txn_s']):,.1f} | {int(r['committed_with_writes'])/seconds:,.1f} | {int(r['aborted'])/commits:.3f} | {int(r['retries'])/commits:.3f} | {r['retry_exhausted']} | {r['logical_unfinished']} | {r['max_retries']} | {int(r['retry_backoff_total_ns'])/commits/1e6:.3f} |"]
    (out/'summary.md').write_text('\n'.join(lines)+'\n')
    print(f'Plots and tables saved in {out}; missing={len(missing)}')

if __name__ == '__main__':main()
