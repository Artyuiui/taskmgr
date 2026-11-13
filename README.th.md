# taskmgr

[English README](README.md)

Task Manager แบบ Windows สำหรับ terminal บน Linux เขียนด้วย C + ncurses ไฟล์เดียว
อ่านข้อมูลจาก `/proc` ตรงๆ ไม่มี thread, ไม่มี dependency นอกจาก ncurses

- RAM ที่ใช้จริง ~1–2 MB, CPU ~0% ระหว่างรอ refresh
- Binary ประมาณ 40–60 KB

## หน้า Processes

```
 Task Manager  [ Processes ] [ Performance ]              refresh 1s  q:quit
 CPU  12%   Memory  45% (7.1 GB / 15.6 GB)   312 processes   1180 threads   up 2d 03:14
 Name                      PID Status        CPU%      Memory   Thr User
 firefox                  4123 Sleeping       8.3    1.2 GB      92 karin
 code                     3311 Sleeping       2.1  620.4 MB      41 karin
 ...
```

CPU% คิดแบบ Windows คือเทียบกับทั้งเครื่อง (ทุก core รวมกัน = 100%)

## หน้า Performance

กราฟ CPU ย้อนหลัง, bar รายคอร์, Memory/Swap, GPU, Network, Disk I/O ต่อวินาที และความจุ Storage
เลื่อนดูด้วย `↑` `↓` `PgUp` `PgDn` ถ้าเนื้อหายาวกว่าจอ

```
 GPU      56%   NVIDIA GeForce RTX 3060   mem 3.0 GB / 12.0 GB   61°C
  ███████████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░

 Storage
  /          ██████████████████████░░░░░░░░░░░░░░░░░  47%  216.7 GB / 460.4 GB  ext4
  /home      ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  21%  412.0 GB / 1.8 TB    ext4
```

**GPU**
- NVIDIA: โหลด `libnvidia-ml.so.1` (NVML) ตอนรันด้วย `dlopen` ไม่ต้องติดตั้งอะไรเพิ่มถ้ามี driver อยู่แล้ว
  ได้ utilization, VRAM, อุณหภูมิ ถ้าไม่มี NVML จะแสดงแค่ชื่อรุ่นจาก `/proc/driver/nvidia`
- AMD (amdgpu): อ่านจาก sysfs `gpu_busy_percent`, `mem_info_vram_*`, hwmon ได้ครบเหมือนกัน
- Intel / nouveau: แสดงชื่อและอุณหภูมิเท่าที่ sysfs มีให้ (i915 ไม่มี utilization ใน sysfs)
- ไม่มี GPU จะขึ้น `none detected` ไม่มี process ลูกถูก spawn (ไม่เรียก nvidia-smi)

**Storage**
- อ่าน `/proc/mounts` + `statvfs` เฉพาะ filesystem จริง (ext4, xfs, btrfs, ntfs, vfat, zfs ...)
- ตัด loop/snap, tmpfs, bind mount และ btrfs subvolume ที่ซ้ำ device เดียวกันออก

**Sensors / อุณหภูมิ**
- อ่านทุกตัวจาก `/sys/class/hwmon` (coretemp, k10temp, nvme, amdgpu, macsmc บน Apple Silicon ...) ถ้าไม่มี hwmon ใช้ `thermal_zone`
- อุณหภูมิ CPU ขึ้นที่บรรทัดสรุปด้านบนและบรรทัด CPU ของหน้า Performance สีเขียว/เหลือง/แดงตาม <60 / <80 / 80+ °C

## หน้า System (สเปคเครื่อง)

```
 System
   Machine        Apple MacBook Air (M1, 2020)
   OS             Fedora Linux Asahi Remix 40
   Kernel         6.11.3-401.asahi.fc40.aarch64+16k
 CPU
   Model          Apple M1
   Vendor         Apple
   Architecture   ARM64 (aarch64, 64-bit)
   Cores          8 cores · 8 threads: 4× Icestorm @ 2.06 GHz + 4× Firestorm @ 3.20 GHz
   Frequency      now 2.08 GHz   max 3.20 GHz
   Cache          L1d 128K ×8 · L1i 192K ×8 · L2 12M ×2
   Features       AES, NEON, SHA2, SHA3, I8MM, BF16, DotProd, LSE
   Temperature    52°C
 GPU
   GPU 0          Apple GPU (AGX, t8103)   driver apple
 Memory / Storage / Network / Battery ...
```

- **Architecture** บอกชัดว่า `x86_64 (x64, 64-bit)`, `ARM64 (aarch64, 64-bit)`, `ARM (armv7l, 32-bit)`, RISC-V, PowerPC ฯลฯ
- **CPU** x86 (Intel/AMD/Zhaoxin/Hygon) อ่านชื่อรุ่นจาก `/proc/cpuinfo`
  ARM อ่าน device-tree (`/proc/device-tree`) รู้จัก SoC ของ Apple M1–M4, Snapdragon (รวม X Elite), Raspberry Pi, Rockchip, MediaTek, Tegra ฯลฯ
  และแปลง ARM core ID เป็นชื่อ (Cortex-A/X, Neoverse, Apple Firestorm/Icestorm/Avalanche/Blizzard, Qualcomm Kryo/Oryon ...)
- **Cores** แสดง big.LITTLE หรือ P/E core แยกกลุ่มพร้อมความถี่สูงสุด ถ้า core เหมือนกันหมดจะแสดง socket/cores/threads
- **GPU** ทุกตัว: NVIDIA (NVML + driver version), AMD/Intel (ชื่อจาก `pci.ids`), Apple AGX, Adreno, Mali, VideoCore (ผ่าน device-tree) พร้อม VRAM, PCI ID, อุณหภูมิ
- **Storage** รายชื่อ disk จริง (NVMe/SSD/HDD/SD) รุ่นและความจุ, **Network** interface จริงพร้อม IPv4 และความเร็ว link, **Battery** % และสถานะ

## ติดตั้ง

ติดตั้งบรรทัดเดียว (โหลด binary สำเร็จรูป x86_64/aarch64 จาก GitHub Release ถ้าไม่เข้ากับเครื่องจะ build จาก source ให้เอง)

```bash
curl -fsSL https://raw.githubusercontent.com/Artyuiui/taskmgr/main/install.sh | sh
```

หรือ build เองต้องมี ncurses dev headers ก่อน

```bash
# Debian / Ubuntu
sudo apt install build-essential libncursesw5-dev
# Fedora
sudo dnf install gcc make ncurses-devel
# Arch
sudo pacman -S base-devel ncurses
```

```bash
make
./taskmgr
```

ถ้าอยากได้ binary เล็กสุด `make small` ติดตั้งลง `/usr/local/bin` ด้วย `sudo make install`

## คีย์

| คีย์ | ทำอะไร |
|---|---|
| `Tab` / `←` `→` / `1` `2` `3` | สลับแท็บ Processes / Performance / System |
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | เลือก process |
| `Del` หรือ `e` | End task (SIGTERM) ถามยืนยันก่อน |
| `K` | Force kill (SIGKILL) ถามยืนยันก่อน |
| `/` | ค้นหาตามชื่อ, PID หรือ user (`Enter` คงไว้, `Esc` ล้าง) |
| `c` `m` `n` `p` `u` | เรียงตาม CPU / Memory / Name / PID / User (กดซ้ำ = กลับด้าน) |
| `r` | กลับด้านการเรียง |
| `+` / `-` | refresh เร็วขึ้น / ช้าลง (0.25–5 วินาที) |
| `↑` `↓` `PgUp` `PgDn` (หน้า Performance / System) | เลื่อนดูเนื้อหา |
| `q` | ออก |

## ตัวเลือก

```
taskmgr [-a] [-d ms]
  -a      โหมด ASCII ล้วน (terminal ที่ไม่รองรับ unicode)
  -d ms   ช่วง refresh เป็นมิลลิวินาที (ค่าเริ่มต้น 1000)
```

ถ้า locale ไม่ใช่ UTF-8 โปรแกรมจะสลับเป็น ASCII ให้เอง

ตัวแปรแวดล้อม `TASKMGR_NO_NVML=1` ปิดการโหลด NVML ถ้าต้องการ

## ข้อจำกัด

- รองรับ Linux เท่านั้น (อ่าน `/proc` และ `/sys`) บน Apple Silicon ต้องเป็น Linux เช่น Asahi ไม่ใช่ macOS
- Intel iGPU (i915/xe) ไม่มี utilization ใน sysfs จึงแสดงแค่ชื่อและอุณหภูมิ
- ชื่อรุ่น GPU ของ AMD/Intel มาจากไฟล์ `pci.ids` (แพ็กเกจ `hwdata` หรือ `pciutils`) ถ้าไม่มีจะแสดง `AMD GPU (amdgpu)`
