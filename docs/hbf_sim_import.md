# HBF-Sim 引入及验证记录

日期：2026-10-13。EventLab 基线为 `014c7838fbebbac44b3d4a82ddfac09784c196ed`，开发分支 `gpu-sim-integration`。

## 原始来源与固定版本

- 论文：[HBF-Sim: An Extensible HBF Simulator for Large-scale GPU Memory Systems](https://arxiv.org/abs/2609.29246)。已核对官方 HTML 第 4 节以及第 5.2 节。
- 作者公开实现：[jager197/accel-sim-hbf](https://github.com/jager197/accel-sim-hbf)，`dev` 提交 `3415a3171d9312fac6a835bc1aa188f7aab685c0`。
- GPU 模拟器固定基线：GPGPU-Sim `6c3cf4ff32110908386d605a7034fc67666a92de`。
- pybind11：`da3bdcad040c1c2da02358e0ff5e281e9e97348a`。
- 保留原版许可证与归属；具体引入文件及改动见 `third_party/hbf_sim/NOTICE.md`。

## 三条已经验证的运行路径

| 路径 | 当前行为 | 与另一条路径的关系 |
|---|---|---|
| 原版 HBF-Sim | Accel-Sim/GPGPU-Sim 执行 GPU 指令/核心模型，原 HBF controller 和 media 返回完成 | 独立参考实现，已经编译和运行 |
| EventLab trace/CLI | 模拟 GPU 请求队列和周期发射，page controller 动态提交到原 EventLab Host/Device | 便于确定性测试前端行为和故障边界 |
| EventLab live backend | Accel-Sim/GPGPU-Sim GPU Core 在线产生 `mem_fetch`，EventLab Device 完成后返回原 source partition | 复用同一 page controller、Host、NAND 和 replay；GPU 发射受到完成反馈 |

在线接口在独立 `/home/server/HBF-Sim-eventlab` 副本中实现；固定上游目录保持原样。它替换 `hbf_system_t` 的设备后端，未整体复制原版 controller、FTL 和逻辑 Die cache。功能数据仍由 GPGPU-Sim 管理，EventLab 使用符号内容并决定完成时间与最终成功/恢复结果。

## node2 实测结果

环境：Ubuntu 22.04.5，GCC 11.4，CUDA 12.6，48 核。新构建与实验均低优先级；原仓库和既有服务未修改。

原版目录：`/home/server/HBF-Sim-upstream-src`。

- `make bootstrap`：通过，18 个补丁路径、16 个 HBF 文件、25 个生成解析器快照及 45 个配置选项核验通过。
- `make build JOBS=4`：通过，产出 `gpu-simulator/bin/release/accel-sim.out` 与 GPGPU-Sim CUDA 拦截库。
- `make test`：通过，10 个 Python trace 工具测试、原版 C++ media/coverage/multistack 测试及 release checks 通过。
- `make reproduce-smoke`：数值 DRAM 基线和 7 个 HBF 实验通过。1/2/4/8/16 Channel 各保全 64 请求/完成；同页 32 请求使用 1 次页读。
- `make trace-smoke`：LUD GPU 指令 trace 通过，158 个 INGRESS、158 个 COMPLETED，15 列 `hbf-trace-v1` 校验无错误；9137 GPU cycles、2 次页读、91 MSHR hits。这一原版 wiring smoke 明确启用 partial-write compatibility，不能当作严格完整页写验证。

原版结果标签：`node2-integration-20261013-01`，保存在 `experiments/10_validation/results/` 与 `experiments/11_trace_smoke/results/` 对应子目录。构建工具 makedepend 解压在 `.local-deps/`，Python 依赖保存在 `.venv/`，没有进行系统级安装。

EventLab 接入目录：`/home/server/HBF-EventLab-gpu-sim`；GPU 集成副本：`/home/server/HBF-Sim-eventlab`。

- Release：204/204 通过，2 并发，6.55 s。
- Debug：204/204 通过，2 并发，83.20 s。
- GPU 专项共 17 项；新增 streaming 动态入口、32B/128B mask 写覆盖和外部时钟不提前执行测试。
- 双 Channel、MSHR=1 的 basic 案例：6 issued、6 completed、0 failed，GPU/Host/Device 都排空。
- merge32 + 单次program失败：33 issued、33 completed、0 failed，31 MSHR hits、1 个 backend页读、1 次NAND sense、64 条失败命令尝试、1 遍replay，所有逻辑工作最终成功。
- live GPU结果标签 `node2-03`：7个CUDA kernel运行全部通过。1/2/4 Channel用例各4请求精确完成并返回；同页256个线程形成32个32B `mem_fetch`，31次MSHR hit和1次sense；把`tR`从20增加到200个DRAM周期后GPU周期由5710增至5950。
- 完整页用例跨两个GPU kernel执行128个32B写和128个32B读，256组`INGRESS/COMPLETED/RETURNED`全部守恒；Device执行1次program、1次sense，1024个32位值读回校验通过。
- 故障用例注入一次program失败，记录64条失败command尝试、2次program、2次erase和1遍Host replay；32个GPU请求最终全部成功，GPU/Host/Device排空。
- Qwen3-1.7B解码trace已从校验归档提取全部1819个kernel。选取真实BF16向量乘法kernel 3834做EventLab模型级smoke：1个稀疏权重页在GPU计时前经正常Host/Device/NAND路径预加载；GPU执行26861 cycles、11776条指令和2个CTA，128个32B请求全部完成并返回，127次MSHR hit、1次页读和1次sense，生命周期校验通过。该证据是单kernel闭环，不是完整1819-kernel decode性能结果。

这些墙钟耗时和模型 Tick 都不是实际 HBF 硬件性能。

## 实际接入的代码

`GpuPageController` 拥有 GPU 请求队列、MSHR、write buffers 和最终源请求结果；`HostDriver::enqueue` 动态接收完整页事务；异步 completion observer 只传最终逻辑结果；`SimulationRunner` 纳入外部 GPU source probes。live adapter 将 `mem_fetch` 地址、source、32B大小和128B byte mask 转换成动态请求，按GPU DRAM时间调用 `EventQueue::run_until`，最终把原对象放回source return queue。Host继续拥有command credit、retention、资源重试和program-fail replay。

## 继续推进的范围

1. 将 upstream controller 的分 Stack FTL、logic-die cache、wear leveling 和调度策略做成可选择实现，并与当前扁平 EventLab profile 做语义对照。
2. 若研究需要端到端数据值/ECC，把当前符号内容扩展为明确的数据快照接口；当前读回数值由GPGPU-Sim功能内存保证，EventLab只控制完成。
3. 做相同输入、不同后端的对照；把当前Qwen3-1.7B单kernel smoke扩展为完整1819-kernel decode，保存输入hash、配置、seed和语义差异。
4. 扩展atomics、最终设备失败的GPU可见错误协议，以及超过16个扁平channel/当前稀疏驻留预算的配置。

模型trace可通过`HBF_EVENTLAB_IMAGE_PAGE_FILE`列出实际触及的稀疏逻辑页，并经正常EventLab写路径显式预编程；这避免为高地址权重预写所有低地址页，但仍不把符号镜像描述成真实模型字节。

这些范围没有因在线接口和smoke通过而自动完成；当前证据支持GPU memory-system/request-path闭环，不代表真实GPU执行或纸面参数的硬件校准。
