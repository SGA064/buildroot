# Rhodes Island ePass Buildroot 镜像

本配置面向全志 F1C200S Rhodes Island ePass 开发板，采用 SPI NAND 单槽方案：

- U-Boot 正常启动只读取 raw `bootctrl` 和 raw `kernel`，不 attach UBI。
- Linux attach 一个 118.125 MiB 的 `ubi` MTD，并从 `ubi0:rootfs` 启动。
- 同一个 UBI 设备包含 `rootfs` 和 `data` 两个卷；`/data` 在登录提示后挂载。
- Linux 和 U-Boot 均支持 UBI Fastmap，首次 attach 自动创建 Fastmap。
- OTA 为单槽原地更新；失败后使用 USB DFU 恢复，不提供 A/B 回滚。

## 构建

```sh
cd /home/sga064/f1c200s/buildroot
make epass_f1c200s_defconfig
make
```

主要部署产物位于 `output/images/`：

| 文件 | 用途 |
| --- | --- |
| `u-boot-sunxi-with-spl.bin` | SPL 与 U-Boot proper |
| `fitImage.itb` | raw `kernel` 分区中的 FIT 内核和设备树 |
| `rootfs.ubifs` | OTA 或 DFU 单独更新 rootfs 卷 |
| `rootfs.ubi` | 首次刷写的统一 UBI 镜像，包含 rootfs 和 data 两卷 |
| `epass-flash.py` | 刷写工具：无参数启动图形界面，带参数为命令行（见下文） |

`post-image.sh` 会删除旧布局遗留的 `data.ubi`、`data.ubifs`、`epass.ubi`
和 `system.ubi`，避免误刷旧镜像。

## SPI NAND 布局

U-Boot 与 Linux 设备树使用同一分区表：

```text
128k(spl)ro,896k(u-boot)ro,512k(bootlogo),128k(env),
8m(kernel),256k(bootctrl),-(ubi)
```

| MTD | 起始地址 | 大小 | 内容 |
| --- | ---: | ---: | --- |
| `spl` | `0x0000000` | 128 KiB | SPL |
| `u-boot` | `0x0020000` | 896 KiB | U-Boot proper |
| `bootlogo` | `0x0100000` | 512 KiB | BMP 开机图 |
| `env` | `0x0180000` | 128 KiB | U-Boot 环境变量 |
| `kernel` | `0x01a0000` | 8 MiB | raw FIT，坏块跳读/跳写 |
| `bootctrl` | `0x09a0000` | 256 KiB | 2 个 raw 擦除块槽位 |
| `ubi` | `0x09e0000` | 118.125 MiB | rootfs 和 data UBI 卷 |

`rootfs` 是卷 0，类型为 dynamic，固定为 20 MiB，UBIFS 最大 LEB 数为
166。`data` 是卷 1，类型为 dynamic，带 `autoresize` 标志；其空 UBIFS
最大 LEB 数为 753。这个上限已经扣除 rootfs、UBI 布局、WL、EBA、20 个
坏块保留以及两个 Fastmap PEB。

### 布局参数维护

以本节分区表为布局依据。修改布局时必须同步核对下表，不能只改镜像打包参数：

| 参数 | 数值/推导 | 本仓库中的使用位置 |
| --- | --- | --- |
| kernel 上限 | 8 MiB | `post-image.sh`、`epass-flash.py`、`package/epass-otactl/otactl.c` |
| rootfs 上限 | 20 MiB | 上述三个文件及 `ubinize.cfg` 的 `vol_size` |
| UBI 分区 | 128 MiB − 0x9e0000 = 0x7620000 | `post-image.sh`、`epass-flash.py` |
| PEB / 最小 I/O / LEB | 0x20000 / 0x800 / 0x1f000 | `post-image.sh` 与 defconfig 的 UBIFS 配置 |
| rootfs 最大 LEB 数 | ceil(20 MiB / 0x1f000) = 166 | `configs/epass_f1c200s_defconfig` |
| data 最大 LEB 数 | 945 − 166 − 2（布局）− 1（WL）− 1（EBA）− 20（坏块保留）− 2（Fastmap）= 753 | `post-image.sh` |

运行时安装器从 `epass-otactl status` 获取镜像大小限制，不再维护常量。
布局变更还须同步外部 U-Boot 和 Linux 设备树；主机格式校验不能替代设备端容量检查。

`bootctrl` 使用两个 128 KiB 擦除块轮转保存 96 字节 v3 记录。一个块损坏时
退化为单槽，写新记录时不再具备旧记录冗余；两个块都损坏时 U-Boot 进入 DFU。

## 正常启动路径

1. U-Boot 扫描 `bootctrl` 的两个 raw 擦除块，跳过坏块并选择最新有效记录；
   空分区或无有效记录按 normal 模式处理。
2. normal 模式从 raw `kernel` 分区按逻辑顺序跳过坏块读取 FIT，不 attach UBI。
3. U-Boot 传递以下关键参数：

   ```text
   ubi.fm_autoconvert=1 ubi.mtd=ubi,0,0,0,1 root=ubi0:rootfs
   rootfstype=ubifs rw rootwait ota.mode=normal
   ```

4. Linux attach `ubi` 为 `ubi0`，首次完整扫描并创建 Fastmap，挂载
   `ubi0:rootfs` 后运行 BusyBox init 和串口 getty。
5. `S99epass-late` 在后台依次挂载 data 卷、清理已完成的 OTA、恢复随机
   种子、尝试初始化 MTP，最后启动 SSH。MTP 默认关闭。
   `/etc/init.d/seedrng`、`umtprd`、`dropbear` 提供独立的手动启停入口，
   不参与 rcS/rcK 自动遍历。关机由 `S99epass-late` 先停止初始化 worker，
   再停止 SSH/MTP、保存随机种子，最后卸载 data。

登录后可检查：

```sh
cat /proc/cmdline
cat /proc/mtd
mount | grep ubifs
cat /sys/class/ubi/ubi0/mtd_num
cat /sys/class/ubi/ubi0_0/name
cat /sys/class/ubi/ubi0_1/name
cat /var/log/epass-late-init.log
```

稳定状态下的挂载关系应为：

```text
ubi0:rootfs on / type ubifs
ubi0:data on /data type ubifs
```

`/run/epass-late-init.ready` 表示延迟初始化完成；MTP 状态可用
`/etc/init.d/umtprd status` 查看。

## Wi-Fi（手工开启）

WS73/Hi3873S 与 SD 卡共用 `mmc0`，二者互斥。镜像不为 WiFi 做任何自动拉起
（`S40network` 只拉起 `lo`），需要时手工加载；后续改由 app 决定是否开启：

```sh
# 先取出 SD 卡
ws73-switch wifi                # 加载 plat_soc、wifi_soc
ip link set wlan0 up
wpa_supplicant -Dnl80211 -i wlan0 -c /etc/wpa_supplicant.conf -B
udhcpc -i wlan0

ws73-switch status              # 查看 mmc0 设备、已加载模块和 wlan0
ws73-switch sd                  # 卸载 wifi_soc、plat_soc，切回 SD 卡
```

镜像里的 `/etc/wpa_supplicant.conf` 是 rootfs overlay 提供的安全模板：只有
`ctrl_interface`、`ap_scan=1` 和 `country=CN`，**不含任何 network**。Buildroot 的默认
模板带 `network={key_mgmt=NONE}`，会让 wpa_supplicant 自动关联任意开放 AP，已被覆盖。
真实凭据在运行时提供（见上面命令），不要写进镜像。SD 卡在位时 `ws73-switch wifi` 会直接失败。

`regulatory.db` 与 `.p7s` 内建在内核里：`CONFIG_EXTRA_FIRMWARE` +
`CONFIG_EXTRA_FIRMWARE_DIR="lib/firmware"`（相对 `$(srctree)`，资材随内核源码树
放在 `lib/firmware/`，来源为 wireless-regdb 2026.02.04，tarball 的 sha256 见
`package/wireless-regdb/wireless-regdb.hash`）。cfg80211 在 initcall 阶段就会请求
该文件，那时 rootfs 还没挂载、文件系统里也没有它，所以必须内建而不是放 rootfs
（内建固件优先于文件系统查找，命中后不再访问 `/lib/firmware`，因此启动日志里不会
出现 "Direct firmware load ... failed" / "failed to load regulatory.db"）。
更新数据库需要改 `lib/firmware/` 下的资材并重编内核。

**省电（PS）与交互延迟。** 实测固件进入节能后，"空闲后第一个包"的 RTT 约 75–100 ms，
关闭节能后约 5 ms。连续 ping 测不出这个差异（0.2 s 间隔会把链路一直喂醒），判断这类
问题必须用"静默 4 s 再发单包"的方式。一次干净的 bring-up（`ws73-switch wifi` →
`wpa_supplicant` → `udhcpc`）之后默认是**关闭**节能，不需要额外操作。驱动源码里有一条
"STA 拿到 IPv4 就自动开节能"的路径（`wal_linux_netdev.c` 的 inetaddr notifier →
`hmac_sta_pm.c` 的 `hmac_sta_pm_set_on`），但在本板实测没有生效：该函数第一道判断是
`up_vap_num > 1` 就直接返回，而本板存在 `Featureid0`/`wlan0`/`p2p0` 多个 netdev，
**很可能是这个原因，但未逐项验证**。

`CONFIG_CFG80211_DEFAULT_PS` 在本板是**惰性的**：`wal_linux_cfg80211.c` 里驱动主动清掉
`WIPHY_FLAG_PS_ON_BY_DEFAULT`（注释「不使能节能」），所以不要为省电去改内核配置。
若某处显式开过节能，用 `iw dev wlan0 set power_save off` 恢复即可（实测有效）。
`iw dev wlan0 get power_save` 读的是 cfg80211 的软件状态，驱动自行开启节能时不会同步，
判断实际状态应以延迟实测为准。

## 首次刷写和布局迁移

新布局与原有 rootfs/data 分区不兼容。迁移必须重建统一 UBI，现有 `/data`
会被清除。备份数据后让设备进入 FEL 模式，同时刷入新 U-Boot、kernel 和 UBI：

```sh
cd /home/sga064/f1c200s/buildroot
PATH=output/host/bin:$PATH output/images/epass-flash.py \
    -B output/images/u-boot-sunxi-with-spl.bin \
    -k output/images/fitImage.itb \
    -u output/images/rootfs.ubi
```

需要更新开机图时追加 `-L output/images/bootlogo.bmp`。`-B` 会把组合镜像写入
SPL/U-Boot；小写 `-b` 只指定 FEL 临时启动的 U-Boot，不写入 NAND。

主要 DFU 参数和 alt 对应如下：

| 参数 | DFU alt | 写入目标 |
| --- | --- | --- |
| `-B` | `bootloader` | SPL 与 U-Boot |
| `-k` | `kernel` | raw `kernel`，标准 MTD DFU 直接写入并跳过坏块 |
| `-R` | `rootfs` | `ubi` MTD 内的 rootfs UBIFS 卷 |
| `-u` | `ubi` | 整个统一 UBI MTD，`partubi 7` |
| `-L` | `bootlogo` | raw `bootlogo` |

`-u` 可以和 `-k` 同时使用，但不能和 `-R` 同时使用。首次使用新 alt 前必须先
刷入本方案的 U-Boot。设备已停在新版 U-Boot DFU 时可跳过 FEL 临时启动：

```sh
PATH=output/host/bin:$PATH output/images/epass-flash.py -F \
    -k output/images/fitImage.itb \
    -R output/images/rootfs.ubifs
```

kernel 在主机检查 FIT 格式和大小后，由标准 MTD DFU 按擦除块直接写入，设备端
不再执行 FIT/SHA-256 回读校验；使用 `-V` 可在写入后通过 DFU upload 比对。
rootfs 仍会先完整接收到 RAM，校验 UBIFS 格式，再更新卷并执行 SHA-256 回读。
整分区 `ubi` 适合首次刷写或重建全部 UBI 卷。

### 进入 FEL 的注意事项（重要）

默认真刷流程先用 `sunxi-fel` 把临时 U-Boot 通过 FEL 载入内存，再由它进入 DFU。FEL
有两种进入方式，行为**并不等价**：

- **冷 FEL（推荐）**：上电/复位时由 BootROM 自己进入 FEL（硬件 FEL 按键/strap，或启动
  介质不可引导时 BROM 自行落入）。此时 SoC 是复位后的干净状态，`sunxi-fel uboot` 能正常
  完成 SPL 与 U-Boot 的交接。
- **热 FEL**：在 U-Boot 命令行执行 `fel`，或正常启动后按 FEL 键（`epass_jump_to_fel()`）。
  这是 U-Boot **热跳转**进 BootROM 的 FEL 入口，SoC 仍保留 U-Boot 初始化过的外设状态。

**热 FEL 与 bootlogo 的冲突**：正常启动只要显示过 bootlogo，U-Boot 就会打开 LCDC/DE 并保持
扫帧，显示引擎持续从 DRAM 读像素。此时再热跳进 FEL，`sunxi-fel` 载入的 SPL 会重跑 PLL/DRAM
初始化，与仍在运行的显示 DMA 冲突，SPL 可能挂死并**不再回到 BROM FEL**，表现为主机侧：

```text
==> Booting temporary ePass U-Boot over FEL from /tmp/epass-fel-uboot.XXXX
usb_bulk_send() ERROR -7: Operation timed out
```

之后设备没有任何反应（SPL 没起来，U-Boot proper 也没被传输）。清空 bootlogo 分区后显示不会
开启，热 FEL 也就恢复正常——这就是"有 bootlogo 刷不进、清掉就能刷"的原因。

因此：

- 用 FEL 刷写时优先**冷启动进 FEL**，不要在 U-Boot 里敲 `fel`；
- 设备已能正常进 U-Boot 时，也可直接 `dfu 0` 再执行 `epass-flash.py -F ...`，完全绕开 FEL；
- 该问题已在 U-Boot 修复：`fel` 命令与 FEL 键在热跳转前会关闭显示（背光、TCON、DE，并复位
  对应时钟门），使热 FEL 的状态等价于冷 FEL。刷入修复版 U-Boot 时若仍被此问题卡住，可先
  `dfu 0` + `-F` 刷入，或临时清空 bootlogo。

## 刷写工具

`epass-flash.py` 是唯一的刷写工具：不带参数启动图形界面，带任意参数进入
命令行模式。命令行沿用旧 `epass-flash.sh` 的选项字母（`-B`/`-k`/`-u`/
`-R`/`-L`/`-e`、`-b`/`-s`/`-p`/`-a`、`-F`/`-l`/`-V`/`-N`/`-n`、`-d`/`-t`/
`-r`），`FEL_TOOL`、`DFU_UTIL`、`DFU_DEV`、`DFU_WAIT`、`DFU_RETRIES`、
`DFU_VERIFY` 环境变量同样保留。所有镜像都会检查非空和大小上限；此外检查
BMP 头部、FIT 头部与长度、UBIFS 魔数、SPL 校验和、U-Boot 头部与数据 CRC，
以及 UBI 每个擦除块的 EC/VID 头部 CRC 和 NAND 几何参数（2 KiB 页、128 KiB
擦除块）。`-e` 要求带有效 CRC 的 128 KiB 非冗余环境镜像。bootloader 的写入
校验独立于 FEL 启动，即使使用 `-F` 或单独指定 `-b` 也会执行。
这些检查不能替代设备端校验，也不验证 FIT/UBIFS 的全部内容。

`-t SEC` 限制等待 DFU 的实际时间，包含设备查询耗时。每次启动外部刷写工具
默认最多运行 600 秒，CLI 可用 `--tool-timeout SEC` 调整；超时会终止工具并
报告失败。两个时间参数都必须为正整数。

`DFU_VERIFY`（或 `-V`/`-N`）取值：`none` 不校验；`bootloader` 是历史默认值，
**只写不校验**，与 `none` 等价（不对 SPL/U-Boot 区域做 DFU 回读）；`raw` 校验
`bootlogo`/`env`/`kernel`；`all` 在此基础上再校验 `ubi`/`rootfs` 传输。

图形界面需要主机安装 Python 3 tkinter（Debian/Ubuntu 为 `python3-tk`）；
命令行模式无此依赖：

```sh
output/images/epass-flash.py            # 图形界面
output/images/epass-flash.py --help     # 命令行帮助
```

图形界面默认使用脚本所在目录的镜像，也可在界面上方修改镜像目录或为单个
组件更换文件。左侧显示镜像路径、校验结果和所选文件总大小，右侧集中显示设备
状态和刷写选项。方案下拉框提供：

- **完整刷写**：bootloader + 内核 + 统一 UBI（首次刷写/迁移布局）。
- **更新内核 + rootfs**：`-k` + `-R`，适合设备已运行本方案 U-Boot 时。
- **单组件刷写**：引导加载器、内核、统一 UBI、rootfs 卷或开机图。
- **自定义**：自行勾选组件。

路径框可选中复制及横向滚动。手动修改镜像目录后按回车即可重新扫描。
进度区分别显示当前步骤、整体传输百分比与用时，切换组件时不重置整体进度。
日志支持横向滚动、复制和保存；关闭“跟随输出”后可停留查看之前的内容。
刷写期间仍可复制、保存日志，刷写参数会暂时锁定。

界面通过 sysfs 枚举 USB 设备实时显示 FEL（`1f3a:efe8`）和 DFU
（`1f3a:1010`）连接状态、每个镜像的大小与格式校验结果，刷写时按镜像字节数
汇总进度条并流式显示输出。勾选”设备已在 DFU 模式 (跳过 FEL)”等价于
`-F`；校验下拉框对应 `DFU_VERIFY`。统一 UBI 与 rootfs 卷互斥，同时勾选会
在开始前被拦截。两种启动方式都会弹出刷写确认，选择统一 UBI 时会提示清空数据。
取消按钮向工具进程组发送 SIGTERM，后台任务负责清理仍未退出的进程。

无显示环境可用 `--check` 仅做镜像校验：

```sh
output/images/epass-flash.py --check output/images
```

## OTA 更新

同时更新 FIT 和 rootfs：

```sh
epass-ota-install /data/fitImage.itb /data/rootfs.ubifs
reboot
```

也可以只更新一个目标：

```sh
epass-ota-install --kernel /data/fitImage.itb
epass-ota-install --rootfs /data/rootfs.ubifs
```

安装器会验证 FIT/UBIFS 魔数、raw kernel 的实际好块容量、8 MiB 上限、rootfs
20 MiB 上限和卷容量、SHA-256。镜像先写入 `ubi0:data` 的 `/data/ota`，同步后才向
raw `bootctrl` 写 update 请求。

重启后的 U-Boot 更新流程：

1. 从 raw `bootctrl` 读取 update 请求。
2. attach `ubi`，挂载 data 卷并校验全部暂存文件及 SHA-256。
3. rootfs 更新时卸载 data，更新同一 UBI 设备内的 rootfs 卷并回读校验。
4. kernel 更新时按需重新挂载 data 读取 FIT，再擦写 raw kernel 并回读校验。
5. 将 normal 记录轮转写入另一个 bootctrl 槽，再从 raw kernel 启动。

查看或取消尚未执行的请求：

```sh
epass-otactl status
epass-otactl cancel-update
```

成功启动后，延迟初始化中的 `S99epass-late` 删除 `/data/ota`。这是单槽原地
更新；更新期间断电可能破坏 kernel 或 rootfs，设备随后应进入 USB DFU。

## Fastmap

Linux 配置启用 `CONFIG_MTD_UBI_FASTMAP`，U-Boot 同时启用 Fastmap 和
autoconvert。`ubi.fm_autoconvert=1` 允许为普通 ubinize 镜像自动创建 Fastmap；
`ubi.mtd=ubi,0,0,0,1` 的第五个参数显式允许 attach 使用 Fastmap。首次 Linux
attach 执行完整扫描并写入 Fastmap；后续 Linux 启动以及 U-Boot OTA attach 使用
该 Fastmap。Fastmap 无效时实现应回退到完整扫描。

首次刷写后至少检查两次冷启动日志：第一次应包含完整扫描和 Fastmap 创建，
第二次应包含 `attached by fastmap`。量产验证需覆盖随机断电、rootfs/data 写入、
kernel+rootfs OTA、Fastmap 损坏回退以及有出厂坏块的样本。
