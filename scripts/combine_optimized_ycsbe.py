#!/usr/bin/env python3
"""Select each algorithm's complete latest sweep; retain explicit provenance."""
import csv,datetime,itertools,json,subprocess,sys
from pathlib import Path

def main():
    baseline,classic=map(Path,sys.argv[1:3])
    base_meta=json.loads((baseline/'metadata.json').read_text())
    new_meta=json.loads((classic/'metadata.json').read_text())
    cfg=base_meta['config']
    for key,value in cfg.items():
        if key!='algorithms':assert new_meta['config'][key]==value,(key,value,new_meta['config'][key])
    assert set(new_meta['config']['algorithms'])=={'next-key','gap-lock','prior-key'}
    sources=[(baseline,['record-2pl','hierarchical','exact-range'],base_meta),
             (classic,['next-key','gap-lock','prior-key'],new_meta)]
    out=baseline.parent/('optimized-ycsbe-final-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out.mkdir();rows=[];selection=[]
    for directory,algorithms,meta in sources:
        group=[r for r in csv.DictReader((directory/'results.csv').open()) if r['cc'] in algorithms]
        expected=set(itertools.product(algorithms,cfg['policies'],map(float,cfg['thetas'])))
        assert {(r['cc'],r['wait_policy'],float(r['theta'])) for r in group}==expected
        assert len(group)==len(expected)
        for r in group:
            r['source_campaign']=directory.name;r['source_runner_sha256']=meta['runner_sha256']
        rows+=group
        selection.append(dict(algorithms=algorithms,campaign=directory.name,runner_sha256=meta['runner_sha256'],source_sha256=meta['source_sha256']))
    with (out/'results.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    note=('The selected 162 measurements come from two explicitly identified source campaigns: '
          'Record/Hierarchy/Exact use the completed six-algorithm sweep; Next-key/Gap/Prior use '
          'the full 81-case rerun after scan-coverage reuse. All workload and measurement parameters '
          'match. No data points from the earlier classic implementation are retained in those three '
          'curves. The later code change is restricted to the classic interval execution branch. '
          'Each selected configuration has one measured trial; this is not a same-binary six-algorithm rerun.')
    (out/'metadata.json').write_text(json.dumps(dict(config=cfg,passed=len(rows),failures=0,selection=selection,provenance_note=note),indent=2))
    subprocess.run(['python3','scripts/plot_optimized_ycsbe.py',str(out)],check=True)
    print('FINAL '+str(out))

if __name__=='__main__':main()
