# 完整 MGL 模式能力（2026-11-07）

Gray1976 Table1 的 IS/IX/S/SIX/X，与 SQL Server 官方 full compatibility chart 的 U/IU/SIU/UIX 扩展分开归因；统一为九种模式。IU 映射中间页/桶意向，不把它视为普通记录读写权限。

旧 IS/IX/S/X wire 值保留，追加 SIX/U/IU/SIU/UIX。兼容矩阵、permission join、缓存满足判断、local/RDMA/native/TCP/object 模式校验同步。S+IX=SIX，S+IU=SIU，U+IX=UIX。转换必须先对最终模式检查他人锁，失败保持原模式；批量 RDMA 同样先求 effective mode。

桶混合扫描/写改为 SIX + 实际写记录 X，允许其他事务在同桶以 IS + 不同记录 S 读取；不再保守地整桶 X。物理叶层级同样在无需结构 X 的已存在记录更新时产生 SIX；分裂/删除结构保护仍可需要 X。

组合父锁对后代记录的直接保护只取 S/U/X 覆盖分量，IX/IU 不作为额外记录锁冲突。纯意向 IS/IX/IU 不直接锁住每条后代记录。

锁槽语义版本4、object magic RRLOBJ02、TCP magic RRLWAIT3；不可混用旧服务。IRangeLockManager 签名不变，本提交是模式能力的接口里程碑，粒度升降另做。

验证：构建成功，真实 RDMA 回环完整 CTest 49/49，无跳过。
日志：experiments/logs/classic-mgl-check-20261107-082821/ctest.log。
独立转录官方九模式矩阵，逐对兼容检查、全部 join 的代数/兼容保序、每个合法 peer 下的动态转换，覆盖本地、远端区间表、本地和远端 object 表。原 gate 内零 new 分配断言保持通过。没有正式性能矩阵。
