# GPT-2 统一配置完整重跑（2026-11-10）

本目录保存真实预训练 GPT-2 124M、WikiText-2 输入轨迹在模拟 GPU + 增量 EventLab HBF 上使用统一后端配置后的完整回放结果。实验源码基于上游 `sjtu-epcc/HBF-EventLab` 的 `exp/simulated-gpu-unified-config` 分支提交 `d32959113553ed9b38eb02bb10f3b85cb01f74e5`；该提交直接继承上次已验证实验的 `bec042748246f02cce2d0f848505e8854ffc8bc8`。压缩生命周期轨迹所需的本轮脚本在 `scripts/`，其中 `collect_metrics.py` 和 `audit_trace.py` 仅增加 `.csv.gz` 读取能力，严格审计条件保持不变。

## 与上次结果的对比

| 模拟指标 | 2026-11-07 基线 | 统一配置重跑 | 差值 |
|---|---:|---:|---:|
| TTFT (ms) | 72.99253617021276 | 72.99253617021276 | 0 |
| TPOT (ms/token) | 48.000169030732856 | 48.000169030732856 | 0 |
| 总 GPU 周期 | 305,960,191 | 305,960,191 | 0 |
| 完成的 kernel | 1,480 | 1,480 | 0 |

四个输出 token 的 ID 依次为 `4014, 290, 6260, 13`，结束周期依次为 `102919476, 170216747, 238225156, 305960191`，与基线逐项相同。`comparison.json` 记录精确差值；`summary.json` 和 `per-token.csv` 记录本次结果。输入仍是 WikiText-2 test parquet 第 3 行的 8 个 token，提示文本 ` Robert Boulter is an English film`，生成文本 ` critic and writer.`。推理采用 FP32、batch 1、KV cache 和四个自回归输出 token。

## 验收与配置

- 模拟器 `run.rc=0`，外层 `full-compressed-launcher.rc=0`；1,480 个 kernel 按序完整结束，四个 token 的 kernel 边界为 `343/722/1101/1480`。
- 70,710,912 个 GPU 请求全部 admitted/completed/returned；逐请求生命周期审计 `valid=true`，`failed=owned=0`，Device `drained=1`，页面 `incomplete=0`。
- 权重在计时前经过正常 Host 写入，共 121,551 个 4KiB 页；`seeded_pages=0`。接入代码在预载和 kernel 边界核对 Host、Device 和 GPU 控制器排空。
- GPU 接入层使用 512 个 64B MSHR，死锁检测开启。设备为实验用 16 channel × 每 channel 4 die × 每 die 16 bank；最终输入和生效后的统一配置分别见 `backend-input.ini`、`backend-input.ini.effective.ini`。
- 完整压缩轨迹通过 `gzip -t`，严格 `collect_metrics.py` 校验输入/输出 token、全部 kernel 周期、四个 token 边界和逐请求生命周期后生成结果。源文件、二进制、配置和输入轨迹哈希见 `full-compressed-source.sha256`。
- 运行于 node2 的 `/home/server/hbf-experiments/20261109-gpt2-unified/full-run-unified-gz/`，UTC 开始、模拟结束和审计结束时间见 `full-run-compressed.time`。该墙钟时间不参与 TTFT/TPOT。

`scripts/` 保留本轮准备、短测、完整压缩回放和审计脚本。脚本中的 `/home/server/...` 是该次隔离运行的原始路径；在其他机器重跑时需提供相同模型、WikiText-2、NVBit 捕获和 Accel-Sim 环境，并调整这些路径。源码中的统一配置入口和 GPU 接入变更由本分支直接保存。

与上次公开结果相同，仓库只收录可复核的小型证据。GPT-2 权重、数据集、完整 NVBit 轨迹、模拟器二进制、约 2.2 GB 的 `eventlab.csv.gz` 和约 71 MB 的 `run.log` 保留在 node2，不收入 Git。压缩轨迹 SHA-256 为 `e03a55d67d9114d50de012164f89633a2d3c7b2256eda494eff8ac514c0e5253`。本目录文件的 SHA-256 见 `SHA256SUMS`。

这些 TTFT/TPOT 是轨迹驱动的模拟 GPU 时间；HBF 承载模型权重路径，KV cache 与激活位于模拟 HBM。指标不含分词、模型加载、Host launch 和排队时间，也不代表真实 GPU 或实物 HBF 延迟。
