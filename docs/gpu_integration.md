# GPU/HBF integration

This branch contains an online GPU request controller above HBF EventLab, a live Accel-Sim/GPGPU-Sim adapter, and a pinned import of helpers from [HBF-Sim](https://github.com/jager197/accel-sim-hbf). The unchanged upstream artifact is also built and validated separately; see [import and verification record](hbf_sim_import.md).

```text
GPU source queues, cycle issue width, outstanding limit
    → GpuPageController: page MSHRs / exact write coverage
    → HostDriver::enqueue: streamed Global page transaction
    → existing DeviceDriver / RX/TX / Bank Cache / NAND
    → Host final logical completion (including retry/replay)
    → MSHR fan-out to original GPU IDs and source partitions
    → capacity release enables the next GPU issue
```

The live path replaces the upstream HBF device at `hbf_system_t`, receives actual `mem_fetch` objects from simulated GPU cores, advances EventLab only to the current DRAM-domain time, and returns the same objects to their original memory subpartitions after final Host completion:

```text
GPU core/L2 request → mem_fetch → hbf_eventlab_bridge
    → GpuPageController → EventLab Host/Device/NAND
    → final completion/replay result → source subpartition → GPU core resumes
```

## Input and CLI

Input starts with `HBF_EVENTLAB_GPU_TRACE 1`:

```text
READ  id gpu_cycle global_byte bytes [source=N] [after=id,id] [expect=content]
WRITE id gpu_cycle global_byte bytes content=N [source=N] [after=id,id]
```

File-trace requests must be 64B aligned, nonempty, at most 4096 bytes, and contained in one Global 4KiB page. Live GPU requests additionally accept 32B sectors and the GPGPU-Sim 128B byte mask. Each source keeps FIFO issue order. GPU dependencies are validated by an iterative topological check and resolved in the controller; live dependencies may refer only to requests already admitted by the adapter.

Run the basic example in the isolated server worktree:

```sh
./build-baseline/hbf_eventlab_gpu_cli examples/gpu_traces/basic.txt \
  --gpu-cycle-ps 500 --issue-width 2 --issue-limit 2 --mshrs 1 --channels 2
```

Use `--fail-page 0` to inject one reusable program failure on channel 0. The Host retains data and performs replay; GPU waiters receive a final result after recovery.

`examples/gpu_traces/merge32.txt` demonstrates 32 cache-line reads sharing one page transaction. Run it with `--issue-width 8 --channels 2`; the expected result includes `mshr_hits=31 page_reads=1`. Adding `--fail-page 0` exercises recovery. `incomplete.txt` intentionally returns exit code 2 with an incomplete-write diagnostic.

| Option | Default | Meaning |
|---|---:|---|
| --gpu-cycle-ps | 1000 | GPU cycle expressed in EventLab 1ps Ticks |
| --issue-width | 2 | Maximum request admissions per GPU cycle |
| --issue-limit | 128 | Maximum unfinished issued GPU requests |
| --mshrs | 16 | Maximum read/active program page entries |
| --write-buffers | 8 | Maximum buffered write pages |
| --write-deadline | 10000000 | Incomplete-write deadline in Ticks |
| --read-window | 0 | Bounded aggregation wait; zero selects demand |
| --no-merge | off | Serialize same-page reads as distinct page transactions |
| --stacks / --channels | 1 / 2 | Number of stacks and channels per stack |
| --dies / --banks | 1 / 1 | Dies per channel and Banks per Die |
| --mapping | interleave | interleave, contiguous, or grouped page placement |
| --group-pages | 2 | Adjacent pages before changing channel in grouped mode |
| --max-tick | 1000000000000 | Retains state on simulated-time limit |

The GPU CLI uses page placement (4096B stripes), not the ordinary Host CLI's 64B stripes. One GPU page therefore maps to one local DLU; two-channel, one-MSHR full-page writes no longer depend on a second Global page to fill the first DLU.

## Read and write semantics

Same-page reads share a live page entry. One backend page transaction serves all current waiters, and final completion is distributed to their IDs and sources. Backend reads remain 64 OCP-like 64B commands, reusing the existing sense/Bank Cache path; no new device read-burst opcode is claimed. Waiting for all 64 responses is a conservative page-completion boundary, and link overhead differs from upstream HBF-Sim.

Writes use the upstream exact-byte coverage algorithm. Repeated stores do not increase coverage. Complete coverage generates one 4096B backend write and acknowledgements follow final program/replay completion. A forced incomplete page reports a strict error; no missing tokens or padding are fabricated. If the issue window is smaller than the number of stores needed to fill a page, the deadline can expose an incomplete-write failure. Data is symbolic, with one content identity for the page assembly; arbitrary partial updates across content versions and read-modify-write are not implemented.

Read-after-write waits for an active full-page write. A read that forces an incomplete assembly to flush records `ReadAfterIncompleteWrite` for its write waiters. Source ordering and explicit dependencies still apply.

The runner checks GPU source queues, MSHRs, buffered writes and owned events in addition to Host and Device activity. A GPU-side strict failure makes the combined workload fail even if no Device command was issued.

## Live Accel-Sim backend

The integration is applied only to a pinned, separate `HBF-Sim-eventlab` checkout. `scripts/build_accel_sim_eventlab.sh` verifies both upstream commits, applies `integration/accel_sim/eventlab_backend.patch` with zero fuzz, builds the EventLab core as PIC, and links it into the simulated `libcudart.so`. Select it with `HBF_BACKEND=eventlab`; omitting the variable retains the upstream controller.

`HBF_EVENTLAB_IMAGE_PAGES=N` selects the first `N` logical image pages. `HBF_EVENTLAB_IMAGE_PAGE_FILE=PATH` adds sparse logical page numbers (one decimal or `0x` value per line, with blank lines and `#` comments allowed), which is useful for replaying model weights mapped high in the HBF address space without describing every preceding page. Duplicate pages are loaded once. The image has two explicit initialization modes. `HBF_EVENTLAB_PRELOAD_IMAGE=1` programs every selected page through the normal Host/Device/NAND write path and event-drives it to quiescence before timed GPU execution. `HBF_EVENTLAB_SEED_IMAGE=1` instead establishes the selected symbolic pages as pre-existing NAND contents without runtime erase/program timing or counters; all subsequent GPU reads still traverse the normal Host/Device/Bank Cache/NAND path. The modes are mutually exclusive. Seed mode is intended for full-model replay where deployment happened before the measured interval; program mode remains the verification path for image-write timing and recovery. Neither mode invents an unwritten page in response to a read. `HBF_EVENTLAB_PAGES_PER_CHANNEL` controls the bounded sparse resident-page budget (default 4096), and `HBF_EVENTLAB_TRACE` writes one `INGRESS`, `COMPLETED`, and `RETURNED` record for every GPU request. `HBF_EVENTLAB_FAIL_ONCE=1` injects one reusable program failure for recovery testing.

The reproducible smoke entry point is:

```sh
bash scripts/build_accel_sim_eventlab.sh /home/server/HBF-Sim-eventlab
bash scripts/smoke_accel_sim_eventlab.sh /home/server/HBF-Sim-eventlab <fresh-run-tag>
```

The smoke suite executes compiled CUDA kernels through GPU cores. It covers 1/2/4 flattened channels, 32B sector ingress, same-page MSHR fan-out, device-latency feedback, strict complete-page stores followed by a second-kernel readback, and Host replay after a program failure. `scripts/check_live_gpu_trace.py` rejects missing, duplicated, reordered, or source/payload-changing lifecycle records.

## Accounting and limits

- GPU issue/completion counts are original requests; Host counts are translated 64B commands and attempts.
- `page_read_bytes` counts backend page payload, including Cache-served transactions. NAND sense traffic is `DEVICE senses × 4096`, not this field.
- Stack channels are flattened into independent EventLab channel IDs. This provides stack-affine routing and separate channel resources, not an imported upstream per-stack FTL/cache controller.
- The CLI remains a finite request-source model. The optional live adapter provides GPU instruction/core execution and 32B/128B-mask conversion through the pinned Accel-Sim build.
- The live adapter flattens stacks and channels into at most 16 EventLab channels and currently uses a sparse one-Die/one-Bank, one-page-per-erase-block backend profile. Upstream per-stack FTL, logic-die cache, wear-leveling policy, explicit placement tables and partial-write compatibility are not imported.
- GPGPU-Sim remains the owner of functional payload bytes. EventLab validates symbolic content identity and controls completion timing; it does not copy NAND payload bytes into the GPU functional memory image. GPU atomics and final device failures have no `mem_fetch` error-response encoding and are rejected/terminate the run.
- EventLab NAND timings, ideal Core Channel and symbolic-content limits remain unchanged. This is not hardware calibration or a numerical reproduction of the paper.
