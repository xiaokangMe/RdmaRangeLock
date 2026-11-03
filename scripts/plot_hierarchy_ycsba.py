#!/usr/bin/env python3
"""Audit the targeted hierarchy/YCSB-A run and plot paired historical comparisons."""
import argparse
import base64
import csv
import hashlib
import html
import itertools
import json
from pathlib import Path
import statistics as st
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from run_verified_matrices import read_rows, validate, ERRORS

ROOT=Path(__file__).resolve().parent.parent


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def key(row): return tuple(int(row[f]) for f in ('bucket','threads','seed'))
def metric(row,name):
    seconds=int(row['elapsed_ns'])/1e9
    if name=='tps': return int(row['committed'])/seconds
    if name=='attempts': return int(row['transactions'])/seconds
    if name=='abort': return 100*int(row['aborted'])/int(row['transactions'])
    if name=='wr': return int(row['rdma_operations'])/int(row['transactions'])
    if name=='locks': return int(row['lock_acquires'])/int(row['transactions'])
    raise ValueError(name)
def triplet(rows,name):
    v=[metric(r,name) for r in rows]
    return st.median(v),min(v),max(v)
def format3(v,digits=1): return f'{v[0]:,.{digits}f} [{v[1]:,.{digits}f}, {v[2]:,.{digits}f}]'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',required=True)
    parser.add_argument('--tag',default='hierarchy-ycsba-2026-11-03')
    args=parser.parse_args()
    mpath=Path(args.manifest).resolve(); m=json.loads(mpath.read_text()); c=m['config']
    assert m['status']=='complete' and m['completed_cases']==c['expected_cases']
    newpath=Path(m['result_csv']); new=read_rows(newpath)
    assert digest(newpath)==m['csv_sha256']
    oldpath=ROOT/c['baseline_csv']; bm=json.loads((ROOT/c['baseline_manifest']).read_text())
    assert digest(oldpath)==bm['cross-host_sha256'] and bm['source_commit']==c['baseline_commit']
    # All effective workload, repetition and transport policy values match.
    for field in ('duration_seconds','keys','transactions','operations','threads','buckets','seeds',
                  'zipf','tpcc_items','profile','slot_lookup','local_gate','gate_retry','gid_index',
                  'rdma_connections','server_roce','client_roce'):
        assert c[field]==bm['config'][field], 'baseline configuration differs: '+field
    old=[r for r in read_rows(oldpath) if r['cc']==c['cc'] and r['workload']==c['workload']]
    expected=set(itertools.product(c['buckets'],c['threads'],c['seeds']))
    for label,rows in [('before',old),('after',new)]:
        assert len(rows)==c['expected_cases'] and set(map(key,rows))==expected and len(set(map(key,rows)))==len(rows)
        validate(rows,c['duration_seconds'],c['expected_cases'])
        for r in rows:
            assert int(r['run'])==c['seeds'].index(int(r['seed']))+1
            assert r['index']==c['index'] and r['transport']=='rdma-cross-host'
            assert int(r['range'])==0 and float(r['zipf'])==c['zipf']
            assert r['server_host']==c['server_roce'] and r['client_host']==c['client_roce']
            assert int(r['hierarchy_bucket_width'])==int(r['bucket']) and int(r['rdma_connections'])==min(int(r['threads']),2)
            assert int(r['index_order'])==128
            assert not any(int(r[f]) for f in ('lock_capacity_aborts','lock_gate_aborts','rdma_gate_transport_errors'))
            assert not any(int(r[f]) for f in ('inserts','deletes','range_reads'))
            if label=='after': assert r['source_commit']==m['source_commit'] and int(r['committed'])>0
    figdir=ROOT/'figures'/args.tag;figdir.mkdir(parents=True,exist_ok=True)
    results=ROOT/'experiments/results';reportdir=ROOT/'docs/notes'
    summary=[]
    def group(rows,b,t): return [r for r in rows if int(r['bucket'])==b and int(r['threads'])==t]
    for b,t in itertools.product(c['buckets'],c['threads']):
        a,z=group(old,b,t),group(new,b,t)
        record={'bucket':b,'threads':t,'old_committed':[int(r['committed']) for r in sorted(a,key=key)],
                'new_committed':[int(r['committed']) for r in sorted(z,key=key)]}
        for label,rows in [('before',a),('after',z)]:
            for name in ('tps','abort','attempts','wr','locks'):
                for suffix,value in zip(('median','min','max'),triplet(rows,name)):
                    record[label+'_'+name+'_'+suffix]=value
        before=record['before_tps_median'];after=record['after_tps_median']
        record['throughput_ratio']=after/before if before else None
        record['abort_change_pp']=record['after_abort_median']-record['before_abort_median']
        summary.append(record)
    flat=[{k:json.dumps(v) if isinstance(v,list) else v for k,v in r.items()} for r in summary]
    summarypath=results/(args.tag+'-summary.csv')
    with summarypath.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(flat[0]));w.writeheader();w.writerows(flat)
    colors={'before':'#C6783B','after':'#146C94'}
    plt.rcParams.update({'font.size':11,'axes.spines.top':False,'axes.spines.right':False,
                         'axes.titleweight':'bold','savefig.facecolor':'white'})
    def draw(ax,rows,b,name,label):
        values=[triplet(group(rows,b,t),name) for t in c['threads']]
        med,lo,hi=zip(*values)
        ax.plot(c['threads'],med,color=colors[label],marker='o',lw=2,label='Before (historical)' if label=='before' else 'After (new run)')
        ax.fill_between(c['threads'],lo,hi,color=colors[label],alpha=.15)
        ax.set_xscale('log',base=2);ax.set_xticks(c['threads'],list(map(str,c['threads'])))
        ax.grid(True,alpha=.2)
    fig,axes=plt.subplots(2,3,figsize=(15,8),layout='constrained')
    for col,b in enumerate(c['buckets']):
        for i,(name,ylabel) in enumerate([('tps','Committed transactions / s'),('abort','Abort rate (%)')]):
            ax=axes[i,col]
            draw(ax,old,b,name,'before');draw(ax,new,b,name,'after')
            if i==0:
                ax.set_title('Bucket width = '+str(b));ax.set_yscale('symlog',linthresh=1)
                ax.set_ylim(bottom=0);ax.set_yticks([0,1,10,100,1000,10000],['0','1','10','100','1,000','10,000'])
                ax.set_ylim(top=max(metric(r,'tps') for r in old+new)*1.5)
            else:ax.set_ylim(0,101);ax.set_xlabel('Worker threads')
            if col==0: ax.set_ylabel(ylabel)
    axes[0,0].legend(loc='lower left',fontsize=9)
    fig.suptitle('RDMA hierarchy / YCSB-A: before and after\n10 s per case, 3 seeds; median and min-max; throughput uses symlog (linear below 1)',fontsize=15)
    overview=figdir/'before-after.png';fig.savefig(overview,dpi=180);fig.savefig(figdir/'before-after.pdf');plt.close(fig)
    fig,axes=plt.subplots(1,3,figsize=(15,4.6),layout='constrained')
    for ax,b in zip(axes,c['buckets']):
        for name,label,color in [('attempts','Attempts / s','#8496A3'),('tps','Committed / s','#146C94')]:
            values=[triplet(group(new,b,t),name) for t in c['threads']]
            med,lo,hi=zip(*values)
            ax.plot(c['threads'],med,marker='o',label=label,color=color)
            ax.fill_between(c['threads'],lo,hi,color=color,alpha=.15)
        ax.set_xscale('log',base=2);ax.set_xticks(c['threads'],list(map(str,c['threads'])))
        ax.set_ylim(bottom=0);ax.grid(True,alpha=.2);ax.set_xlabel('Worker threads');ax.set_title('Bucket width = '+str(b))
    axes[0].set_ylabel('Transactions / s');axes[0].legend()
    fig.suptitle('After optimization: attempted work vs successful work\nYCSB-A remains a hot-key NO_WAIT workload',fontsize=14)
    detail=figdir/'after-progress.png';fig.savefig(detail,dpi=180);fig.savefig(figdir/'after-progress.pdf');plt.close(fig)
    audit={'status':'PASS','pairs':len(new),'groups':len(summary),'new_source_commit':m['source_commit'],
           'old_source_commit':bm['source_commit'],'new_csv':str(newpath),'new_sha256':digest(newpath),
           'old_csv':str(oldpath),'old_sha256':digest(oldpath),'correctness':m['correctness'],
           'new_zero_commit_cases':sum(int(r['committed'])==0 for r in new),'old_zero_commit_cases':sum(int(r['committed'])==0 for r in old),
           'new_elapsed_seconds':[min(int(r['elapsed_ns'])/1e9 for r in new),max(int(r['elapsed_ns'])/1e9 for r in new)],
           'new_error_totals':{f:sum(int(r[f]) for r in new) for f in list(ERRORS)+['lock_capacity_aborts','lock_gate_aborts','rdma_gate_transport_errors']},
           'config_match':True,'matrix_is_targeted_subset':True}
    (results/(args.tag+'-audit.json')).write_text(json.dumps(audit,indent=2))
    def cell(r,label,name,d=1):return format3(tuple(r[f'{label}_{name}_{s}'] for s in ('median','min','max')),d)
    lines=['# RDMA 层级锁 YCSB-A 优化前后定向复测（2026-11-03）','',
           '只重跑跨机 RDMA 层级锁的 YCSB-A 子集；没有运行本地矩阵、其他协议或 YCSB-E。','',
           f"优化前测量提交 `{bm['source_commit']}`，来自已验收的历史十秒正式批次；优化后提交 `{m['source_commit']}`，实现修复来自 `cf3612b`。本次未重新运行旧代码，是历史基线与新测量的比较，无法完全排除两个时段的系统噪声，也不能把组合补丁的收益拆分归因。",'',
           '## 配置与验收','',
           f"配置：`experiments/configs/rdma_hierarchy_ycsba_20261103.json`。每 case 10 秒；seed=8/9/10；线程 1/2/4/8/16/32；桶宽 16/64/256，共 54 case。10,000 keys，trace 5,000 transactions 循环，10 ops/txn，Zipf=0.99，50% 读/50% 更新。v2 索引、order=128，NO_WAIT；profile=0、诊断=0、linear lookup、local gate=1、gate retry=64、QP=min(threads,2)。",'',
           '.66 控制并运行 server（10.0.0.3），.67 client（10.0.0.4），GID index=3；两端使用哈希一致的部署文件。正确性先行：真实 RDMA CTest 27/27 无跳过，跨机层级锁保护与最终锁表归零检查通过。','',
           '容量上界按每活跃事务 1 根 + 最多 10 桶 + 最多 10 记录计算：32 × 21 = 672，即使全落在同一 shard 也小于 8192 槽。首先测最高并发的三个桶宽；这三个合格 case 计入 54 次，不额外采样挑选。','',
           f"54/54 参数点、seed、唯一性、事务守恒及旧配置匹配均通过。实际窗口 {audit['new_elapsed_seconds'][0]:.6f}–{audit['new_elapsed_seconds'][1]:.6f} 秒；硬错误、容量和 gate 中止均为 0。新数据零提交 case={audit['new_zero_commit_cases']}，历史同子集零提交 case={audit['old_zero_commit_cases']}。",'',
           '## 对比图','',f'![优化前后吞吐量与中止率](../../figures/{args.tag}/before-after.png)','',
           '曲线为三次 seed 的中位数，阴影为最小—最大，不是置信区间。吞吐轴使用 symlog，1 txn/s 以下是线性，以显示零值与接近零的旧数据；中止率使用线性轴。TPS 由 committed / 实际 elapsed 重算，避免 CSV 显示精度造成误差。','',
           f'![优化后尝试与成功吞吐](../../figures/{args.tag}/after-progress.png)','',
           '## 结果表','',
           '吞吐：中位数 [最小值, 最大值]，单位 committed txn/s；中止率同样显示三次范围。倍率是两组中位数之比；旧中位数为零时不计算。旧值接近零时倍率会非常大，只能表示恢复提交，不能解释为一般性的算法加速比。','',
           '| 桶宽 | 线程 | 优化前吞吐 | 优化后吞吐 | 吞吐倍率 | 优化前中止率 % | 优化后中止率 % |',
           '|---:|---:|---:|---:|---:|---:|---:|']
    for r in summary:
        ratio='不可计算（旧值为0）' if r['throughput_ratio'] is None else f"{r['throughput_ratio']:,.2f}×"
        lines.append(f"| {r['bucket']} | {r['threads']} | {cell(r,'before','tps')} | {cell(r,'after','tps')} | {ratio} | {cell(r,'before','abort',2)} | {cell(r,'after','abort',2)} |")
    focus=next(r for r in summary if r['bucket']==256 and r['threads']==4)
    regressions=[r for r in summary if r['after_tps_median']<r['before_tps_median']]
    lines+=['','## 效果与仍存在的问题','',
            f"原异常点（桶宽 256、4 线程）历史三次提交数为 `{focus['old_committed']}`，本次为 `{focus['new_committed']}`；中位成功吞吐从 {focus['before_tps_median']:.3f} 变为 {focus['after_tps_median']:.3f} txn/s，中止率从 {focus['before_abort_median']:.2f}% 变为 {focus['after_abort_median']:.2f}%。这说明在本次十秒样本中，原先几乎零提交的退化形态已消除；有限样本不能证明所有事务无饥饿。",'',
            f"18 个参数组中，优化后吞吐中位数低于历史基线的有 {len(regressions)} 组。"+('分别为：'+ '、'.join(f"b{r['bucket']}/t{r['threads']}（{r['throughput_ratio']:.2f}×）" for r in regressions)+'。' if regressions else ''), '',
            '点访问现在需要根、桶意向与记录锁；执行前计划也改变了申请数量和持锁时长。因此不能预设所有线程数都提速，单线程和无冲突路径要单独看成本。高线程的热点 NO_WAIT 中止已被观察到；共享 QP、根元数据及 gate 调度是待 profile 核实的候选瓶颈。吞吐随线程增加下降不等于保护语义仍错误。','',
            '本轮没有开启 profile 或逐 worker 诊断，不能从这些正式计时数据判定各等待阶段占比，也不能证明每个线程或事务都无饥饿。此前独立诊断检查只能辅助说明短时进展。下一步应先做独立 profile/消融，再决定是否优化锁元数据与传输调度；等待策略维持 NO_WAIT，暂不引入 Wait-Die。','',
            '预生成访问计划的排序、去重与分配在测量窗口外，完整端到端成本未计入。YCSB-A 只有点读/更新，没有范围扫描；本报告说明点访问并发和进展改善，不能据此推断 YCSB-E 范围查询性能或幻读保护效果。保护语义以正确性测试为依据。','',
            '## 数据与复现','',f"- 新原始 CSV：`{newpath.relative_to(ROOT)}`；SHA256 `{digest(newpath)}`。",f"- 新 manifest：`{mpath.relative_to(ROOT)}`；包含运行环境、部署哈希与源码提交。",f"- 旧原始 CSV：`{oldpath.relative_to(ROOT)}`；SHA256 `{digest(oldpath)}`。",f"- 日志：`{Path(m['log_dir']).relative_to(ROOT)}`。",f"- 汇总与验收：`experiments/results/{args.tag}-summary.csv`、`experiments/results/{args.tag}-audit.json`。",'',
            '```bash','python3 scripts/run_hierarchy_ycsba.py',f'python3 scripts/plot_hierarchy_ycsba.py --manifest {mpath.relative_to(ROOT)}','```','',
            '第一条会重新启动独立定向实验；只画已有数据时执行第二条。原始 CSV、manifest 和日志不入库。']
    report=reportdir/(args.tag+'.md');report.write_text('\n'.join(lines)+'\n')
    # Offline, self-contained viewing copy; preserve the authoritative Markdown.
    body=[]
    in_table=False
    for line in lines:
        if in_table and not line.startswith('|'):
            body.append('</tbody></table>'); in_table=False
        if line.startswith('|'):
            if line.startswith('|---'): continue
            if not in_table: body.append('<table><tbody>'); in_table=True
            body.append('<tr>'+''.join('<td>'+html.escape(v.strip())+'</td>' for v in line.strip('|').split('|'))+'</tr>')
        elif line.startswith('#'):
            level=len(line)-len(line.lstrip('#'))
            body.append('<h%d>'%level+html.escape(line.lstrip('# '))+'</h%d>'%level)
        elif line.startswith('!['):
            imagepath=overview if 'before-after.png' in line else detail
            body.append('<img style="width:100%;height:auto" src="data:image/png;base64,'+base64.b64encode(imagepath.read_bytes()).decode()+'">')
        else:body.append('<div style="white-space:pre-wrap">'+html.escape(line)+'</div>')
    htmldir=ROOT/'docs/reports';htmldir.mkdir(parents=True,exist_ok=True)
    (htmldir/(args.tag+'.html')).write_text('<!doctype html><style>table{border-collapse:collapse;font-size:13px;width:100%}td{padding:8px;border-bottom:1px solid #ddd}tr:first-child{font-weight:bold;background:#eaf2f7}h1,h2{color:#174461}body{color:#22313f}</style><meta charset="utf-8"><title>RDMA hierarchy YCSB-A</title><body style="font:16px/1.7 system-ui;max-width:1400px;margin:30px auto;padding:0 24px">'+''.join(body)+'</body>')
    print(json.dumps(audit,indent=2))
    print('report:',report)

if __name__=='__main__': main()
