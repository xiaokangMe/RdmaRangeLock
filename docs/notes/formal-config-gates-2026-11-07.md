# 正式矩阵配置入库与门禁整理（2026-11-07）

## 问题与处理

正式 runner 要求已跟踪文件相对 HEAD 无改动，并拒绝 include/src/benchmarks/test/experiments/configs 下未跟踪文件。两份 README 的未提交说明和 20 份旧配置使本轮新配置在运行前被拒绝。

将两份 README 校正后提交，20 份 JSON 保持内容及路径不变并入库。保留旧参数是为了追溯既有报告及探测，不改写旧实验依据，也不将它们自动纳入本轮矩阵。不修改 .gitignore，不放宽正式门禁，不使用 --smoke 伪装正式结果。

## 当前正式计划入口

使用 experiments/configs/ycsb_e_skew/ 下七份配置：
- `experiments/configs/ycsb_e_skew/ycsb-a-zipf-0p4.json`
- `experiments/configs/ycsb_e_skew/ycsb-e-skew-hot-ops-0p8-data-0p1.json`
- `experiments/configs/ycsb_e_skew/ycsb-e-skew-hot-ops-0p8-data-0p2.json`
- `experiments/configs/ycsb_e_skew/ycsb-e-skew-hot-ops-0p9-data-0p1.json`
- `experiments/configs/ycsb_e_skew/ycsb-e-skew-hot-ops-0p9-data-0p2.json`
- `experiments/configs/ycsb_e_skew/ycsb-e-skew-uniform.json`
- `experiments/configs/ycsb_e_skew/ycsb-e-zipf-0p4.json`

本轮固定 32 OS 线程、execution_mode=threads、coroutines=1、种子 8、repetition_policy=single-run、10 秒预热和至少 10 秒测量；A/E theta=0.4，E/E-skew scan 宽度为 32；六种不同锁协议、三种等待策略，层级桶宽展开 16/64/256。本地 175、跨主机 168 个主 case，共 343 个，正确性及容量门禁另计。参数定义见 [负载说明](ycsb-e-skew-2026-11-07.md)。

## 保留的历史和探测配置

以下配置不是本轮正式入口。它们保留 coroutines=4、旧协议维度、旧 case 计数等历史参数；部分 runs=1 文件也没有本轮要求的 repetition_policy=single-run。入库不表示这些文件在当前 runner 中已通过正式计划校验，更不表示生成过正式结果。不要通过修改旧配置重标旧数据。

| 配置 | 用途 | 原内容 SHA256 |
|---|---|---|
| `cross_host_all8_z000.json` | 历史偏斜率配置 | `5556b0606e53e52f0c44bea42446dcff3be9d5b25b8756f3312b20e6de8f72f8` |
| `cross_host_all8_z040.json` | 历史偏斜率配置 | `47a236f2a2916cb5527297f21c15ce595fef45324166787695140a40720fd18b` |
| `cross_host_all8_z050.json` | 历史偏斜率配置 | `6ca9b0368bfb79b02b76a95ca05278ae0bc02cceebe28d53814775eaabadf7cb` |
| `cross_host_all8_z070.json` | 历史偏斜率配置 | `d2730a67e1b748e1c98a03e00ba156fa938251dfaeec8f2ccdf3118fb04e2ea1` |
| `cross_host_all8_z090.json` | 历史偏斜率配置 | `8e66fcc69209a5d06f29ae1ebeb84d4b662dd70a950159fb75c321464d8f4efe` |
| `formal_native_record_gap_2m_10s.json` | 历史候选配置 | `10cc322491741fd2dd8b8a72cbe3f7b47ef94cdf5abf0e3f218308febf2d001d` |
| `local_bucket_t32_pred_z000.json` | 历史偏斜率配置 | `53a9ce36ddeb9c230dc7671cb9e6293b1fd33cd7d41a3dbbc69a8006a1e3e184` |
| `local_bucket_t32_pred_z040.json` | 历史偏斜率配置 | `bc8ae6582714e3ab29ee6b7aaaf7a71627eafe8347e0cda174d415279e3a5e18` |
| `local_bucket_t32_pred_z050.json` | 历史偏斜率配置 | `1c2e139e3d1cc3152042aae2e3cff8a93dbd61ba2762127a36bf374fc87e4e82` |
| `local_bucket_t32_pred_z070.json` | 历史偏斜率配置 | `ff71f0fe64586a71dab82b445b33dd614f16c6b9950b2eb218d4344fee42018d` |
| `local_bucket_t32_pred_z090.json` | 历史偏斜率配置 | `0c035aea0a3ffa3df907ae4f97086432d299bd3ca098c5bbb4d1492f5acf84da` |
| `local_bucket_t32_z000.json` | 历史偏斜率配置 | `58cb0cfbf2361ffb2f8ab0e347db84dfe9b3bf8e426b31b9a81532e97f62c8a9` |
| `local_bucket_t32_z040.json` | 历史偏斜率配置 | `4c98928e0bf3c31b41071f3e039d17018ff9d7b487d68d3a4e52125cba70b644` |
| `local_bucket_t32_z050.json` | 历史偏斜率配置 | `414c610c8045b516cfc55be74e261132fae8683cbc59ed784fcefca46475fcc0` |
| `local_bucket_t32_z070.json` | 历史偏斜率配置 | `3d92116fd2b4bca2237869311fe14efbb70340c99d865b9d7d2572cfbe4f3ff8` |
| `local_bucket_t32_z090.json` | 历史偏斜率配置 | `582387fdb49f0d136b62c5abcb83feb28247a049a3007e454ece05fb2566bbbe` |
| `probe_b16_local.json` | 探测配置 | `efeddda90c8700abe1a76c3268a522ed09e5c630be3a70f4190fae2c85096802` |
| `probe_nextkey_local.json` | 探测配置 | `ef2adae8ece2c7ded05ff9c7ed10105b4e499e12f211494dbf3846f1ee444bd6` |
| `probe_rdma_gap.json` | 探测配置 | `09b3b4bf90ad42e17c030f0c97e3e3dafacdca71e68aa4f9377634ac5618b45f` |
| `probe_rdma_predicate.json` | 探测配置 | `f8f88a357bd1a5302ad18f758d9f0c3c71773d23a02c77ddf6b39ad9d1c44ddf` |

## preflight-only 的含义

--preflight-only 不是静态检查：非 smoke 模式仍会先构建、固定二进制、运行正确性检查，在 cross-host 模式下启动共享服务并执行容量负载，然后在主矩阵之前退出。因此保留其提交、配置、正确性与容量门禁，不能为了进入预检而跳过可追溯性检查。--mode local --preflight-only 不会验证远端容量。

本次仅解析 JSON、检查文档与 Git 门禁条件，未启动 runner、构建、CTest、容量预检或矩阵。未验证吞吐、锁槽容量和运行时正确性。此前 53/53 结果不作为本轮整理的运行验证。

## 验证范围

检查 20 份旧 JSON 的内容哈希不变；七份本轮配置的线程/协程/等待策略/重复策略/测量时长符合用户要求，静态展开数为每组本地 25、跨主机 24；提交后以 runner 相同的 Git 查询确认两个工作区门禁条件均已清除。其他未跟踪图表、原始数据与绘图脚本不纳入本次提交。
