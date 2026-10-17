# 梅林 / Koolshare 离线安装包

适用于带 **Koolshare / KoolCenter 软件中心**的固件。针对 RT-AC86U 的
3.0.0.4.386 Koolshare 环境编写；不适用于没有软件中心的原版 Asuswrt-Merlin。
RT-AC86U 的内核报告 aarch64，但本包使用已能在该设备执行的 **armv7 静态程序**。
其他 ARM64 固件可构建 aarch64 包；安装时先执行 `-h` 检查二进制可用性。
这不是完整固件，也不是 OpenWrt 的 ipk。

## 构建

先通过项目现有流程生成 `build/out/armv7/remodroid-agent` 和 `build/android/`
（手机 jar、arm64-v8a/armeabi-v7a 的库），再执行：

```sh
make package-merlin VERSION=0.3.4 ARCH=armv7
# 或
./packaging/merlin/build.sh 0.3.4 armv7
```

输出：`build/packages/remodroid-0.3.4-merlin-armv7.tar.gz`。
GitHub Actions 在 Linux / OpenWrt 包全部完成后，单独执行
`Build Merlin agent offline packages`，生成 armv7 和 aarch64 两个离线包。
梅林包只包含路由器 agent、必需的手机端组件和软件中心管理页，不包含面板 server 程序或面板网页。
两个包与其他产物一起写入 `SHA256SUMS` 并上传到 `packages` artifact；
推送 `v*` 标签时也会自动附加到 GitHub Release。
本地 `packaging/package.sh` 仅生成 Linux / OpenWrt 包，梅林包使用上面的独立命令构建。

也可以使用已下载的同版本发行文件（不重新编译）：

```sh
BIN=/path/to/remodroid-0.3.4-linux-armv7/bin \
PHONE=/path/to/remodroid-0.3.4-linux-armv7/phone \
./packaging/merlin/build.sh 0.3.4 armv7
```

源码位于 `packaging/merlin/deploy/`；构建时附加二进制、手机端文件和版本信息。
压缩包只有 `remodroid/` 一个顶层目录，其内含 `install.sh`，符合软件中心离线安装格式。
其中 `.valid` 包含 `hnd` 平台标记，供 RT-AC86U 软件中心的离线安装器校验；
不要漏掉这个隐藏文件，否则上传后会被拒绝。
包中不包含个人服务器密钥或已有设备身份。

## 安装与使用

1. 软件中心 → 手动安装 → 选择上述 `.tar.gz` → 上传并安装，不要解压后上传。
2. 在“已安装”里打开 **RemoDroid**；也可打开 `/Module_remodroid.asp`。
3. 在“绑定面板”中粘贴面板生成的整行 `remodroid enroll --panel ... --code ...` 命令，
   点击“绑定并启动”。程序会兑换配对码、保存设备身份并启动；配对失败保留原配置。
   已运行的代理须先停止才能重新绑定。页面只提取地址和配对码，不执行粘贴的 shell 命令。
4. 也可展开“高级配置”，粘贴完整 `agent.ini`，保存后启动。
   USB 手机需要开启 USB 调试并在手机上授权。
5. 日志用于确认面板连接和手机连接；“正在运行”只表示代理进程存在。
6. 按需开启“开机启动”。初次安装不自动运行，升级会停止进程但保留开机启动选择，
   升级完成后请手动启动。

从 NAS 迁移时，先停止 NAS 上同一身份的 agent，避免同时连接。可粘贴原 INI：
保存时会自动重写 `[agent]` 的 `files_dir`、`adbkey`、`socket`，其他设置保留。
如需沿用手机已有的信任授权，可自行将旧 `adbkey`（及存在时的 `adbkey.pub`）
拷到下述配置目录，权限设为 600；否则手机会要求授权新密钥。

- 程序、手机端文件：`/koolshare/remodroid/`
- 配置、ADB 密钥、开机启动标记：`/jffs/configs/remodroid/`（目录 700，配置 600）
- 上一次配置：`/jffs/configs/remodroid/agent.ini.bak`
- PID、通信 socket、日志：`/tmp/remodroid/`（重启后清空）
- 日志每 5 秒检查一次，超过 256KB 时保留末尾 150 行。短时突发日志可能超过此阈值。
- 配对输出和配对暂存配置也在 `/tmp/remodroid/`；轮转、状态轮询不写 JFFS。
  配置和密钥仅在保存、成功配对或首次生成时持久化，开机启动选项仅在切换时写入。

页面与 Clash 一样加载固件的 `state.js`、`index_style.css`、`form_style.css`，
通过 `show_menu()` 保留 ASUS 顶栏和左侧导航，内容使用原生 `FormTitle`、`FormTable`、
`button_gen`。插件 CSS 仅作用于 `#remodroid`，不覆盖固件导航。

插件不修改 LAN/WAN、默认路由、DNS、防火墙，也不调整路由器登录设置。
若系统时间、DNS 或默认路由不正确，请先修复联网再启动；WSS 还需要正确的 CA 证书路径。
页面复用软件中心认证与 RPC，不增加监听端口；配置只通过认证接口读取，不放到网页静态目录。
仅允许固定控制动作，配置内容不作为 shell 执行。

## 卸载

通过软件中心卸载会停止本插件管理的 agent，并删除界面、程序和启动钩子。
配置、ADB 密钥保留在 `/jffs/configs/remodroid/`，便于重装；自动启动标记会移除。
不会终止其他路径下手动运行的 agent，也不会停止 NAS 上的服务。

## 验证

```sh
python3 tests/merlin.py
node --check packaging/merlin/deploy/res/remodroid.js
# 已安装 Playwright 和浏览器时（也可用 PLAYWRIGHT_MODULE、BROWSER_BIN 指定路径）：
node tests/merlin-ui.cjs
```

测试覆盖离线包结构、INI 路径转换、非法输入，以及 Linux 隔离目录中的安装、
启停、配置保留、RPC 编码和卸载。生命周期测试使用本地假 agent，既不连接面板，也不控制手机。
2026-10-17 已在 RT-AC86U（KoolCenter 1.9.14）通过软件中心原生
`ks_tar_install.sh` 验证首次安装与升级，ARMv7 0.3.4 包占用约 1.2MB。
浏览器使用 SSH 读取的已安装页面资源及隧道中的真实 RPC，验证了中文配置保存、
重新读取、开机启动开关、空配置拒绝启动、真实 ARM 程序启停/重启和日志清空。
启停测试使用本机关闭端口的临时配置，完成后恢复模板、停止进程并关闭开机启动。
网页登录及手动选择文件上传未自动化；真实手机/面板连接和路由器重启仍需验证。
原生框架改版的交互测试覆盖菜单钩子、整行配对解析、非法命令拒绝和配对错误显示；
原生菜单的最终外观需在已登录的路由器网页检查。隔离生命周期测试覆盖配对成功自动启动、
失败保留配置、运行中拒绝重新配对。目标路由器已验证配对连接失败时保留配置，
并确认运行/配对日志所在 `/tmp` 为 tmpfs；未使用用户的一次性配对码进行测试。
该设备测试时系统时间停在 2018 年，连接 WSS 面板前需先校准时间。
