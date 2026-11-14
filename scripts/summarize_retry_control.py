#!/usr/bin/env python3
"""Report paired retry-control TPS, write completion, waits and all failures."""
import csv,json,statistics,sys
from pathlib import Path


def main():
    out=Path(sys.argv[1]);metadata=json.loads((out/'metadata.json').read_text());config=metadata['config']
    rows=list(csv.DictReader((out/'results.csv').open()));summaries=[]
    med=lambda values:statistics.median(values)
    for cc in config['algorithms']:
        for policy in config['policies']:
            for theta in config['thetas']:
                for mode in config['retry_modes']:
                    group=[r for r in rows if r['cc']==cc and r['wait_policy']==policy and float(r['theta'])==theta and r['retry_mode']==mode]
                    if len(group)!=config['rounds']:raise RuntimeError(('missing results',cc,policy,theta,mode,len(group)))
                    result=dict(algorithm=cc,policy=policy,theta=theta,retry_mode=mode,rounds=len(group))
                    for field in ['throughput_committed_txn_s','retry_exhausted','logical_unfinished','deadline_aborts','wound_aborts','committed_read_only','committed_with_writes','retry_backoff_count','retry_backoff_total_ns']:
                        result[field+'_median']=med(float(r[field]) for r in group)
                    result['tps_min']=min(float(r['throughput_committed_txn_s']) for r in group)
                    result['tps_max']=max(float(r['throughput_committed_txn_s']) for r in group)
                    result['write_tps_median']=med(int(r['committed_with_writes'])/(int(r['elapsed_ns'])/1e9) for r in group)
                    for field in ['aborted','retries','lock_wait_total_ns','retry_backoff_total_ns']:
                        result[field+'_per_commit_median']=med(int(r[field])/max(1,int(r['committed'])) for r in group)
                    summaries.append(result)
    with (out/'summary.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(summaries[0]));writer.writeheader();writer.writerows(summaries)
    get=lambda cc,p,z,m:next(r for r in summaries if r['algorithm']==cc and r['policy']==p and r['theta']==z and r['retry_mode']==m)
    lines=['# YCSB-E retry control comparison','',f"Single-machine compact backends, same trace and unchanged 1000-attempt cap. Each cell is the median of {config['rounds']} runs. All individual results and source snapshots are retained.",
           '',f'CONFIG: `{config}`','',f"Failures: {metadata['failures']}; valid trials: {len(rows)}."]
    for theta in config['thetas']:
        lines+=['',f'## Theta {theta:g}','','| Algorithm | Policy | Off TPS | Adaptive TPS | Ratio | Off exhausted | Adaptive exhausted | Off writes/s | Adaptive writes/s | Off unfinished | Adaptive unfinished |',
                '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
        for cc in config['algorithms']:
            for policy in config['policies']:
                a,b=get(cc,policy,theta,'off'),get(cc,policy,theta,'adaptive')
                t='throughput_committed_txn_s_median';e='retry_exhausted_median';u='logical_unfinished_median'
                lines.append(f"| {cc} | {policy} | {a[t]:,.1f} | {b[t]:,.1f} | {b[t]/max(1,a[t]):.2f}x | {a[e]:,.0f} | {b[e]:,.0f} | {a['write_tps_median']:,.1f} | {b['write_tps_median']:,.1f} | {a[u]:,.0f} | {b[u]:,.0f} |")
    lines+=['','## Attempt and delay costs','','Ratios are calculated per trial, then the median is taken. Wait/backoff are sums across workers per committed transaction, not added wall time.',
            '','| Algorithm | Policy | Theta | Retry mode | TPS min--max | Aborts/commit | Retries/commit | Lock wait ms/commit | Retry delay ms/commit |',
            '|---|---|---:|---|---:|---:|---:|---:|---:|']
    for r in summaries:
        lines.append(f"| {r['algorithm']} | {r['policy']} | {r['theta']:g} | {r['retry_mode']} | {r['tps_min']:,.1f}--{r['tps_max']:,.1f} | {r['aborted_per_commit_median']:.2f} | {r['retries_per_commit_median']:.2f} | {r['lock_wait_total_ns_per_commit_median']/1e6:.4f} | {r['retry_backoff_total_ns_per_commit_median']/1e6:.4f} |")
    high=max(config['thetas']);hi=[r for r in rows if float(r['theta'])==high and r['retry_mode']=='adaptive']
    old=[r for r in rows if float(r['theta'])==high and r['retry_mode']=='off']
    lines+=['','## Interpretation','',
            f"At theta={high:g}, retry-exhausted logical transactions across all trials, adaptive: {sum(int(r['retry_exhausted']) for r in hi):,}; off: {sum(int(r['retry_exhausted']) for r in old):,}.",
            'A failed transaction now releases all locks before randomized retry pacing. No-wait/wait-die/wound-wait acquisition rules, transaction ages, isolation and attempt caps are unchanged. No global serialization or predicted winners are introduced.',
            'Compare committed transactions containing writes as well as total TPS. The old path can exhaust and discard difficult transactions before proceeding to later read-only transactions. Lower total TPS with fewer exhausted writers is a real tradeoff and is not hidden.',
            'This is a short-window diagnostic with three repetitions, not a proof of starvation freedom or a guarantee for all workloads. Window-end unfinished transactions and run-to-run variability remain.',
            'The YCSB-E variant uses fixed key-width ranges and a finite repeated trace. Insert labels can become overwrites after warmup; keys do not grow without bound.',
            'record-2PL does not provide range phantom protection; algorithm throughput is not an isolation-equivalent ranking.','']
    (out/'summary.md').write_text('\n'.join(lines))
    print('wrote summary for',len(rows),'trials')

if __name__=='__main__':main()
