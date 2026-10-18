<p align="center"><a href="../../README.md">English</a> · <b>简体中文</b> · <a href="../ja/README.md">日本語</a> · <a href="../ko/README.md">한국어</a> · <a href="../es/README.md">Español</a> · <a href="../pt-BR/README.md">Português (Brasil)</a> · <a href="../de/README.md">Deutsch</a> · <a href="../fr/README.md">Français</a> · <a href="../ru/README.md">Русский</a></p>

# RemoDroid

通过 iPhone 上的 Safari 低延迟操控 Android 手机，足以用于云游戏。
手机留在家里，用 USB 线连接路由器。你可以在任何地方查看和触控它。

<p align="center"><a href="https://remodroid.pages.dev/zh/"><img src="../../wizard/button.svg" alt="使用向导安装 RemoDroid：remodroid.pages.dev" width="318" height="64"></a></p>

<p align="center"><img src="../architecture.svg" alt="RemoDroid 的工作原理：iPhone 上的 Safari 登录控制服务器；家中路由器上的代理通过 USB 操控 Android 手机；视频、声音和触控通过 WebRTC 在 Safari 与路由器之间传输，UDP 直连受阻时通过 TURN 中继" width="100%"></p>

**用一段话说明工作原理。** 公网机器上的一个小型**服务器**（或免费的 Cloudflare Worker）负责提供
网页、登录和连接建立。家中路由器上的**代理**通过
内置的 ADB（无需 `adb`）经 USB 与手机通信，并在手机上启动一个小型辅助程序。辅助程序捕获屏幕，
使用硬件编码，再通过 WebRTC 直接发送到 Safari，触控操作沿同一路径传回。
路由器只转发数据包；中间不进行任何解码。

<details>
<summary>更多细节：媒体传输路径、TURN、手机上运行的程序</summary>

- **媒体传输路径。** 默认情况下，手机通过 USB 线发送媒体，代理将其以
  UDP 形式发送到网络，因此手机无需连接 Wi-Fi。在 `agent.ini` 中设置 `[phone] path = wifi` 后，手机改用自身的 Wi-Fi，
  如果路由器同时也是 Wi-Fi 接入点，这样可能更快。
- **TURN。** 当 iPhone 与家中之间无法建立 UDP 直连（运营商 NAT、严格的
  防火墙）时，媒体通过服务器内置的 TURN 中继传输。仍然不会进行解码。
- **手机上运行的程序：** `remodroid-server.jar`（屏幕捕获、MediaCodec H.265/H.264、输入注入、剪贴板、
  屏幕电源控制、文件）和 `libremodroid.so`（用 C 编写的 WebRTC 协议栈：ICE、DTLS-SRTP、SCTP 数据通道、拥塞
  控制）。两者都通过 `app_process` 以 shell 用户身份运行，因此不会安装任何应用。连接期间，
  手机屏幕会关闭以节省电量，声音则在 iPhone 上播放。
- 设计说明与决策：[`plan.md`](../../plan.md)。

</details>

## 所需条件

RemoDroid 很轻量：手机承担主要工作（屏幕捕获和硬件编码），路由器只转发数据包，
服务器只负责建立连接。最重要的是**家中的上传速度**。

| | 最低要求 | 推荐配置 | 资源占用 |
|---|---|---|---|
| **服务器** | 任何有公网 IP 的 Linux 机器：1 个 CPU、128 MB 内存、几 MB 磁盘空间。也可以用免费的 Cloudflare 账户代替 | 位于**离你家最近**的云区域的小型虚拟机（需要 TURN 时会承载媒体流），并有足够的月流量 | 约 2 MB 内存，几乎不占用 CPU。只有会话回退到 TURN 时才消耗带宽；此时整个媒体流都经过服务器（10 Mbps 时约每小时 4.5 GB） |
| **路由器** | OpenWrt、Linux（Debian、Ubuntu、CentOS、Alpine）或带 Koolshare 的 ASUS Merlin，有 USB 接口和约 2 MB 可用存储空间。CPU：x86、ARM 或 MIPS | ARM（aarch64、ARMv7）或 x86 路由器。性能较弱的 MIPS 路由器也能使用，但可能无法跟上最高码率 | 约 2 MB 内存。CPU 仅用于在 USB 和 UDP 之间搬运数据包；不进行解码 |
| **Android 手机** | Android 8 或更新版本、USB 调试、H.264 硬件编码器 | Android 12 或更新版本（用于声音），配有 H.265 硬件编码器 | 使用自身的硬件编码器；连接期间屏幕关闭 |
| **iPhone** | Safari | 较新版本的 iOS，以通过 WebRTC 使用 H.265（较旧版本的 iOS 会自动使用 H.264） | 硬件视频解码 |
| **网络** | 家中上传速度 3–5 Mbps（720p） | 家中上传速度 10–20 Mbps，iPhone 下载速度相同（1080p、60 fps） | 每路媒体流 1–20 Mbps（默认上限 20 Mbps、60 fps）；画质自动调整 |

iPhone 与家中直连时，预期延迟为 40–80 ms；通过 TURN 时约为 70–120 ms。
内存数据来自运行中的服务器和 RT-AC86U 上的代理。

## 安装

<p align="center"><a href="https://remodroid.pages.dev/zh/"><img src="../../wizard/button.svg" alt="使用向导安装 RemoDroid：remodroid.pages.dev" width="318" height="64"></a></p>

**向导会引导你完成所有步骤，并提供复制按钮。** 更愿意在这里操作？按照下面五步进行，
与图中底部的五步相同。

### 1. 服务器

在 Linux 服务器上运行：

```sh
curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh
```

它会选择合适的软件包，安装并启动服务器，然后输出类似下面的链接：
`http://<ip>:5580/wizard.html?token=…`。

<!-- downloads:server -->
#### 下载服务器（v0.4）

根据机器的 CPU 选择对应行（`uname -m`：`x86_64`、`aarch64` = ARM64、`armv7l` = ARMv7、`i686` = x86），根据系统选择对应列。OpenWrt：选择 `opkg print-architecture` 列出的架构版本。*任意 Linux* 包含服务器和代理。

| 服务器 | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | 任意 Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 位</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="服务器 .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="服务器 .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="服务器 .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="服务器 .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="服务器 .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="服务器 .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="服务器 .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="服务器 .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="服务器 .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="服务器 .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 位 ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="服务器 .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="服务器 .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="服务器 .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="服务器 .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="服务器 .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="服务器 .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 位</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="服务器 .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="服务器 .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="服务器 .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="服务器 .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="服务器 .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="服务器 .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

校验和：[SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:server -->

安装并启动服务：

```sh
sudo apt install ./remodroid-server_*.deb                  # Debian, Ubuntu
sudo dnf install ./remodroid-server-*.rpm                  # CentOS, Fedora
sudo apk add --allow-untrusted ./remodroid-server_*.apk    # Alpine
opkg install ./remodroid-server_*.ipk                      # OpenWrt

sudo systemctl enable --now remodroid-server               # systemd
rc-update add remodroid-server && rc-service remodroid-server start            # Alpine
/etc/init.d/remodroid-server enable && /etc/init.d/remodroid-server start      # OpenWrt
```

然后打开 `http://<server>:5580/wizard.html?token=…`。令牌位于 `/var/lib/remodroid/setup-token`。

<details>
<summary>在任意 Linux 上手动安装（.tar.gz）</summary>

压缩包包含 `bin/`、`web/`、`phone/` 和 `etc/`（ini 示例文件和服务单元文件）。服务器的安装方式：

```sh
sudo install -m 0755 bin/remodroid-server /usr/bin/
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r web /usr/share/remodroid/web
sudo install -m 0640 etc/server.ini.example /etc/remodroid/server.ini
remodroid-server -c /etc/remodroid/server.ini
```

`etc/remodroid-server.service` 是 systemd 单元文件，可以复制到 `/etc/systemd/system/`。

</details>

**完成设置。** 在向导中填写网页端口、你的**域名**（用于通过 Let's Encrypt 自动启用 HTTPS）、
管理员登录信息和 STUN/TURN 端口。服务器会使用你设置的端口重启，面板即可使用。

在防火墙中开放这些端口：**TCP 5580**（仅用于初始设置）、网页端口（**80**），使用域名时还需开放 **443**，
以及 **UDP 3478** 和 **UDP 49152–49252**（TURN）。

<details>
<summary>没有服务器？改用 Cloudflare</summary>

1. 在 Cloudflare 控制台中创建 TURN 密钥：**实时 → TURN 服务器**。
2. 部署 `worker/`：`cd worker && npm install && npx wrangler deploy`，或将你 fork 的仓库导入 Cloudflare，
   根目录设为 `worker`。
3. 打开 `https://remodroid.<you>.workers.dev`。向导会要求填写登录信息和 TURN 密钥。

Worker 提供相同的面板（登录、路由器、信令），运行在带有内置存储的 Durable Object 上。
跳过防火墙步骤；由 Cloudflare 的 TURN 负责中继。

</details>

<details>
<summary>初始设置后的服务器配置</summary>

向导会写入 `/etc/remodroid/server.ini`。之后登录后可在同一页面编辑设置，
面板的 **服务器**标签页则可逐项修改设置（域名、端口、登录信息、STUN/TURN 列表）。
`remodroid-server -t` 会检查文件，不启动任何服务。HTTPS 证书会自动续期。
软件包升级时会保留你的 `server.ini`。

</details>

### 2. 路由器上的代理

OpenWrt、Alpine：

```sh
wget -qO- https://remodroid.pages.dev/install_agent.sh | sh
```

Debian、Ubuntu、CentOS、Fedora：

```sh
curl -fsSL https://remodroid.pages.dev/install_agent.sh | sudo sh
```

<!-- downloads:agent -->
#### 下载代理（v0.4）

根据机器的 CPU 选择对应行（`uname -m`：`x86_64`、`aarch64` = ARM64、`armv7l` = ARMv7、`i686` = x86），根据系统选择对应列。OpenWrt：选择 `opkg print-architecture` 列出的架构版本。*任意 Linux* 包含服务器和代理。

| 代理 | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | 任意 Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 位</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="代理 .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="代理 .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="代理 .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="代理 .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="代理 .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="代理 .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="代理 .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="代理 .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="代理 .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="代理 .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 位 ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="代理 .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="代理 .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="代理 .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="代理 .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="代理 .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="代理 .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 位</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="代理 .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="代理 .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="代理 .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="代理 .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="代理 .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="代理 .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="服务器 + 代理 .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Koolshare / KoolCenter 离线安装包：[Merlin armv7](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-armv7.tar.gz) · [Merlin aarch64](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-aarch64.tar.gz)。RT-AC86U 和 RT-AX88U：选择 aarch64。这些是仅包含代理的插件，不是 OpenWrt 软件包。

校验和：[SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:agent -->

**该选哪个 OpenWrt 文件？** 在 [doc/openwrt.md](../openwrt.md) 中查找你的路由器及其 CPU，其中还列出了
可用的 OpenWrt 版本。

**使用 LuCI 的 OpenWrt，无需命令行：** 在 LuCI 中打开 **系统 → 软件包 → 上传软件包…**，选择
适合路由器的 `.ipk` 并安装。随后会出现 **服务 → RemoDroid**页面：用于将路由器与你的
面板配对（第 3 步）、启动和停止代理，以及显示面板和手机是否已连接。

在其他系统上，安装并启动服务：

```sh
opkg install ./remodroid-agent_*.ipk                       # OpenWrt
sudo apt install ./remodroid-agent_*.deb                   # Debian, Ubuntu
sudo dnf install ./remodroid-agent-*.rpm                   # CentOS, Fedora
sudo apk add --allow-untrusted ./remodroid-agent_*.apk     # Alpine

/etc/init.d/remodroid-agent enable && /etc/init.d/remodroid-agent start        # OpenWrt
sudo systemctl enable --now remodroid-agent                # systemd
rc-update add remodroid-agent && rc-service remodroid-agent start              # Alpine
```

<details>
<summary>在任意 Linux 上手动安装（.tar.gz）</summary>

对于代理，在解压后的目录中运行：

```sh
sudo install -m 0755 bin/remodroid-agent /usr/bin/
sudo ln -s remodroid-agent /usr/bin/remodroid
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r phone/. /usr/share/remodroid/
sudo install -m 0600 etc/agent.ini.example /etc/remodroid/agent.ini
remodroid-agent -c /etc/remodroid/agent.ini
```

`etc/remodroid-agent.service`（systemd）和 `etc/remodroid-agent.init`（OpenWrt procd）用于开机启动。

</details>

<details>
<summary>ASUS Merlin / Koolshare</summary>

从上方选择与你的路由器 CPU（`uname -m`）匹配的 Merlin 软件包：

- `remodroid-<version>-merlin-aarch64.tar.gz` 适用于 `aarch64`：RT-AC86U 和 RT-AX88U 等 64 位 HND 机型
- `remodroid-<version>-merlin-armv7.tar.gz` 适用于 `armv7l`：RT-AC68U 等 32 位 ARM 机型

在 **软件中心 → 手动安装**中安装。安装程序会拒绝 CPU 不匹配的软件包。
它会添加一个路由器页面，用于设置、启动/停止、查看日志，以及选择是否开机自启。安装后不会启动
代理。不支持没有 Koolshare/KoolCenter 软件中心的原版 Asuswrt-Merlin。
构建、迁移和限制：[Merlin 打包](../../packaging/merlin/README.md)。

</details>

### 3. 注册路由器

在面板中打开 **代理 → 添加**。页面会显示一条带有一次性代码的命令；在路由器上运行：

```sh
remodroid enroll --panel https://your.panel --code X5SX-K3M7
```

在 OpenWrt 上，也可以将同一条命令粘贴到 LuCI 的 **服务 → RemoDroid → 配对** 中。

代码只能使用一次，有效期为 10 分钟。之后路由器会显示为在线；点按它即可修改设置。

### 4. 连接手机

在手机上启用**开发者选项 → USB 调试**。将手机连接到路由器的 USB 接口，在
“允许 USB 调试？”提示中点按**允许**（勾选“始终允许”）。这是唯一需要你操作手机的时候。

### 5. 在 iPhone 上打开

在 Safari 中打开面板，登录，然后选择**分享 → 添加到主屏幕**。从主屏幕启动时会全屏
运行（Safari 无法为普通网页隐藏工具栏）。点按你的手机即可连接。

## 使用

- **触控**与直接操作手机相同，也支持多点触控。**悬浮球**可打开菜单：锁定、重新连接、
  全屏、声音、上传照片、剪贴板和设置。
- **声音**来自手机（Android 12+），连接期间手机自身的扬声器会静音。在 Safari 中点按
  一次画面即可取消静音；菜单中的**声音**开关可将声音交还给手机播放。
- 之后只需一条命令即可**更新**服务器或路由器。它会将已安装的组件更新到最新
  发布版本，保留 `server.ini` / `agent.ini`，并重启这些组件：

  ```sh
  curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh    # OpenWrt：wget -qO- … | sh
  ```

<details>
<summary>路由器命令</summary>

在路由器上，这些命令与运行中的代理通信（无需 adb）：

```sh
remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server
```

</details>

## 附加功能

<details>
<summary><b>AI 控制</b>：让 AI 读取屏幕并替你点按</summary>

AI 通过 `/api/v1/devices/{id}/` 下的 REST 接口操作手机：

- `GET …/ui` 以 JSON 返回屏幕内容（来自手机上进程内的 UiAutomation；回退方案为 `uiautomator dump`）。
- `GET …/screenshot?format=jpeg&max_edge=1280` 返回图像（在手机上由 GPU 缩放、用 C 压缩，从不写入磁盘）。
- `POST …/actions` 通过选择器执行点按、输入、滚动和等待。

路由器代理提供该接口（在 `agent.ini` 中设置 `[api] listen` 和 `token`），服务器负责转发
（`remodroid-server gentoken NAME`）。AI 探索出一次工作流程后，就会将其保存到 `skills/` 下，由 Python
重放，无需调用模型；遇到新情况时再将屏幕交回 AI。

```sh
export PYTHONPATH=python RD_URL=http://192.168.1.1:8790 RD_TOKEN=…
python3 -m remodroid ui                      # 以大纲形式显示屏幕内容
python3 -m remodroid tap --rid search --wait-change
python3 skills/settings-search/run.py --query wifi
```

设计：[skill-plan.md](../../skill-plan.md)。编写技能：[skills/README.md](../../skills/README.md)。

</details>

<details>
<summary><b>手机通知</b>：在 iPhone 上或通过 Bark 接收手机通知</summary>

手机上的辅助程序轮询 `dumpsys notification`（shell 用户唯一能看到的通知内容），
过滤掉持续性通知和低重要性通知，通过查看端 WebSocket 将每条新通知发送给已连接的客户端，
并在服务器配置了 `bark_url` 时发送到 [Bark](https://github.com/Finb/Bark)。随后你可以调用 AI
控制 API（或技能）来处理通知。

- 路由器的 `agent.ini`：`[notify] enabled = true`，以及 `min_importance` 和 `ignore` 列表。
- 服务器的 `server.ini`：`[notify] bark_url = https://api.day.app/<key>`（或在 `[device_notify]` 下按路由器分别配置）。
  Bark 密钥保留在服务器上，从不写入日志。通过 WebSocket 发送无需配置服务器。

示例配置位于 `deploy/`。

</details>

## 状态

下表中的每项测试均已通过，包括使用真实 Chrome 作为查看端的测试，声音也已在
Sony SO-51A（Android 12）上验证。尚未在真实硬件上验证的项目（计划里程碑 M0）：Java 手机服务在各种
手机上的表现（屏幕捕获、编码器设置、输入、剪贴板、屏幕电源控制）、路由器上的 USB ADB、Safari 特性（H.265
支持、硬件解码），以及真实网络中的拥塞控制。

## 开发

<details>
<summary>仓库内容</summary>

| 路径 | 内容 | 语言 |
|---|---|---|
| `server/` | 控制服务器：登录页面 + Cookie（路由器使用 Basic 认证）、REST API、查看端/代理 WebSocket、内置 TURN 中继、可选的 HTTPS 与自动获取的 Let's Encrypt（ACME）证书 | C + libev + mbedTLS |
| `agent/` | 路由器代理：内置 ADB 客户端（usbfs）、手机进程监管、UDP 中继、WebSocket 客户端、命令行接口 | C + libev |
| `android/native/` | 手机 WebRTC 协议栈：ICE + TURN 客户端、DTLS-SRTP、SCTP 数据通道、H.265/H.264 RTP、拥塞控制 | C |
| `android/java/` | 手机服务：屏幕捕获、MediaCodec、输入注入、剪贴板、屏幕电源控制、文件、logcat | Java（app_process） |
| `web/` | iPhone 客户端（PWA）：视频、声音、多点触控、悬浮球菜单、上传照片、剪贴板、全部设置；`panel.html`（设备、代理、服务器标签页）和 `wizard.html`（首次运行设置） | HTML/CSS/JS |
| `worker/` | 运行在 Cloudflare Workers + Durable Object 上的相同面板，使用 Cloudflare TURN | JS |
| `wizard/` | 安装网站（Cloudflare Pages，remodroid.pages.dev）、`install_server.sh`、`install_agent.sh`、`update.sh` | HTML、sh |
| `common/` | 共用 C 代码：WebSocket 帧处理、隧道帧、STUN、JSON 封装、ini、工具函数 | C |
| `third_party/` | 仓库内附带、静态链接的库：libev、inih、cJSON、picohttpparser、crypt_blowfish、mbedTLS、libsrtp2、usrsctp | C |
| `deploy/` | `server.ini` / `agent.ini` 示例、systemd 单元文件、OpenRC 和 procd 脚本 | |
| `packaging/`, `.github/workflows/` | nFPM 软件包定义（deb、rpm、apk、ipk）和 CI 构建/发布工作流 | |
| `python/`, `skills/`, `.claude/skills/phone/` | AI 控制：`remodroid` SDK 和 `rd` 命令行接口、可重放的技能，以及使用它们的 Claude Code 技能 | Python |
| `tests/` | 端到端测试 | Node、Python |
| `demo/` | 最初的模拟面板（离线，无服务器） | HTML |

</details>

<details>
<summary>构建</summary>

宿主机构建（开发和测试）：

```sh
make            # build/host/remodroid-server, build/host/remodroid-agent
make rtc-tests  # build/host/rtc/rtc_test, rtp_dump（在宿主机上运行手机协议栈）
```

用于路由器和服务器的静态发布二进制文件（使用 Zig 交叉编译器、musl，所有依赖均链接在内）：

```sh
make dist                   # 在构建容器内运行（build/Dockerfile，需要 podman/docker）
./build/dist.sh mipsel      # 也可直接运行，需在 PATH 中有 `zig`；目标：x86 x86_64 armv7 aarch64 mipsel mips
```

结果位于 `build/out/<arch>/`：每个二进制文件约 0.8 MB（MIPS 上为 1.2 MB）。

手机文件（`remodroid-server.jar` + 各 ABI 对应的 `libremodroid.so`）：

```sh
ANDROID_JAR=.../platforms/android-34/android.jar D8=.../build-tools/34.0.0/d8 NDK=.../android-ndk-r27c \
  ./android/build.sh        # build/android/remodroid-server.jar, build/android/<abi>/libremodroid.so
```

</details>

<details>
<summary>测试</summary>

| 命令 | 覆盖内容 |
|---|---|
| `make test` | 控制平面端到端测试：真实服务器 + 代理、忠实实现协议的模拟 adbd（使用 OpenSSL 检查 RSA 认证）、模拟手机；认证、REST、WebSocket、ADB push/shell/sync、隧道、UDP 中继、命令、`agent.ini` 修改、命令行接口、重新连接（48 项检查） |
| （包含在 `make test` 中） | 通知转发：`[notify]` 配置传到手机，捕获的通知通过 WebSocket 到达查看端，服务器将其作为 JSON 推送到模拟 Bark 端点 |
| `python3 tests/python_sdk.py` | 使用模拟手机 API 测试 AI 控制 SDK、`rd` 命令行接口和技能运行器：选择器、内存中的截图、交接给 AI（退出码 3）、使用 `--from-step` 恢复 |
| `QEMU=qemu-mipsel node tests/run.mjs build/mipsel` | 在用户态模拟环境中，对静态 MIPS 路由器二进制文件执行相同测试 |
| `python android/native/test/rtc_interop.py` | 手机 WebRTC 协议栈 ↔ aiortc：ICE、DTLS-SRTP、SCTP、H.264 解码帧、数据通道 |
| `python android/native/test/rtp_depack.py` | H.265 和 H.264 打包器：通过独立解包器 + FFmpeg 进行往返测试，结果逐位一致 |
| `python tests/e2e_webrtc.py [--relay]` | 完整路径：查看端 → 服务器 → 代理 → 隧道 → 手机协议栈；`--relay` 强制使用内置 TURN |
| `python tests/e2e_browser.py` | 在无头 Chrome 中使用真实网页客户端测试完整协议栈：视频、触控、菜单、协议 |
| `node tests/setup.mjs build/host` | 首次运行向导（设置令牌、保存、在新端口上重启）、登录、使用真实代理的 `enroll` 测试注册码、通过面板设置路由器、重命名、移除 |
| `node tests/worker.mjs build/host` | 在 `wrangler dev` 下运行 Cloudflare Worker（先执行 `cd worker && npm install`），配合真实代理测试：设置、登录、注册、查看端 WebSocket 与信令、命令、重命名、移除 |
| `sudo tests/openwrt.sh IP [VERSION]` | 在 podman 中运行 OpenWrt（19.07–24.10），使用独立局域网地址（macvlan），配备 LuCI、从本仓库构建的代理 ipk 和服务器；从另一台设备打开 `http://IP/`，在“服务 → RemoDroid”中配对 |
| `node tests/tls.mjs build/host` | HTTPS、重定向、`Secure` Cookie、查看端和代理通过 `wss://` 连接（受信任和不受信任的 CA）、收到 SIGHUP 时重新加载证书；设置 `PEBBLE_DIR` 后，使用 Pebble（Let's Encrypt 的测试 CA）测试 ACME 签发、复用和自动续期 |

Python 测试需要 `aiortc`（浏览器测试还需要 `playwright` + Chromium）。

</details>

设计文档：[plan.md](../../plan.md)（架构与决策）、[skill-plan.md](../../skill-plan.md)（AI 控制）、
[packaging/merlin/README.md](../../packaging/merlin/README.md)（Merlin 插件）。
