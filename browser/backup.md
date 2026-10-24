# OECT: how it boots, and the SSD + eMMC rescue plan

The OECT (`root@192.168.10.15`) was checked on 2026-10-24 with the SSD not yet installed. Everything below
was read from the box itself: its eMMC, its U-Boot binary and environment, its kernel and its initramfs.
The few things that are inferred and not read are marked as such.

**The plan in five lines:**
1. The OECT's U-Boot **can't read SATA**. So the kernel always loads from the eMMC, and the SSD can never
   boot on its own.
2. The eMMC keeps the bootloader and `/boot`, plus a small **rescue Armbian**. The SSD holds the **main
   root file system** and `/var/hqnode`.
3. A 20-line **initramfs script** picks the root: the SSD when it's there, otherwise the eMMC. Pull the SSD
   out and the box comes up in the rescue system, with no other change.
4. `oect-boot emmc` keeps the rescue system booting **even with the SSD plugged in**, so you can repair or
   restore the SSD from it.
5. Every night the main system **mirrors itself onto the eMMC** (`/srv/ssd-root`), so a new SSD can be
   restored from the rescue system.

---

## 1. The box, as checked

| | Found | Notes |
|---|---|---|
| Board | `OEC BOX WXY4`, DT `rockchip,rk3566-oec-box-wxy4` | Booted with `rk3566-wxy-oec-turbo-4g.dtb` |
| SoC | RK3566, 4× Cortex-A55, up to **1.8 GHz** (`schedutil`) | |
| RAM | **4 GB** (3.8 GiB in Linux, 288 MiB of it CMA) | The browser plan assumed 2 GB |
| eMMC | 8 GB Samsung-style `8GTF4R`, HS200, `mmcblk0` | Plus `boot0`/`boot1` (4 MiB each, empty, unused) and RPMB |
| SATA | One port, `sata@fc800000` (AHCI, 6 Gb/s); driver `ahci-dwc` **built into the kernel** | Link down: no disk yet |
| SD card | Controller `mmc@fe2b0000` **disabled**: no slot | |
| USB | One USB 3 host, plus USB 2 hosts | The USB 3 lanes of the OTG port are taken by SATA |
| OS | Armbian 26.08 (Debian 13 trixie) | An **ophub** build, not Armbian's own |
| Kernel | `6.1.141-rk35xx-ophub`, not from apt | Updated with ophub's `armbian-update`. No `linux-image` or u-boot package is installed, so apt can't touch the boot chain |
| `initramfs-tools` | 0.148.4, **held** (`hi`) | |
| Network | `eth0` 192.168.10.15/24 by DHCP (NetworkManager), on the home LAN | |
| Watchdog | `/dev/watchdog0` (DesignWare), `watchdog.service` running | |

---

## 2. Does `remote-browser-plan.md` work on this box?

**Yes. Nothing found blocks it.** The hardware unknowns are still the ones the plan lists for build step 1
(the encoder at 880×1808 from BGRA). That test needs no SSD and can be run on the eMMC today.

| Plan requirement | On the box | Verdict |
|---|---|---|
| Vendor kernel 6.1, ≥ 5.19 | 6.1.141-rk35xx (ophub) | ✓ |
| `/dev/mpp_service`, `/dev/dma_heap/*` | `mpp_service` 240:0; heaps `system`, `system-uncached`, `reserved`; all `root:root 0600` | ✓ hqnode's `rkmpp` option makes its own nodes, so the host's mode doesn't matter |
| H.265 encoder | `rkvenc@fdf40000` (`rkv-encoder-v1`, H.264/H.265) and `vepu@fdee0000` (VEPU2) both `okay`; `CONFIG_ROCKCHIP_MPP_RKVENC=y` | ✓ (the BGRA and 1808-line questions are still open) |
| `/dev/rga` | present, 10:123 | ✓ |
| cgroup v2 | `cgroup2fs`; controllers `cpuset cpu io memory pids rdma misc` | ✓ |
| Idmapped mounts (hqnode) | **Tested:** an idmapped bind on ext4 shows uid 100000; overlay on that idmapped lower works | ✓ |
| User namespaces | `CONFIG_USER_NS=y`, `max_user_namespaces` 14335 | ✓ |
| nftables + cgroup match | `CONFIG_NF_TABLES=y`, `CONFIG_NFT_SOCKET=m` | ✓ in the kernel; the `nftables` package isn't installed yet |
| `passt` | apt has **no package lists** (`/var/lib/apt/lists` is missing) | Run `apt update` first; Debian 13 ships `passt` |
| zram | `CONFIG_ZRAM=y`; `zram0` exists, size 0 | ✓ |
| `uinput` (fallback) | `CONFIG_INPUT_UINPUT=y` | ✓ |

**Corrections the browser plan needs:**
- **RAM is 4 GB, not 2 GB.** The container can have about 2.5–3 GB instead of 1.5 GB. The `light`
  screen and one tab are still right, because the CPU is the limit.
- **Storage changes with this plan.** The SSD carries the root file system as well. `/var/hqnode` is the
  SSD's **second** partition (`LABEL=hqnode`, `/dev/sda2`), not `/dev/sda1` on an otherwise empty SSD.
  `RequiresMountsFor=/var/hqnode` stays.
- The kernel is ophub's rk35xx 6.1, not Armbian's. It does the same job, but updates come from
  `armbian-update` (section 5.5).
- Trim the host before hqnode goes on it. `rpcbind` (listening on port 111), `nfs-client`, `openvpn`,
  `wpa_supplicant` and `vnstat` are enabled and not needed.
- The last shutdown wasn't clean: the kernel log shows `EXT4-fs (mmcblk0p2): recovery complete`. The
  current root uses `data=writeback` and `commit=600`, so up to 10 minutes of writes can be lost and
  files can end up with stale contents after a power cut. The SSD root uses ext4's defaults instead
  (section 5.2).

---

## 3. How the box boots, step by step

```
power on
  │
  ▼
① BootROM (mask ROM inside the RK3566; can't be changed)
  │   looks for a boot loader on: SPI flash (none) → eMMC (found) → SD (no slot) → else USB "Maskrom" mode
  ▼
② idblock, eMMC sector 64:  DDR init  →  SPL
  │   DDR init trains the 4 GB of RAM; SPL then loads the FIT image from eMMC 8 MiB
  ▼
③ FIT image (uboot.itb), eMMC 8 MiB:  ATF (BL31)  +  OP-TEE (BL32)  +  U-Boot (BL33)  +  U-Boot's DT
  │   ATF stays resident as the secure monitor (PSCI: CPU on/off, reboot); U-Boot runs next
  ▼
④ U-Boot 2017.09 (Rockchip vendor tree)
  │   environment from eMMC 144 MiB → bootcmd → USB stick with extlinux.conf?  ── yes ─▶ boots the USB stick
  │                                            └─ no → sysboot mmc 0:1 /extlinux/extlinux.conf
  │   loads /Image, the initrd and the .dtb from eMMC p1 into RAM, fixes up the DT, jumps to the kernel
  ▼
⑤ Linux 6.1 kernel
  │   built-in drivers come up (SATA at ~6.5 s, eMMC at ~14.6 s); unpacks the initramfs into RAM; runs /init
  ▼
⑥ initramfs (initramfs-tools)
  │   udev → wait for root=UUID=… → fsck → mount it on /root → switch to it
  ▼
⑦ systemd from the root file system
      /etc/fstab → /boot, /tmp → network, ssh, … → ready ~19 s after power on
```

### 3.1 What is on the eMMC, byte by byte

The eMMC's user area is one 7.28 GiB disk (15 269 888 sectors of 512 bytes) with a **GPT** partition
table. The first 180 MiB are **not in any partition**: that's where the boot loader lives, at offsets
fixed by the BootROM and by U-Boot.

| Sector | Offset | Size | What | Who reads it |
|---|---|---|---|---|
| 0 | 0 | 512 B | Protective MBR (`55aa`) | Tools, so that old ones don't treat the disk as empty |
| 1 | 512 B | 512 B | GPT header (`EFI PART`), disk GUID `86CDFFFF-…` | U-Boot, Linux |
| 2–33 | 1 KiB | 16 KiB | GPT partition entries | U-Boot, Linux |
| **64** | **32 KiB** | 309 KiB | **idblock**: header `RKSS`, then DDR init (`DDR Version V1.13 20220218`) and the SPL | BootROM |
| 1088, 2112, 3136, 4160 | +512 KiB each | 309 KiB each | Four identical backup copies of the idblock | BootROM, if a copy's check fails |
| 7168 | 3.5 MiB | 64 KiB × 4 slots | **Vendor storage** `RKVD` (3 copies in use): serial number, MAC address and other factory data | U-Boot, the kernel's vendor-storage driver |
| **16384** | **8 MiB** | 1.96 MiB | **`uboot.itb`**: a FIT image holding ATF, OP-TEE, U-Boot and U-Boot's DT; sha256 hashes and an rsa2048 signature (key `dev`) | SPL |
| 20480 | 10 MiB | 1.96 MiB | Identical backup copy of `uboot.itb` | SPL, if the first fails its hash check |
| **294912** | **144 MiB** | 64 KiB | **U-Boot environment in use**: CRC32 + `name=value\0…` | U-Boot |
| 295040 | 144 MiB + 64 KiB | 64 KiB | A second, older environment: the box's factory (Xunlei) one, `bootcmd_emmc=sysboot mmc 0:7 …` | Nothing now (see 3.5) |
| 368640 | 180 MiB | 511 MiB | **p1** `BOOT`, ext4, UUID `4352928f-…` → `/boot` | U-Boot (reads files from it), Linux |
| 1417216 | 692 MiB | 6.6 GiB | **p2** `ROOTFS`, ext4, UUID `94665fdf-…` → `/` | Linux |

> **Never** write to sectors 0–368639 by hand, and never `dd` a disk image onto `mmcblk0` without knowing
> what it contains. The vendor storage holds the box's own MAC and serial number, and it isn't in any
> image you can download. A copy of these first 180 MiB was taken on 2026-10-24 (step 0 in section 8).

### 3.2 ① The BootROM

The BootROM is code burnt into the SoC; it runs first and can't be changed. It initializes just enough
hardware to read storage. On the RK3566 it tries, in order: SPI NOR, SPI NAND, **eMMC**, SD card, and
finally, if none of them holds a valid loader, it waits on USB as a **Maskrom** device. In Maskrom mode a
PC with `rkdeveloptool` can write the eMMC directly. That's the last-resort recovery, and it works even
if everything on the eMMC is erased.

On the eMMC it reads the **idblock** at sector 64, falling back to the copies at +512 KiB. The idblock has
a header (`RKSS`: Rockchip's v2 format, as on RK3568/RK3588) followed by two programs. The BootROM loads
each into the SoC's small internal SRAM and runs it.

### 3.3 ② DDR init and SPL

- **DDR init** (Rockchip's closed `rk3566_ddr` blob, V1.13). The 4 GB of RAM doesn't work until the memory
  controller is trained for these chips, so this runs from SRAM. When it returns, RAM is usable.
- **SPL** (U-Boot's *Secondary Program Loader*). It runs from RAM, reads the **FIT image** at eMMC 8 MiB
  (or the copy at 10 MiB), checks each part's sha256, and places them:

  | FIT part | Size | Loaded at | What it is |
  |---|---|---|---|
  | `atf-1` … `atf-6` | 160 KiB + small pieces | 0x40000, and SRAM at 0xfdcc… | **ARM Trusted Firmware, BL31**: runs at EL3, the CPU's most privileged level, and stays resident. It's the "PSCI firmware" the kernel talks to for CPU on/off, suspend and reboot (`psci: PSCIv1.1 detected in firmware`) |
  | `optee` | 446 KiB | 0x8400000 | **OP-TEE, BL32**: a small OS in the secure world. Not used by Armbian, but it holds its memory |
  | `uboot` | 1.2 MiB | 0xa00000 | **U-Boot proper, BL33** |
  | `fdt` | 17 KiB | (with U-Boot) | U-Boot's own device tree: `rockchip,rk3568-evb` |

  SPL jumps into ATF, which sets up the secure world and then drops to U-Boot at a lower privilege
  level.

The FIT also carries an RSA signature. The SoC enforces it only if secure boot has been fused on, which
can't be read from Linux here. That's one more reason **not to rebuild or patch this U-Boot**: if the box
does enforce it, a modified one won't start, and only Maskrom gets it back.

### 3.4 ④ U-Boot: the environment

U-Boot's behaviour is decided by its **environment**, a list of variables, some of them small scripts. On
start, U-Boot reads 64 KiB from eMMC offset 144 MiB and checks its CRC32:
- **CRC good:** it uses that environment.
- **CRC bad:** it prints `bad CRC` and falls back to its built-in default. That default is
  `bootcmd=boot_fit`, the factory Android-style boot, **which can't boot Armbian**. A damaged environment
  therefore means a box that doesn't boot (recover with section 7.5). Nothing writes the environment
  unless you run `saveenv` at U-Boot's prompt or a `fw_setenv` tool, and this plan uses neither.

The environment in use (as read from the eMMC), reduced to what runs:

```
bootdelay=1                       # 1 s to press a key on the serial console (1 500 000 baud) and get a prompt
bootcmd=run extlinux_env; if test ${enabled_usbboot} -eq 1; then usb start; run detect_usb; fi; run bootcmd_emmc
extlinux_env=setenv fdt_addr_r 0x12000000; setenv kernel_addr_r 0x02000000; setenv ramdisk_addr_r 0x12180000
enabled_usbboot=1
detect_usb=if test -e usb 0 extlinux/extlinux.conf; then run bootcmd_usb0; fi
bootcmd_emmc=sysboot mmc 0:1 any 0x00c00000 /extlinux/extlinux.conf
```

So, in order:
1. **`extlinux_env`** sets where in RAM the kernel (32 MiB), the device tree (288 MiB) and the initrd
   (289.5 MiB) are loaded.
2. **USB first.** `usb start` powers the USB ports and scans them. If **USB disk 0** has
   `extlinux/extlinux.conf` on its first partition, U-Boot boots *that* system and never looks at the
   eMMC. This is useful (a USB stick is a rescue path, section 7.5) and a hazard: any USB disk with an
   `extlinux.conf` takes over the box at the next reboot.
3. **eMMC.** `sysboot mmc 0:1` reads `/extlinux/extlinux.conf` from eMMC partition 1 (`/boot` in Linux).
   `mmc 0` is the eMMC; `mmc 1` would be the SD slot, which this box doesn't have.

**What U-Boot can and can't see.** The U-Boot binary contains SATA, SCSI and NVMe code, but its device
tree (`rk3568-evb`) has **no SATA node and no combo PHY**. The nodes it does have: eMMC, the two dwmmc
controllers, USB (two DWC3 + USB 2 PHY), Ethernet, PCIe, SPI flash and NAND. So U-Boot can read the eMMC
and USB disks, but **not a disk on the SATA port**. This one fact shapes the whole plan (section 4).

### 3.5 ④ U-Boot: `sysboot` and `extlinux.conf`

`sysboot` is U-Boot's reader for the `extlinux.conf` format (the same as PXELINUX). Today's file:

```
LABEL Armbian
  LINUX /Image                                       → /boot/Image → vmlinuz-6.1.141-rk35xx-ophub (44 MB)
  INITRD /uInitrd                                    → /boot/uInitrd → initrd.img-6.1.141-rk35xx-ophub (19 MB, xz)
  FDT /dtb/rockchip/rk3566-wxy-oec-turbo-4g.dtb
  APPEND root=UUID=94665fdf-… rootflags=data=writeback rw rootwait rootfstype=ext4
         earlycon=uart8250,mmio32,0xfe660000 console=ttyS2,1500000 console=tty1 … fsck.repair=yes …
```

U-Boot loads the three files from p1 (following the symlinks: its ext4 reader handles them). It sets the
kernel command line to `APPEND`, applies Rockchip's fixups and starts the kernel with `booti`:
- It writes the RAM size into the device tree's `memory` node.
- It records where the initrd sits (`/chosen/linux,initrd-start` and `-end`).
- It changes the console: `extlinux.conf` says `console=ttyS2,1500000`, but the kernel received
  `console=ttyFIQ0`, Rockchip's FIQ debugger on the same UART.

Two details you'll trip over if you don't know them:
- **`/boot/boot.scr`, `/boot/boot.cmd` and `/boot/armbianEnv.txt` are not used.** They're generic
  leftovers in ophub's image (they mention Amlogic and Odroid). The proof is in the kernel's command line:
  `boot.scr` would add `cgroup_enable=memory` and `ubootpart=…`, and neither is there. **Editing
  `armbianEnv.txt` does nothing; `extlinux.conf` is the file that counts.**
- **`/boot/uInitrd` is a symlink to the plain xz initrd**, not to the `mkimage`-wrapped
  `uInitrd-6.1.141-rk35xx-ophub` beside it. The hook `/etc/initramfs/post-update.d/99-uboot` re-points
  the link to the wrapped file on every `update-initramfs`. It isn't known whether `sysboot` on this
  U-Boot accepts the wrapped form. So this plan's `extlinux.conf` names the plain file,
  `/initrd.img-<version>`, which is known to boot.

### 3.6 ⑤ The kernel

From the kernel log of the current boot:

| Time | What |
|---|---|
| 5.06 s | `Booting Linux`. The clock started at power on, so about 5 s went to the BootROM, DDR init, SPL and U-Boot (1 s of it is `bootdelay`) |
| 5.23 s | The command line, as U-Boot passed it |
| 5.34 s | `Memory: 3670000K/4175872K available` |
| 6.22 s | Unpacks the initramfs (xz, 1422 files) into a RAM file system |
| 6.55 s | `ahci-dwc fc800000.sata` probes; `ata1: SATA link down` (no disk). Built in, so no module is needed |
| 14.63 s | eMMC: `mmc0: new HS200 MMC card`, `mmcblk0: p1 p2` |
| 14.96 s | `Run /init as init process`: the initramfs takes over |

### 3.7 ⑥ The initramfs: how the root file system gets mounted

The initramfs is a small Linux userland (busybox, udev, fsck, `MODULES=most` kernel modules) that the
kernel unpacks into RAM. Its job is to find the real root file system and switch to it. It's built by
`update-initramfs` from `/usr/share/initramfs-tools` plus anything in `/etc/initramfs-tools`, and written
to `/boot/initrd.img-<version>`.

What `/init` does (Debian's `initramfs-tools`, read from this box's initrd):
1. Mounts `/proc`, `/sys`, `/dev` (devtmpfs) and `/run`, then reads the command line: `root=`,
   `rootflags=`, `rootfstype=`, `rootdelay=`, `fsck.*`, `break=`.
2. **`init-top` scripts**: udev starts and creates `/dev/disk/by-uuid/…` as disks appear.
3. Loads modules, then runs the **`init-premount`** scripts.
4. **`mountroot` → `local_mount_root`:**
   1. **`local-top` scripts** run. After each one, `/init` reads `/conf/param.conf`, so **a `local-top`
      script can change `ROOT`** by writing `ROOT=…` there. This plan's switch uses exactly that
      (section 5.3).
   2. `local_device_setup "$ROOT"` waits for the root device: up to 30 s, or `rootdelay`. If it never
      appears, you get `ALERT! UUID=… does not exist. Dropping to a shell!` and an `(initramfs)` prompt on
      HDMI (`tty1`) and on serial.
   3. **`local-premount`**, then **`checkfs`**: `fsck` of the root file system. `fsck.repair=yes` means it
      answers yes to repairs.
   4. `mount -w -t ext4 -o <rootflags> <device> /root`. The `recovery complete` line is ext4 replaying
      its journal here, after an unclean power-off.
   5. **`local-bottom`**.
5. **`init-bottom`**: udev stops. `/dev`, `/run`, `/sys` and `/proc` are moved under `/root`.
6. `run-init /root /sbin/init`: the RAM file system is emptied, `/root` becomes `/`, and **systemd**
   starts as PID 1 (18.87 s).

### 3.8 ⑦ systemd and `/etc/fstab`

systemd reads `/etc/fstab` from the root file system it was started from, and turns each line into a
mount unit:

```
UUID=94665fdf-…  /      ext4  defaults,noatime,nodiratime,commit=600,errors=remount-ro  0 1   ← remounted with these options
UUID=4352928f-…  /boot  ext4  defaults                                                  0 2   ← boot.mount, at +2.9 s
tmpfs            /tmp   tmpfs defaults,nosuid                                           0 0
```

Then `local-fs.target` (2.9 s), NetworkManager with DHCP (4.9 s), ssh, getty and the watchdog.
`graphical.target` is reached 6.0 s after systemd started: **19.3 s from power on**.

Note the split: **the initramfs decides which disk is `/`** (from `root=`), but **`fstab` on that disk
decides everything mounted after it**. Two root file systems therefore need two different `fstab`s. They
can never be byte-for-byte clones.

---

## 4. Why the SSD can't simply be "the boot disk"

| Way to boot | Possible here? | Why |
|---|---|---|
| BootROM loads from the SSD | **No** | The RK3566 BootROM doesn't boot from SATA |
| U-Boot loads the kernel from the SSD | **No** | U-Boot's device tree has no SATA (3.4). Changing that means rebuilding and re-signing U-Boot: risky, needs Maskrom, and may not start at all |
| USB SSD instead of SATA | Would work in U-Boot | But the SSD is SATA, in the box's bay |
| **Kernel and initrd from the eMMC, root file system from the SSD** | **Yes** | Linux has the SATA driver built in; `root=` can point anywhere |

So the eMMC always does steps ①–⑥, and the SSD takes over at "mount root". To make "pull the SSD and
it boots from the eMMC" work, the initramfs needs to **choose**: the SSD's root when it's present,
otherwise the eMMC's. Stock initramfs-tools has no such fallback. It waits 30 s and drops to a shell, so
the plan adds a small `local-top` script.

---

## 5. The design

### 5.1 Layout

```
eMMC (8 GB) — always boots; unchanged partition table           SSD (64 GB, SATA)
┌─────────────────────────────────────────────┐                ┌──────────────────────────────────┐
│ 0–180 MiB  idblock, vendor storage,         │                │ GPT                              │
│            uboot.itb, U-Boot env            │                │ p1 ssdroot  16 GiB  ext4  →  /   │
│ p1 BOOT    511 MiB  ext4  → /boot (SHARED)  │◀── kernel ─────│ p2 hqnode   ~43 GiB ext4         │
│            extlinux.conf, Image, initrd,    │    from here   │        → /var/hqnode             │
│            dtb  — used by BOTH systems      │                │   (images, container disks,      │
│ p2 ROOTFS  6.6 GiB  ext4                    │                │    swap file)                    │
│            → / of the RESCUE system         │                └──────────────────────────────────┘
│            + /srv/ssd-root (nightly mirror) │
└─────────────────────────────────────────────┘
```

| | Main system | Rescue system |
|---|---|---|
| `/` | SSD p1 `ssdroot` | eMMC p2 `ROOTFS` (as today) |
| `/boot` | eMMC p1 (shared) | eMMC p1 (shared) |
| Hostname | `oect` | `oect-rescue` (and a red `RESCUE` in the prompt and MOTD) |
| Runs | hqnoded, the browser container, the backup timer | sshd and tools only: no hqnode, nothing that writes to the SSD on its own |
| Mounts the other disk | eMMC p2 at `/mnt/rescue`, `noauto` (only during a backup) | nothing at boot; mount the SSD by hand when repairing it |
| machine-id, SSH host keys | **the same in both** (cloned) | so DHCP gives the same 192.168.10.15 and ssh shows no host-key warning |

Why the partitions are split this way:
- **`/var/hqnode` on its own partition.** The main root can be wiped and restored without touching
  container disks, and the other way round.
- **16 GiB root.** Armbian uses 1.8 GB today. The rootfs stays small enough that its mirror fits in the
  eMMC's 4.7 GB of free space.
- **No `/boot` on the SSD.** U-Boot couldn't read it, and a second copy would only drift.
- **No swap partition.** As in the browser plan: zram, plus hqnode's swap file in `/var/hqnode`.

### 5.2 The root file systems

- The SSD's file systems are **made fresh** (`mkfs.ext4`) and **filled with `rsync`**, never `dd`'d from
  the eMMC. `dd` would copy the eMMC's UUID as well. Two disks with one UUID means `root=UUID=` picks
  either one, and the systems get mixed up.
- Mount options on the SSD root: `defaults,noatime,errors=remount-ro`. That's ext4's `data=ordered` and
  5-second commits, so a power cut loses seconds, not minutes. The `rootflags=data=writeback` on today's
  command line is dropped for both systems.
- `fstrim.timer` (already enabled) trims the SSD weekly.

### 5.3 The root switch: `/etc/initramfs-tools/scripts/local-top/ssdroot`

The kernel command line keeps **`root=` pointing at the eMMC**, and adds **`ssdroot=`** for the SSD:

```sh
#!/bin/sh
# Root on the SSD when it is there: ssdroot=UUID=… on the command line names it. Without it, the boot
# keeps root= (the eMMC rescue system).
PREREQ=""
prereqs() { echo "$PREREQ"; }
case "$1" in prereqs) prereqs; exit 0 ;; esac

. /scripts/functions

ssd=
for x in $(cat /proc/cmdline); do
	case "$x" in ssdroot=*) ssd="${x#ssdroot=}" ;; esac
done
[ -n "$ssd" ] || exit 0

i=0
while [ "$i" -lt 10 ]; do
	if dev=$(resolve_device "$ssd") && [ -b "$dev" ]; then
		log_success_msg "ssdroot: root on the SSD, $dev"
		echo "ROOT=$dev" >>/conf/param.conf
		exit 0
	fi
	sleep 1
	i=$((i + 1))
done
log_warning_msg "ssdroot: $ssd not there after 10 s, root stays on the eMMC ($ROOT)"
exit 0
```

How it fits the sequence in 3.7: it's a `local-top` script, so it runs at step 4.1, **before** the
initramfs waits for `ROOT`. If the SSD's partition is there, it writes `ROOT=/dev/sda1` to
`/conf/param.conf`, and `/init` mounts that. If not, `ROOT` stays the eMMC's UUID.

Why it's built this way round (eMMC in `root=`, SSD in `ssdroot=`):
- **Fail-safe.** If the script is ever missing from a rebuilt initrd, the box boots the rescue system
  instead of stopping at `(initramfs)` waiting for a UUID that isn't there.
- **10 s is plenty.** The SATA driver probes at 6.5 s and the script runs after 15 s, so a present SSD
  is normally found on the first try. The wait covers a slow link. Booting without the SSD costs those
  10 s.
- **Same script on both systems.** It goes into `/etc/initramfs-tools` **before** the eMMC is cloned, so
  `update-initramfs` produces the same initrd whichever system runs it.

### 5.4 `extlinux.conf`: two entries and `DEFAULT`

```
DEFAULT ssd

LABEL ssd
  LINUX /Image
  INITRD /initrd.img-6.1.141-rk35xx-ophub
  FDT /dtb/rockchip/rk3566-wxy-oec-turbo-4g.dtb
  APPEND root=UUID=94665fdf-2a49-4b6a-877d-08a729c8f2a8 ssdroot=UUID=<ssdroot uuid> rw rootwait rootfstype=ext4 panic=10 earlycon=uart8250,mmio32,0xfe660000 console=ttyS2,1500000 console=tty1 no_console_suspend consoleblank=0 fsck.fix=yes fsck.repair=yes net.ifnames=0 max_loop=128

LABEL emmc
  LINUX /Image
  INITRD /initrd.img-6.1.141-rk35xx-ophub
  FDT /dtb/rockchip/rk3566-wxy-oec-turbo-4g.dtb
  APPEND root=UUID=94665fdf-2a49-4b6a-877d-08a729c8f2a8 rw rootwait rootfstype=ext4 panic=10 earlycon=uart8250,mmio32,0xfe660000 console=ttyS2,1500000 console=tty1 no_console_suspend consoleblank=0 fsck.fix=yes fsck.repair=yes net.ifnames=0 max_loop=128
```

- **`LABEL ssd`** carries `ssdroot=`: the SSD if present, otherwise the eMMC (5.3).
- **`LABEL emmc`** doesn't, so it's **always** the rescue system, even with the SSD plugged in.
- **`DEFAULT`** picks which one. With no `PROMPT` line, `sysboot` boots it straight away. It's switched
  with `oect-boot ssd|emmc` (below), so one mistyped character can't stop the box from booting.
- **`panic=10`** reboots 10 s after a kernel panic instead of hanging forever.
- `INITRD` names the plain initrd by version (see 3.5).

`/usr/local/sbin/oect-boot`, the same on both systems:

```sh
#!/bin/sh
# oect-boot ssd|emmc: which entry of extlinux.conf the next boot uses.
set -eu
f=/boot/extlinux/extlinux.conf
case "${1:-}" in ssd | emmc) ;; *) echo "usage: oect-boot ssd|emmc" >&2; exit 2 ;; esac
grep -q "^LABEL $1\$" "$f" || { echo "oect-boot: no LABEL $1 in $f" >&2; exit 1; }
sed "s/^DEFAULT .*/DEFAULT $1/" "$f" >"$f.new"
sync "$f.new"
mv "$f.new" "$f"
sync
grep '^DEFAULT' "$f"
```

> **Test first, on the eMMC alone.** That this 2017.09 vendor `sysboot` honours `DEFAULT` with two
> labels is expected (it's U-Boot's standard extlinux reader) but not yet seen on this box. Section 8
> step 1 tries it with both labels pointing at the eMMC, after the USB rescue stick from step 0 exists.

### 5.5 One `/boot`, two root file systems: kernel updates

Both systems boot the **same kernel** from the shared `/boot`. A kernel only works with a root file system
that has its modules in `/lib/modules/<version>`, so a kernel update has to reach both root file systems:
1. Run `armbian-update` **on the main system only**, never in rescue. It writes the new kernel to `/boot`
   and the modules to the SSD.
2. `oect-sync` (part of the backup script, 6.1) copies `/lib/modules/<new version>` to the eMMC root.
3. Check that `armbian-update` left `/boot/extlinux/extlinux.conf` alone. Then point the two `INITRD`
   lines at the new version, and reboot.

Until step 2 runs, the rescue system would boot the new kernel without its modules: no Ethernet module,
so no network. The nightly backup does step 2 anyway.

### 5.6 hqnode only on the SSD

hqnoded, passt, nftables and the browser plan's host pieces are installed **after** the clone, on the main
system only. The rescue system never starts containers. `hqnode.service` keeps
`RequiresMountsFor=/var/hqnode` from the browser plan. If the `hqnode` partition is missing, hqnode
doesn't start, instead of filling the root with a fresh `/var/hqnode`.

---

## 6. What the eMMC keeps as a backup

### 6.1 The nightly mirror (`oect-backup.timer`, main system)

```sh
#!/bin/sh
# oect-backup: mirror the SSD root onto the eMMC, so a new SSD can be restored from the rescue system.
set -eu
mount /mnt/rescue                      # eMMC p2, noauto in the main system's fstab
trap 'umount /mnt/rescue' EXIT
# the rescue system boots this kernel too: give it the modules (5.5)
rsync -a --delete /lib/modules/ /mnt/rescue/lib/modules/
rsync -aHAXx --delete --numeric-ids \
	--exclude=/var/cache/apt/ --exclude=/var/log/journal/ --exclude=/var/tmp/ \
	/ /mnt/rescue/srv/ssd-root/
```

- `-x` stays on the root file system, so `/boot`, `/var/hqnode`, `/tmp`, `/proc` and friends are skipped
  (their empty mount-point directories are kept, which a restore needs).
- Size: about 2–3 GB once the host tools are installed, against **4.7 GB free** on eMMC p2. The script
  should refuse to run when less than 1 GB would remain.
- It writes only what changed, so after the first run it's a few MB a night: no worry for the eMMC's
  wear.
- **What it doesn't keep:** `/var/hqnode`, which holds container images (pulled again) and container
  disks (made again from the image). The few small things there worth keeping are hqnode's own state
  and each container's `/data` disk (1 GB, with `browser.conf` and the agent's pairing). They get added
  to this script once hqnode is installed and its layout can be seen, using hqnode's own export if it
  has one rather than copying a live database.

### 6.2 The boot area, kept off the box

The first 180 MiB of the eMMC (idblock, vendor storage, `uboot.itb`, environment) were copied on
2026-10-24. Keep that file **on another machine**, as well as on the SSD once it's in. It's what gives
back a box whose U-Boot environment got damaged (7.5), and its vendor storage can't be downloaded
anywhere.

---

## 7. Rescue procedures

### 7.1 The SSD died: get into the rescue system
1. Power off. Unplug the SSD.
2. Power on. The initramfs doesn't find `ssdroot=` within 10 s and mounts the eMMC.
3. `ssh root@192.168.10.15` (same IP, same host key). The prompt says `oect-rescue` / `RESCUE`.

### 7.2 Work on the SSD from the rescue system
SATA isn't treated as hot-plug here: **power off before plugging or unplugging the SSD.**
1. In the rescue system: `oect-boot emmc`, then power off.
2. Plug the SSD in and power on. `DEFAULT emmc` boots the rescue system with the SSD visible as
   `/dev/sda`, and nothing mounted from it.
3. Repair: `fsck.ext4 -f /dev/sda1`, `fsck.ext4 -f /dev/sda2`, then mount and look around.
4. When it's healthy: `oect-boot ssd` and `reboot`.

The main system can also reboot itself into rescue for maintenance (`oect-boot emmc && reboot`), with no
one at the box.

### 7.3 A new SSD: restore from the mirror (in rescue, after 7.2 steps 1–2)
```sh
sfdisk /dev/sda <<'EOF'
label: gpt
start=2048, size=16GiB, type=linux, name=ssdroot
type=linux, name=hqnode
EOF
mkfs.ext4 -L ssdroot /dev/sda1
mkfs.ext4 -L hqnode /dev/sda2
mount /dev/sda1 /mnt
rsync -aHAX --numeric-ids /srv/ssd-root/ /mnt/
blkid /dev/sda1 /dev/sda2                       # new UUIDs: put them in /mnt/etc/fstab
vi /mnt/etc/fstab                               # / and /var/hqnode lines
sed -i "s/ssdroot=UUID=[^ ]*/ssdroot=UUID=$(blkid -s UUID -o value /dev/sda1)/" /boot/extlinux/extlinux.conf
umount /mnt
oect-boot ssd && reboot
```
Containers then come back through hqnode: pull the image again and recreate (the browser plan's
`create.sh` / `deploy.sh`). Restore their `/data` from the backup once 6.1 covers it.

### 7.4 The SSD is present but its system won't come up
For example a kernel panic, or systemd stuck mounting `/`.
- A panic reboots after 10 s (`panic=10`), into the same failure. You need hands on the box: either
  **unplug the SSD** (7.1), or connect **HDMI and a USB keyboard**. The `(initramfs)` shell and kernel
  messages appear on `tty1`.
- A later improvement can make this automatic, with no hands: a boot counter on `/boot`. The `ssdroot`
  script increments it, a service on the main system clears it once `multi-user.target` is reached, and
  after 3 failed boots the script skips the SSD. The watchdog (already running) covers hangs. Not in v1.

### 7.5 The eMMC boot itself is broken
For example the U-Boot environment is damaged, or `/boot` is unreadable.
1. **A USB rescue stick.** U-Boot tries USB **before** the eMMC (3.4). Write ophub's Armbian image for this
   board (rk3566, `wxy-oect`) to a USB stick and keep it with the box. Plug it in, power on, and it boots
   with the eMMC untouched. Then repair from it: restore `/boot`, or put the saved 180 MiB back with `dd`
   (only the damaged region, for example the 64 KiB environment at 144 MiB).
2. **Maskrom**, the last resort. With the box in Maskrom mode (via its recovery button or the shorting
   pads, depending on the board revision), connect a USB A-to-A cable to a PC and use `rkdeveloptool`
   with an RK356x loader to write the eMMC. With the saved boot area and an image, this restores
   everything.

---

## 8. Setup, in order

Nothing below has been done yet. Steps 0–1 can be done now; the rest waits for the SSD.

0. **Safety net first** (no SSD needed):
   - Keep the 180 MiB boot-area copy off the box (taken 2026-10-24; see the hand-over note).
   - Make the USB rescue stick (7.5) and **check that it boots**. With the stick, every later step can
     be undone.
   - `apt update`, then `apt install rsync nftables passt`. Disable `rpcbind`, `nfs-client.target` and
     `openvpn`.
1. **Prepare the eMMC system, which becomes both systems' base:**
   - Install `scripts/local-top/ssdroot` (5.3) and `oect-boot` (5.4). Run `update-initramfs -u`, then
     `lsinitramfs /boot/initrd.img-$(uname -r) | grep ssdroot`.
   - Write the two-label `extlinux.conf` **with both labels on the eMMC**: `ssd` without `ssdroot=` for
     now, plus `oect.entry=ssd` / `oect.entry=emmc` markers in `APPEND`.
   - Reboot. `/proc/cmdline` should show `oect.entry=ssd`. Then `oect-boot emmc`, reboot, and it should
     show `oect.entry=emmc`. This proves `DEFAULT` works on this U-Boot. If the box doesn't come back,
     boot the USB stick and put the old `extlinux.conf` back.
   - Optional now: build MPP and run the browser plan's encoder test (`mpi_enc_test` at 880×1808, BGRA).
2. **Install the SSD** (power off), boot, and check that `lsblk` shows `sda`.
3. **Partition and format** it as in 7.3 (`sfdisk`, `mkfs.ext4 -L ssdroot`, `-L hqnode`).
4. **Clone the eMMC root:** `mount /dev/sda1 /mnt/ssd && rsync -aHAXx --numeric-ids / /mnt/ssd/`.
5. **Make the clone the main system** (in `/mnt/ssd`):
   - `etc/fstab`: `/` = ssdroot UUID, `/boot` = eMMC p1, `/var/hqnode` = hqnode UUID, `/mnt/rescue` =
     eMMC p2 `noauto`;
   - `etc/hostname` and `etc/hosts`: `oect`.
6. **Mark the eMMC system as rescue:** hostname `oect-rescue`, plus a `RESCUE` prompt and MOTD. Leave its
   `fstab` as it is.
7. **Turn the SSD on:** put `ssdroot=UUID=<ssdroot>` into `LABEL ssd`, then `oect-boot ssd` and reboot.
   Check: `findmnt /` shows `/dev/sda1`, and `hostname` says `oect`.
8. **Tests, before anything is installed on top:**
   - power off, unplug the SSD, power on → `oect-rescue`, and boot time about 10 s longer;
   - SSD back in, `oect-boot emmc` → `oect-rescue` with `/dev/sda` visible; then `oect-boot ssd` → `oect`;
   - a restore drill: run `oect-backup` once, then in rescue restore into a scratch directory and diff.
9. **Main system only:** hqnode, the browser plan's host steps (MPP, `rd-killswitch`, zram + swap file),
   and the `oect-backup.timer` (nightly).
