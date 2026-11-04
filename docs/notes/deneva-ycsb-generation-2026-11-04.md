# Deneva 请求数与轻量负载适配

以 .66 实际 Deneva 为准：config.h:397 的 REQ_PER_QUERY=10；system/global.cpp:270 初始化 g_req_per_query；system/parser.cpp:117 支持 -rpqN。此前对话中提到的 -Crq 不准确，应为 -rpq。benchmarks/ycsb_query.cpp 普通 ZIPF/HOT 分支按该数量生成请求，重复 key 重采样补足数量；KEY_ORDER=false 时保留生成顺序。当前开启的 MIX_WORKLOAD/DYNAMIC_WORKLOAD 另有连续访问和插入分支，不能把普通分支等同于整套配置。

适配计划：继续使用现有 operations_per_transaction/--ops，不硬编码十条；加入默认启用的 ycsb_unique_keys，普通 A/B 和本项目 E 扩展均保证请求起始 key 唯一，不排序。false 仅保留重复键专项正确性回归/历史语义。唯一请求数大于 key_count 时立即报错；极端倾斜采样用有界失败避免生成阶段挂死。E 的扫描区间允许重叠，扫描长度与请求数量分离；插入直接使用采样 key，去除旧生成器偏移到尾部导致的聚集。保留已有确定种子 CDF Zipf 采样，不声称与 Deneva 随机序列相同。A/E 比例仍显式遵循本项目 50/50、95/5，不照抄 Deneva 当前 SCAN_PERC=0。

TPC-C 的事务长度不由 --ops 决定。Deneva New Order 在 5..MAX_ITEMS_PER_TXN(15) 之间生成去重 item，Payment 按业务状态机执行；本项目现有两个简化 trace 暂不冒充完整移植，不把它们称为标准 TPC-C。此次不混入业务模型重写。

配置字段属于公共接口，单独提交。实现后检查：请求长度/唯一性/重现性/非法容量/可配置长度/扫描混合，保留重复键读己之写回归；完整真实 RDMA CTest、双机短负载和矩阵入口验证。生成语义变化后的结果必须标新代码版本，不与旧数据合并。
