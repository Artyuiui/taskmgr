# taskmgr

A Windows-style Task Manager for the Linux terminal. One C file, ncurses, reads `/proc` and `/sys` directly.

- **Tiny footprint**: ~2 MB RSS, ~0 % CPU between refreshes, ~90 KB binary, no threads, no child processes
- **Processes** tab: name, PID, status, CPU %, memory, threads, user. Sort, search, End task / Force kill
- **Performance** tab: CPU history graph and per-core bars, memory, swap, GPU, temperatures, network and disk I/O, storage capacity
- **System** tab: machine model, OS, kernel, CPU (vendor, model, architecture, cores, frequency, cache, instruction sets), GPUs, RAM, disks, network interfaces, battery
- Works on **x86_64 and ARM** (Intel, AMD, Apple Silicon under Asahi Linux, Snapdragon, Raspberry Pi, Rockchip, ...), with NVIDIA, AMD, Intel, Apple, Adreno and Mali GPUs

[ภาษาไทย / Thai README](README.th.md)

## Install

One line, any Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/Artyuiui/taskmgr/main/install.sh | sh
```

The installer downloads a prebuilt binary for x86_64 or aarch64 from the [latest release](https://github.com/Artyuiui/taskmgr/releases/latest), verifies its SHA-256 and installs it to `/usr/local/bin` (or `~/.local/bin` without root). On systems the prebuilt binary does not fit (Alpine/musl, armv7, riscv64, very old glibc) it installs the compiler and ncurses headers with your package manager and builds from source instead.

Options via environment variables:

```bash
TASKMGR_INSTALL_DIR=~/bin  sh install.sh     # custom location
TASKMGR_FROM_SOURCE=1      sh install.sh     # always build locally
TASKMGR_VERSION=v0.1.0     sh install.sh     # pin a release
```

### Build from source

```bash
# Debian / Ubuntu
sudo apt install build-essential libncursesw5-dev
# Fedora
sudo dnf install gcc make ncurses-devel
# Arch
sudo pacman -S base-devel ncurses
# Alpine
sudo apk add build-base ncurses-dev
```

```bash
git clone https://github.com/Artyuiui/taskmgr.git
cd taskmgr
make
./taskmgr
sudo make install        # -> /usr/local/bin/taskmgr
```

`make small` produces the smallest possible binary.

## Processes

```
 Task Manager  [ Processes ] [ Performance ] [ System ]         refresh 1s  q:quit
 CPU  12% 52°C   Memory  45% (7.1 GB / 15.6 GB)   312 processes   1180 threads   up 2d 03:14
 Name                      PID Status        CPU%      Memory   Thr User
 firefox                  4123 Sleeping       8.3    1.2 GB      92 arty
 code                     3311 Sleeping       2.1  620.4 MB      41 arty
 ...
```

CPU % is measured the Windows way: relative to the whole machine, so all cores together add up to 100 %.

## Performance

```
 CPU      62%   8 cores   load 1.25 0.80 0.55   2 running   52°C
  ▂▃▅▇█▆▄▃▂▁▂▄▆█▇▅▃▂▁▁▂▃▅▇█▆▄▃▂   (history graph)
   0 ███░░░░░░░  27%   1 █████░░░░░  45%   2 ██████░░░░  64%   3 ████████░░  82%

 Memory   45%   7.0 GB / 15.6 GB   cached 4.6 GB
 Swap      5%   94.9 MB / 2.0 GB
 GPU      56%   NVIDIA GeForce RTX 3060   mem 3.0 GB / 12.0 GB   61°C
 Sensors
  coretemp     Package id 0 52°C   Core 0 50°C   Core 1 51°C
  nvme         Composite 38°C
 Network        ↓ 1.2 MB/s     ↑ 41.3 KB/s     total ↓ 4.7 GB / ↑ 1.1 GB
 Disk I/O       read 105 KB/s     write 4.1 MB/s
 Storage
  /            ██████████████████████░░░░░░░░░░░░░░░░░░  47%  216.7 GB / 460.4 GB  ext4
  /home        ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  21%  412.0 GB / 1.8 TB    ext4
```

Scroll with `↑` `↓` `PgUp` `PgDn` when the content is taller than the terminal.

**GPU**

| Vendor | Source | Shows |
|---|---|---|
| NVIDIA | NVML (`libnvidia-ml.so.1`, loaded at runtime with `dlopen`, no build dependency, `nvidia-smi` is never spawned) | name, utilization, VRAM, temperature, driver version |
| AMD (amdgpu) | sysfs `gpu_busy_percent`, `mem_info_vram_*`, hwmon | name (from `pci.ids`), utilization, VRAM, temperature |
| Intel (i915 / xe) | sysfs | name (from `pci.ids`), temperature where exposed |
| Apple AGX, Qualcomm Adreno, ARM Mali, Broadcom VideoCore, Tegra | device tree | name |

**Temperatures** come from every `/sys/class/hwmon` chip (coretemp, k10temp, nvme, amdgpu, macsmc on Apple Silicon, ...), with `thermal_zone` as a fallback. The CPU temperature shown in the header prefers `Package id 0` / `Tctl` / `Tdie`, then any CPU chip, then a label containing "CPU".

**Storage** lists real filesystems only (ext4, xfs, btrfs, ntfs, vfat, zfs, ...) from `/proc/mounts` + `statvfs`, skipping tmpfs, snaps/loop devices, bind mounts and duplicate btrfs subvolumes.

## System

```
 System
   Machine        Apple MacBook Air (M1, 2020)
   OS             Fedora Linux Asahi Remix 40
   Kernel         6.11.3-401.asahi.fc40.aarch64+16k
   Uptime         1d 10:17
 CPU
   Model          Apple M1
   Vendor         Apple
   Architecture   ARM64 (aarch64, 64-bit)
   Cores          8 cores · 8 threads: 4× Firestorm @ 3.20 GHz + 4× Icestorm @ 2.06 GHz
   Frequency      now 2.08 GHz   max 3.20 GHz
   Cache          L1d 128K ×8 · L1i 192K ×8 · L2 12M ×2
   Features       AES, NEON, SHA2, SHA3, I8MM, BF16, DotProd, LSE
   Temperature    52°C
 GPU
   GPU 0          Apple GPU (AGX, t8103)   driver apple
 Memory
   RAM            15.6 GB   (available 8.6 GB)
 Storage
   nvme0n1        232.9 GB   NVMe SSD    APPLE SSD AP0256Q
 Network
   wlan0          Wi-Fi     up           192.168.1.23
 Battery
   Charge         87%   Discharging
```

- **Architecture** is spelled out: `x86_64 (x64, 64-bit)`, `ARM64 (aarch64, 64-bit)`, `ARM (armv7l, 32-bit)`, RISC-V, PowerPC, ...
- **x86** CPUs (Intel, AMD, Zhaoxin, Hygon) are read from `/proc/cpuinfo`; features include SSE4.2, AVX, AVX2, AVX-512, FMA, AES, SHA, AMX, plus VT-x / AMD-V and whether you are inside a VM.
- **ARM** CPUs are identified from the device tree: Apple M1–M4 (all Pro/Max/Ultra variants), Snapdragon from 410 to X Elite, every Raspberry Pi, Rockchip, MediaTek, NVIDIA Tegra, Amlogic, Allwinner, and more. Core IDs are translated to names (Cortex-A/X, Neoverse, Apple Firestorm/Icestorm/Avalanche/Blizzard, Qualcomm Kryo/Oryon, Samsung Exynos M, ...).
- **big.LITTLE / P+E** designs are shown per core type with their max frequency; homogeneous CPUs show sockets · cores · threads.

## Keys

| Key | Action |
|---|---|
| `Tab` / `←` `→` / `1` `2` `3` | Switch tab: Processes / Performance / System |
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | Select a process, or scroll Performance / System |
| `Del` or `e` | End task (SIGTERM), asks for confirmation |
| `K` | Force kill (SIGKILL), asks for confirmation |
| `/` | Search by name, PID or user (`Enter` keeps the filter, `Esc` clears it) |
| `c` `m` `n` `p` `u` | Sort by CPU / Memory / Name / PID / User (press again to reverse) |
| `r` | Reverse sort order |
| `+` / `-` | Refresh faster / slower (0.25–5 s) |
| `q` | Quit |

## Options

```
taskmgr [-a] [-d ms] [-v]
  -a      ASCII only (for terminals without Unicode)
  -d ms   refresh interval in milliseconds (default 1000)
  -v      print version and exit
```

taskmgr switches to ASCII automatically when the locale is not UTF-8. Set `TASKMGR_NO_NVML=1` to skip loading NVML.

## How it stays light

- Everything comes from `/proc` and `/sys`: no D-Bus, no `lshw`, no `nvidia-smi`, no shelling out.
- One sample per refresh; the loop sleeps in `getch()` with a timeout, so idle CPU is essentially zero.
- Process arrays are reused between frames; the only per-frame allocations are growth of those arrays.
- Static tables for SoC and core names cost a few KB of read-only data and no runtime work.

## Limitations

- Linux only. On Apple Silicon this means Linux (e.g. Asahi), not macOS.
- Intel integrated GPUs do not expose utilization in sysfs, so only the name and temperature are shown.
- AMD / Intel GPU marketing names come from `pci.ids` (package `hwdata` or `pciutils`); without it you get `AMD GPU (amdgpu)`.
- The prebuilt release binaries are glibc-linked and need `libncursesw.so.6`; the installer falls back to a source build everywhere else.

## License

[MIT](LICENSE)
