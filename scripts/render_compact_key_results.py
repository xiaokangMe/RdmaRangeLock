#!/usr/bin/env python3
import argparse,csv,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser();p.add_argument('main',type=Path);p.add_argument('supplement',type=Path)
args=p.parse_args();main=list(csv.DictReader((args.main/'results.csv').open()))
supp=list(csv.DictReader((args.supplement/'results.csv').open()))
for row in main+supp:
    assert all(int(row[f])==0 for f in ('index_errors','lock_errors','lock_release_failures','failed_transactions'))
    assert int(row['transactions'])==int(row['committed'])+int(row['aborted'])
    assert int(row['transactions'])==int(row['logical_started'])+int(row['retries'])
    assert int(row['logical_started'])==int(row['committed'])+int(row['logical_unfinished'])
protocols=('next-key','gap-lock','prior-key');policies=('no-wait','wait-die','wound-wait');threads=(1,8,32,64)
def select(rows,cc,policy,t,backend,scenario=None):
    return [r for r in rows if r['cc']==cc and r['wait_policy']==policy and int(r['threads'])==t
            and r['key_lock_table']==backend and (scenario is None or r['scenario']==scenario)]
def med(rows,field):return statistics.median(float(r[field]) for r in rows)
def ratio(rows,field):return statistics.median(float(r[field])/max(1,int(r['committed'])) for r in rows)
fig,axes=plt.subplots(3,3,figsize=(12,9),sharex=True,layout='constrained')
for i,cc in enumerate(protocols):
    for j,policy in enumerate(policies):
        ax=axes[i,j]
        for backend,color in [('generic','#d97706'),('compact','#2563eb')]:
            samples=[[float(r['throughput_committed_txn_s']) for r in select(main,cc,policy,t,backend)] for t in threads]
            assert all(len(s)==3 for s in samples)
            mids=[statistics.median(s) for s in samples]
            ax.plot(threads,mids,'o-',color=color,label=backend,linewidth=1.8,markersize=4)
            ax.fill_between(threads,[min(s) for s in samples],[max(s) for s in samples],color=color,alpha=.14)
        ax.set_yscale('log');ax.set_xticks(threads);ax.grid(True,alpha=.22,which='both')
        ax.set_title(f'{cc} / {policy}',fontsize=10)
        if j==0:ax.set_ylabel('Committed transactions / second')
        if i==2:ax.set_xlabel('OS worker threads')
axes[0,0].legend();fig.suptitle('YCSB-E, ordered Zipf 0.99, 10 operations, key width 32\nMedian of 3 trials; shading = min/max; full metrics; 1 s warmup + 3 s measurement',fontsize=12)
fig.savefig(args.main/'throughput.png',dpi=180);fig.savefig(args.main/'throughput.svg');plt.close(fig)
lines=['# 三种经典锁的本地 CAS 优化实测（2026-11-13）','',
       '机器：10.102.35.66，双路 Xeon Gold 5218，32 物理核 / 64 逻辑 CPU。分支 `codex/record2pl-compact-20261112`。',
       '',f'主实验保留 {len(main)} 次运行；补充实验保留 {len(supp)} 次运行。以下均为三轮中位数，不以单次最优值作结论。',
       '', '主实验：项目 YCSB-E，100,000 keys，5,000 条逻辑事务 trace，每事务 10 操作，约 95% 扫描 / 5% Insert，数值区间宽度 32，有序 Zipf 0.99，seed 7。每配置 warmup 1 秒 + 测量 3 秒，OS threads，最多 1,000 次尝试，等待超时 100 ms，full metrics。两后端顺序交错，实验顺序固定随机种子打乱，整机顺序执行。',
       '', '![吞吐与线程数](throughput.png)', '',
       '## 提交吞吐（txn/s）', '', '| 协议 | 策略 | 线程 | generic | compact | 倍率 |', '|---|---|---:|---:|---:|---:|']
for cc in protocols:
    for policy in policies:
        for t in threads:
            g=med(select(main,cc,policy,t,'generic'),'throughput_committed_txn_s')
            c=med(select(main,cc,policy,t,'compact'),'throughput_committed_txn_s')
            lines.append(f'| {cc} | {policy} | {t} | {g:,.1f} | {c:,.1f} | {c/g:.2f}× |')
lines+=['','## 高争用与未完成事务','',
    '下表为 64 线程；中止、重试和等待时间按各次运行的提交数归一化后取中位数。未完成包含重试耗尽及测量截止仍未完成的逻辑事务，吞吐不能单独代表服务质量。no-wait 的等待时间为 0 不代表没有执行重试开销。','',
    '| 协议 | 策略 | 后端 | 中止/提交 | 重试/提交 | 等待 ms/提交 | 未完成 | 重试耗尽 | deadline 中止 |',
    '|---|---|---|---:|---:|---:|---:|---:|---:|']
for cc in protocols:
    for policy in policies:
        for backend in ('generic','compact'):
            rs=select(main,cc,policy,64,backend)
            lines.append(f'| {cc} | {policy} | {backend} | {ratio(rs,"aborted"):.2f} | {ratio(rs,"retries"):.2f} | {ratio(rs,"lock_wait_total_ns")/1e6:.3f} | {med(rs,"logical_unfinished"):.0f} | {med(rs,"retry_exhausted"):.0f} | {med(rs,"deadline_aborts"):.0f} |')
lines+=['','## 一次性初始插入实验','',
    '使用全新的索引，去掉 warmup 和 duration，顺序消耗一次 5,000 条 YCSB-E trace，Zipf=0，每事务 10 操作，范围宽 32，32 线程，最多 100 次尝试。seed、数据装载规则与两后端相同。trace 中 Insert key 初始缺失；首次成功提交会填充空洞。同一 trace 内重复 Insert 仍可能命中已存在 key，因此不把所有 Insert 计数当作新物理行。','',
    '| 协议 | 策略 | generic txn/s | compact txn/s | 倍率 | generic 未完成 | compact 未完成 |',
    '|---|---|---:|---:|---:|---:|---:|']
for cc in protocols:
    for policy in policies:
        gs=select(supp,cc,policy,32,'generic','cold-uniform');cs=select(supp,cc,policy,32,'compact','cold-uniform')
        g=med(gs,'throughput_committed_txn_s');c=med(cs,'throughput_committed_txn_s')
        lines.append(f'| {cc} | {policy} | {g:,.1f} | {c:,.1f} | {c/g:.2f}× | {med(gs,"logical_unfinished"):.0f} | {med(cs,"logical_unfinished"):.0f} |')
lines+=['','## 关闭可选计数的性能','',
    '仅比较 compact 的 full 与 fast（key-latency=0、key-statistics=0），32 线程、no-wait、有序 Zipf 0.99、warmup 1 秒 + 测量 3 秒；此补充实验 max-attempts=100，与主实验的 1,000 次不同，不混入主实验表。fast 不输出有效的 rows/checksum 和细粒度延迟分布，错误、提交与重试计数仍保留。','',
    '| 协议 | full txn/s | fast txn/s | fast/full |','|---|---:|---:|---:|']
for cc in protocols:
    f=med(select(supp,cc,'no-wait',32,'compact','metrics-full'),'throughput_committed_txn_s')
    q=med(select(supp,cc,'no-wait',32,'compact','metrics-fast'),'throughput_committed_txn_s')
    lines.append(f'| {cc} | {f:,.1f} | {q:,.1f} | {q/f:.2f}× |')
lines+=['','## 解释边界','',
    '- 当前是有限、预注册 key 域的本地实现；未注册 key 的在线扩展尚未实现。预注册不插入数据。',
    '- next/prior 每个原子单元为 8 字节，整个目录共 2N+1 个单元；gap 为 record/guard 两个 N 大小的空间。宽范围需要多个 CAS，仍可能成为瓶颈。',
    '- 连续 key 域目录只存长度，主实验 sparse 目录为 0 字节；锁状态约 1.6 MB，另有 context 和预分配释放数组。generic 空间列未采样，不据此比较内存节省比例。',
    '- 第 64 个 context 的 S 继承 record 的保守 X 编码，会引入额外读读冲突；64 线程结果包含这一限制。',
    '- B+ 树 latch、邻居/key 集重检和数据提交仍保留。移除通用框架开销不能消除真实热点冲突。',
    '- 长时间主实验循环有限 trace，warmup 后部分 Insert 已不是新物理插入；两后端推进量不同，索引填充程度可能不同。一次性实验补充观察真实初始 membership 变化，但负载偏斜与主实验不同。',
    '- rows/checksum 等包含中止尝试；lock_acquires 是逻辑请求数，不是 CAS 次数。两后端并发调度不同，不要求并发 checksum 相同。',
    '- 这些结果描述当前机器、实现和项目负载，曲线按实测绘制，不据文献曲线调整数据。','',
    '完整原始结果见本目录 `results.csv`、`summary.csv` 和逐次 stdout/stderr/命令 JSON；机器、源码和二进制 SHA256 见 `metadata.json`。补充结果见相邻 `compact-keys-ycsbe-supplement-20261113` 目录。实现说明见 `../../docs/notes/classic-compact-locks-2026-11-13.md`。']
(args.main/'REPORT.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
print(args.main/'REPORT.md')
