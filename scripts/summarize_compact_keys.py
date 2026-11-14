#!/usr/bin/env python3
"""Summarize retained benchmark CSVs without dropping slow/failed trials."""
import argparse,csv,statistics
from collections import defaultdict
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('csv',type=Path);args=p.parse_args()
rows=list(csv.DictReader(args.csv.open()));groups=defaultdict(list)
for row in rows:groups[(row.get('scenario','steady-hot'),row['cc'],row['wait_policy'],int(row['threads']),row['key_lock_table'])].append(row)
fields=['scenario','cc','wait_policy','threads','backend','runs','median_committed_txn_s',
        'min_committed_txn_s','max_committed_txn_s','median_aborts_per_commit',
        'median_retries_per_commit','median_wait_ns_per_commit','median_unfinished',
        'median_retry_exhausted','median_deadline_aborts','median_lock_bytes','median_directory_bytes']
with args.csv.with_name('summary.csv').open('w',newline='') as f:
    writer=csv.writer(f);writer.writerow(fields)
    for key,rs in sorted(groups.items()):
        rates=[float(r['throughput_committed_txn_s']) for r in rs]
        med=lambda field:statistics.median(float(r[field]) for r in rs)
        ratio=lambda field:statistics.median(float(r[field])/max(int(r['committed']),1) for r in rs)
        writer.writerow([*key,len(rs),statistics.median(rates),min(rates),max(rates),
            ratio('aborted'),ratio('retries'),ratio('lock_wait_total_ns'),med('logical_unfinished'),
            med('retry_exhausted'),med('deadline_aborts'),med('key_lock_bytes'),med('key_directory_bytes')])
print(f'{len(rows)} trials -> {len(groups)} groups; {args.csv.with_name("summary.csv")}')
