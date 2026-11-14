#!/usr/bin/env python3
import argparse,csv,statistics
from collections import defaultdict
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('csv',type=Path);a=p.parse_args();rows=list(csv.DictReader(a.csv.open()))
groups=defaultdict(list)
for r in rows:groups[(int(r['hierarchy_bucket_width']),r['wait_policy'],int(r['threads']),r['hierarchy_lock_table'],r.get('root_variant','0'),r.get('padding_variant','0'),r.get('metrics_mode','full'))].append(r)
with a.csv.with_name('summary.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['bucket_width','wait_policy','threads','backend','implicit_root','padded','metrics','runs','median_tps','min_tps','max_tps','abort_per_commit','retry_per_commit','wait_ms_per_commit','unfinished','retry_exhausted','deadline_aborts','authority_bytes','lease_bytes','plan_bytes','cas_retries'])
 for key,rs in sorted(groups.items()):
  med=lambda field:statistics.median(float(r[field]) for r in rs)
  ratio=lambda field:statistics.median(float(r[field])/max(1,int(r['committed'])) for r in rs)
  w.writerow([*key,len(rs),med('throughput_committed_txn_s'),min(float(r['throughput_committed_txn_s']) for r in rs),max(float(r['throughput_committed_txn_s']) for r in rs),ratio('aborted'),ratio('retries'),ratio('lock_wait_total_ns')/1e6,med('logical_unfinished'),med('retry_exhausted'),med('deadline_aborts'),med('hierarchy_lock_bytes'),med('hierarchy_lease_bytes'),med('hierarchy_plan_bytes'),med('hierarchy_cas_retries')])
print(f'{len(rows)} trials -> {len(groups)} groups; {a.csv.with_name("summary.csv")}')
