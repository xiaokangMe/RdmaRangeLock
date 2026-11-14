#!/usr/bin/env python3
"""Summarize every successful Precision trial, including completion costs."""
import csv,json,statistics,sys
from pathlib import Path


def main():
    out=Path(sys.argv[1]);metadata=json.loads((out/'metadata.json').read_text())
    config=metadata['config'];rows=list(csv.DictReader((out/'results.csv').open()))
    summary=[]
    for theta in config['thetas']:
        for policy in config['policies']:
            for backend,metrics in [('generic','full'),('compact','full'),('compact','fast')]:
                group=[row for row in rows if float(row['theta'])==theta and row['wait_policy']==policy and row['backend']==backend and row['metrics']==metrics]
                if len(group)!=config['rounds']:raise RuntimeError(('missing group',theta,policy,backend,metrics,len(group)))
                trial=[]
                for row in group:
                    commits=int(row['committed'])
                    trial.append({
                        'tps':float(row['throughput_committed_txn_s']),
                        'aborts_per_commit':int(row['aborted'])/commits if commits else float('inf'),
                        'retries_per_commit':int(row['retries'])/commits if commits else float('inf'),
                        'wait_ms_per_commit':int(row['lock_wait_total_ns'])/commits/1e6 if commits else float('inf'),
                        'unfinished':int(row['logical_unfinished']),
                        'retry_exhausted':int(row['retry_exhausted']),
                        'deadline_aborts':int(row['deadline_aborts']),
                    })
                record=dict(theta=theta,policy=policy,backend=backend,metrics=metrics,trials=len(group))
                for field in trial[0]:record[field+'_median']=statistics.median(t[field] for t in trial)
                record['tps_min']=min(t['tps'] for t in trial);record['tps_max']=max(t['tps'] for t in trial)
                record['lock_bytes']=int(group[0]['precision_lock_bytes']);record['plan_bytes']=int(group[0]['precision_plan_bytes'])
                summary.append(record)
    with (out/'summary.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(summary[0]));writer.writeheader();writer.writerows(summary)
    text=(out/'summary.md').read_text().split('\n## Completion costs')[0]
    lines=[text,'','## Completion costs','','Per-trial ratios and counts below are medians across the three runs. Fast mode has no optional wait histogram; its wait value is unavailable, not zero.',
           '','| theta | policy | backend/metrics | TPS min--max | abort/commit | retry/commit | wait ms/commit | unfinished | retry exhausted | deadline aborts |',
           '|---:|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for row in summary:
        wait=f"{row['wait_ms_per_commit_median']:.4f}" if row['metrics']=='full' else 'n/a'
        lines.append(f"| {row['theta']:g} | {row['policy']} | {row['backend']}/{row['metrics']} | {row['tps_min']:,.1f}--{row['tps_max']:,.1f} | {row['aborts_per_commit_median']:.2f} | {row['retries_per_commit_median']:.2f} | {wait} | {row['unfinished_median']:,.0f} | {row['retry_exhausted_median']:,.0f} | {row['deadline_aborts_median']:,.0f} |")
    compact=[row for row in summary if row['backend']=='compact']
    lines+=['',f"Compact CAS state: {min(row['lock_bytes'] for row in compact):,}--{max(row['lock_bytes'] for row in compact):,} bytes; immutable operation plans: {min(row['plan_bytes'] for row in compact):,}--{max(row['plan_bytes'] for row in compact):,} bytes. Transaction contexts and held pointers are additional and recorded in raw CSV.",
            '', 'At high skew the completion counts and run-to-run ranges remain material. Disabling metrics does not consistently improve committed throughput under contention. No claim that retries or hotspot collapse have been eliminated.','']
    (out/'summary.md').write_text('\n'.join(lines))
    print('summarized',len(rows),'valid trials')

if __name__=='__main__':main()
