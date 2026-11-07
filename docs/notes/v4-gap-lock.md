# v4 查询区间 Gap Guard + 记录锁

> 2026-11-07 校正：当前 gap-lock 是项目保守变体，不是 InnoDB gap 协议或真实邻接键区间复现。详见 [复现审计](paper-reproduction-audit-2026-11-07.md)。

## 当前顺序

范围读先取得 (lo,hi)、point(lo)、point(hi) 三把 Gap S（合起来覆盖 [lo,hi]），随后枚举结果、批量申请 Record S，再复扫读取锁后值。旧笔记的“先枚举再拿 Gap 锁”描述已过时。插入拿 Gap X point；更新和删除拿 Record X point；跨 kind 仍按数值重叠检查，所以查询 guard 也排斥覆盖内的已有记录写。写集合延迟到提交应用，锁在事务结束统一释放。

## 论文依据与待补机制

Eswaran1976 §3 允许用覆盖访问集的谓词持锁，提供当前保护的安全依据。Graefe2010 §5 对 record/gap 分离提供目标机制。MySQL 8.4 §17.7.1 的 gap S/X 可以共存、只抑制插入，配合 insert intention；与当前普通 S/X 几何冲突规则不同。

后续必须按复现审计 P0 明确所选 gap 协议，使用真实相邻键、定义插入/删除边界和分裂继承；不能只换名字或加引用。纯 gap 不保护记录值，仍需 record 锁；不得为追求更高并发而漏锁。

## 验证与统计

test/v4_gap_lock_test.cpp 验证当前 guard 阻止范围内点写；test/policy_scan_batch_test.cpp 检查锁后值、删除窗口与清理。统一驱动可切换三种冲突策略。no-wait 中止数不等于实际阻塞时间；正式报告还应分开冲突、容量、gate 拒绝并报告 committed 吞吐。
