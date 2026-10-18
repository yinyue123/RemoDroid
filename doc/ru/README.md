<p align="center"><a href="../../README.md">English</a> · <a href="../zh/README.md">简体中文</a> · <a href="../ja/README.md">日本語</a> · <a href="../ko/README.md">한국어</a> · <a href="../es/README.md">Español</a> · <a href="../pt-BR/README.md">Português (Brasil)</a> · <a href="../de/README.md">Deutsch</a> · <a href="../fr/README.md">Français</a> · <b>Русский</b></p>

# RemoDroid

Управляйте телефоном Android из Safari на iPhone с задержкой, достаточно низкой для облачных игр.
Телефон остаётся дома, подключённый USB-кабелем к роутеру. Вы можете смотреть на его экран и нажимать на него откуда угодно.

<p align="center"><a href="https://remodroid.pages.dev/ru/"><img src="../../wizard/button.svg" alt="Установить RemoDroid с помощью мастера: remodroid.pages.dev" width="318" height="64"></a></p>

<p align="center"><img src="../architecture.svg" alt="Как работает RemoDroid: Safari на iPhone входит на сервер управления; агент на домашнем роутере управляет телефоном Android через USB; видео, звук и касания передаются по WebRTC между Safari и роутером, а при блокировке прямого UDP — через TURN" width="100%"></p>

**Как это работает, в одном абзаце.** Небольшой **сервер** на общедоступной машине (или бесплатный Cloudflare Worker) обслуживает веб-страницу, вход в учётную запись и установку соединения. **Агент** на домашнем роутере общается с телефоном по USB через собственный встроенный ADB (отдельный `adb` не нужен) и запускает на телефоне небольшую вспомогательную программу. Она захватывает экран, кодирует его аппаратно и отправляет по WebRTC прямо в Safari; ваши касания возвращаются тем же путём. Роутер только пересылает пакеты; по пути ничего не декодируется.

<details>
<summary>Подробнее: пути передачи медиа, TURN и программы на телефоне</summary>

- **Путь передачи медиа.** По умолчанию телефон отправляет медиа по USB-кабелю, а агент передаёт их в сеть по UDP, поэтому телефону не нужен Wi-Fi. При `[phone] path = wifi` в `agent.ini` телефон использует собственный Wi-Fi; это может быть быстрее, если роутер одновременно служит точкой доступа Wi-Fi.
- **TURN.** Если прямой путь UDP между iPhone и вашим домом недоступен (NAT оператора, строгие межсетевые экраны), медиа проходят через встроенный TURN-ретранслятор сервера. Декодирования по-прежнему нет.
- **На телефоне:** `remodroid-server.jar` (захват экрана, MediaCodec H.265/H.264, инъекция ввода, буфер обмена, питание экрана, файлы) и `libremodroid.so` (стек WebRTC на C: ICE, DTLS-SRTP, каналы данных SCTP, управление перегрузкой). Оба запускаются от пользователя shell через `app_process`, поэтому приложения не устанавливаются. Во время подключения дисплей телефона выключен для экономии энергии, а звук воспроизводится на iPhone.
- Заметки и решения по проектированию: [`plan.md`](../../plan.md).

</details>

## Что понадобится

RemoDroid нетребователен к ресурсам: тяжёлую работу (захват и аппаратное кодирование) выполняет телефон, роутер только пересылает пакеты, а сервер только устанавливает соединения. Важнее всего **скорость отдачи домашнего интернета**.

| | Минимум | Рекомендуется | Используемые ресурсы |
|---|---|---|---|
| **Сервер** | Любая машина Linux с публичным IP: 1 CPU, 128 MB ОЗУ, несколько MB на диске. Или бесплатная учётная запись Cloudflare | Небольшая VM в облачном регионе, **ближайшем к вашему дому** (она передаёт поток, когда нужен TURN), с достаточным месячным трафиком | Около 2 MB ОЗУ и почти никакой нагрузки на CPU. Пропускная способность нужна только при переходе сеанса на TURN; тогда весь поток проходит через сервер (около 4,5 GB в час при 10 Mbps) |
| **Роутер** | OpenWrt, Linux (Debian, Ubuntu, CentOS, Alpine) или ASUS Merlin с Koolshare, USB-порт и около 2 MB свободного хранилища. CPU: x86, ARM или MIPS | Роутер ARM (aarch64, ARMv7) или x86. Слабый MIPS тоже работает, но может не справляться с максимальными битрейтами | Около 2 MB ОЗУ. CPU используется только для переноса пакетов между USB и UDP; декодирования нет |
| **Телефон Android** | Android 8 или новее, отладка по USB, аппаратный кодировщик H.264 | Android 12 или новее (для звука) с аппаратным кодировщиком H.265 | Собственный аппаратный кодировщик; экран выключен во время подключения |
| **iPhone** | Safari | Свежая iOS для H.265 через WebRTC (старая iOS автоматически получает H.264) | Аппаратное декодирование видео |
| **Сеть** | 3–5 Mbps на отдачу дома (720p) | 10–20 Mbps на отдачу дома и столько же на загрузку на iPhone (1080p, 60 fps) | 1–20 Mbps на поток (стандартный предел 20 Mbps, 60 fps); качество подстраивается автоматически |

Ожидаемая задержка при прямом подключении iPhone к дому — 40–80 ms, через TURN — около 70–120 ms.
Данные об использовании памяти получены на работающем сервере и агенте на RT-AC86U.

## Установка

<p align="center"><a href="https://remodroid.pages.dev/ru/"><img src="../../wizard/button.svg" alt="Установить RemoDroid с помощью мастера: remodroid.pages.dev" width="318" height="64"></a></p>

**Мастер проведёт вас через все шаги и предложит кнопки копирования.** Предпочитаете продолжить здесь? Выполните следующие пять шагов — те же, что внизу на схеме.

### 1. Сервер

На вашем сервере Linux:

```sh
curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh
```

Скрипт выбирает подходящий пакет, устанавливает его, запускает сервер и выводит ссылку вида `http://<ip>:5580/wizard.html?token=…`.

<!-- downloads:server -->
#### Скачать сервер (v0.4)

Выберите строку для CPU машины (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) и столбец для её системы. OpenWrt: берите сборку, указанную в `opkg print-architecture`. *Любой Linux* содержит и сервер, и агент.

| Сервер | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Любой Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 бита</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Сервер .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Сервер .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Сервер .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Сервер .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Сервер .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Сервер .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Сервер .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Сервер .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Сервер .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Сервер .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 бита ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Сервер .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Сервер .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Сервер .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Сервер .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Сервер .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Сервер .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 бита</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Сервер .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Сервер .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Сервер .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Сервер .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Сервер .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Сервер .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Контрольные суммы: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:server -->

Установите пакет и запустите службу:

```sh
sudo apt install ./remodroid-server_*.deb                  # Debian, Ubuntu
sudo dnf install ./remodroid-server-*.rpm                  # CentOS, Fedora
sudo apk add --allow-untrusted ./remodroid-server_*.apk    # Alpine
opkg install ./remodroid-server_*.ipk                      # OpenWrt

sudo systemctl enable --now remodroid-server               # systemd
rc-update add remodroid-server && rc-service remodroid-server start            # Alpine
/etc/init.d/remodroid-server enable && /etc/init.d/remodroid-server start      # OpenWrt
```

Затем откройте `http://<server>:5580/wizard.html?token=…`. Токен находится в `/var/lib/remodroid/setup-token`.

<details>
<summary>Ручная установка на любой Linux (.tar.gz)</summary>

Архив содержит `bin/`, `web/`, `phone/` и `etc/` (примеры ini-файлов и файлы служб). Для сервера:

```sh
sudo install -m 0755 bin/remodroid-server /usr/bin/
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r web /usr/share/remodroid/web
sudo install -m 0640 etc/server.ini.example /etc/remodroid/server.ini
remodroid-server -c /etc/remodroid/server.ini
```

`etc/remodroid-server.service` — юнит systemd, который можно скопировать в `/etc/systemd/system/`.

</details>

**Настройка.** В мастере укажите веб-порт, свой **домен** (для автоматического HTTPS от Let's Encrypt), данные входа администратора и порт STUN/TURN. Сервер перезапустится на указанных портах, и панель будет готова.

Откройте в межсетевом экране порты: **TCP 5580** (только для первоначальной настройки), ваш веб-порт (**80**) и **443**, если используется домен, **UDP 3478** и **UDP 49152–49252** (TURN).

<details>
<summary>Нет сервера? Используйте Cloudflare</summary>

1. В панели Cloudflare создайте ключ TURN: **Реальное время → Сервер TURN**.
2. Разверните `worker/`: `cd worker && npm install && npx wrangler deploy`, либо импортируйте свой форк в Cloudflare с корневым каталогом `worker`.
3. Откройте `https://remodroid.<you>.workers.dev`. Мастер запросит данные входа и ключ TURN.

Worker предоставляет ту же панель (вход, роутеры, сигнализация), работающую в Durable Object со встроенным хранилищем.
Пропустите настройку межсетевого экрана: ретрансляцию выполняет TURN Cloudflare.

</details>

<details>
<summary>Параметры сервера после первоначальной настройки</summary>

Мастер записывает `/etc/remodroid/server.ini`. После входа та же страница позволяет редактировать настройки, а вкладка **Сервер** в панели меняет по одному параметру (домен, порты, данные входа, список STUN/TURN).
`remodroid-server -t` проверяет файл, ничего не запуская. Сертификаты HTTPS обновляются автоматически.
При обновлении пакеты сохраняют ваш `server.ini`.

</details>

### 2. Агент на роутере

OpenWrt, Alpine:

```sh
wget -qO- https://remodroid.pages.dev/install_agent.sh | sh
```

Debian, Ubuntu, CentOS, Fedora:

```sh
curl -fsSL https://remodroid.pages.dev/install_agent.sh | sudo sh
```

<!-- downloads:agent -->
#### Скачать агент (v0.4)

Выберите строку для CPU машины (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) и столбец для её системы. OpenWrt: берите сборку, указанную в `opkg print-architecture`. *Любой Linux* содержит и сервер, и агент.

| Агент | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Любой Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 бита</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Агент .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Агент .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Агент .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Агент .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Агент .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Агент .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Агент .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Агент .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Агент .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Агент .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 бита ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Агент .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Агент .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Агент .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Агент .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Агент .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Агент .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 бита</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Агент .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Агент .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Агент .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Агент .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Агент .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Агент .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Сервер + Агент .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Автономные установщики Koolshare / KoolCenter: [Merlin armv7](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-armv7.tar.gz) · [Merlin aarch64](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-aarch64.tar.gz). RT-AC86U и RT-AX88U: выбирайте aarch64. Это плагины только с агентом, а не пакеты OpenWrt.

Контрольные суммы: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:agent -->

**Какой файл OpenWrt выбрать?** Найдите свой роутер и его CPU в [doc/openwrt.md](../../doc/openwrt.md); там также указаны совместимые версии OpenWrt.

**OpenWrt с LuCI, без командной строки:** в LuCI откройте **Система → Программное обеспечение → Загрузить пакет…**, выберите `.ipk` для своего роутера и установите его. Появится страница **Службы → RemoDroid**: она связывает роутер с панелью (шаг 3), запускает и останавливает агент, показывает, подключены ли панель и телефон.

В других системах установите пакет и запустите службу:

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
<summary>Ручная установка на любой Linux (.tar.gz)</summary>

Для агента, из распакованного архива:

```sh
sudo install -m 0755 bin/remodroid-agent /usr/bin/
sudo ln -s remodroid-agent /usr/bin/remodroid
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r phone/. /usr/share/remodroid/
sudo install -m 0600 etc/agent.ini.example /etc/remodroid/agent.ini
remodroid-agent -c /etc/remodroid/agent.ini
```

`etc/remodroid-agent.service` (systemd) и `etc/remodroid-agent.init` (OpenWrt procd) запускают его при загрузке.

</details>

<details>
<summary>ASUS Merlin / Koolshare</summary>

Возьмите указанный выше пакет Merlin, соответствующий CPU вашего роутера (`uname -m`):

- `remodroid-<version>-merlin-aarch64.tar.gz` для `aarch64`: 64-битные модели HND, например RT-AC86U и RT-AX88U
- `remodroid-<version>-merlin-armv7.tar.gz` для `armv7l`: 32-битные модели ARM, например RT-AC68U

Установите его через **Центр программного обеспечения → Ручная установка**. Установщик отклоняет пакет для неподходящего CPU.
Он добавляет страницу роутера с настройками, запуском/остановкой, журналами и необязательным автозапуском. Установка не запускает агент. Обычный Asuswrt-Merlin без центра программного обеспечения Koolshare/KoolCenter не поддерживается.
Сборка, перенос и ограничения: [Пакеты Merlin](../../packaging/merlin/README.md).

</details>

### 3. Регистрация роутера

В панели откройте **Агенты → Добавить**. Появится команда с одноразовым кодом; выполните её на роутере:

```sh
remodroid enroll --panel https://your.panel --code X5SX-K3M7
```

В OpenWrt ту же команду можно вставить в **Службы → RemoDroid → Сопряжение** в LuCI.

Код действует один раз в течение 10 минут. Затем роутер появится в сети; нажмите на него, чтобы изменить настройки.

### 4. Подключение телефона

На телефоне включите **Для разработчиков → Отладка по USB**. Подключите его к USB-порту роутера и нажмите **Разрешить** в запросе «Разрешить отладку по USB?» (отметьте «Всегда разрешать»). Это единственный раз, когда нужно трогать телефон.

### 5. Открытие на iPhone

Откройте панель в Safari, войдите и выберите **Поделиться → На экран «Домой»**. При запуске с экрана «Домой» она работает на весь экран (Safari не может скрыть свои панели для обычной страницы). Нажмите на телефон, чтобы подключиться.

## Использование

- **Касания** работают так же, как на телефоне, включая мультитач. **Плавающий шарик** открывает меню: блокировка, переподключение, полный экран, звук, загрузка фото, буфер обмена и настройки.
- **Звук** поступает с телефона (Android 12+), а его собственный динамик молчит во время подключения. Один раз нажмите на изображение, чтобы включить звук в Safari; переключатель **Звук** в меню возвращает воспроизведение телефону.
- Позже **обновите** сервер или роутер одной командой. Она обновляет установленные компоненты до последнего выпуска, сохраняет `server.ini` / `agent.ini` и перезапускает их:

  ```sh
  curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh    # OpenWrt: wget -qO- … | sh
  ```

<details>
<summary>Команды роутера</summary>

На роутере эти команды обращаются к работающему агенту (adb не нужен):

```sh
remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server
```

</details>

## Дополнительные возможности

<details>
<summary><b>Управление через ИИ</b>: пусть ИИ читает экран и нажимает за вас</summary>

ИИ управляет телефоном через REST по пути `/api/v1/devices/{id}/`:

- `GET …/ui` возвращает экран в JSON (через UiAutomation внутри процесса на телефоне; запасной вариант — `uiautomator dump`).
- `GET …/screenshot?format=jpeg&max_edge=1280` возвращает изображение (масштабируется на GPU, сжимается кодом на C на телефоне, никогда не записывается на диск).
- `POST …/actions` выполняет нажатия, ввод, прокрутку и ожидание по селекторам.

API предоставляет агент роутера (`[api] listen` + `token` в `agent.ini`), а сервер ретранслирует его (`remodroid-server gentoken NAME`). Один раз найденный ИИ рабочий процесс сохраняется в `skills/` и повторяется средствами Python без вызовов модели; при новой ситуации экран передаётся обратно ИИ.

```sh
export PYTHONPATH=python RD_URL=http://192.168.1.1:8790 RD_TOKEN=…
python3 -m remodroid ui                      # экран в виде структуры
python3 -m remodroid tap --rid search --wait-change
python3 skills/settings-search/run.py --query wifi
```

Проектирование: [skill-plan.md](../../skill-plan.md). Создание навыков: [skills/README.md](../../skills/README.md).

</details>

<details>
<summary><b>Уведомления телефона</b>: получайте их на iPhone или через Bark</summary>

Вспомогательная программа на телефоне опрашивает `dumpsys notification` (единственное содержимое уведомлений, доступное пользователю shell), отбрасывает постоянные и маловажные уведомления и отправляет каждое новое подключённым клиентам через WebSocket просмотра, а также в [Bark](https://github.com/Finb/Bark), если у сервера задан `bark_url`. Затем можно вызвать API управления через ИИ (или навык), чтобы отреагировать на уведомление.

- Роутер, `agent.ini`: `[notify] enabled = true`, а также `min_importance` и список `ignore`.
- Сервер, `server.ini`: `[notify] bark_url = https://api.day.app/<key>` (или для каждого роутера в `[device_notify]`).
  Ключ Bark остаётся на сервере и никогда не записывается в журналы. Доставка через WebSocket не требует настройки сервера.

Примеры конфигурации находятся в `deploy/`.

</details>

## Состояние

Все тесты в таблице ниже проходят, в том числе с настоящим Chrome в качестве клиента просмотра; звук проверен на Sony SO-51A (Android 12). Ещё не проверены на реальном оборудовании (этап M0 плана): Java-сервер телефона на разных моделях (захват, настройки кодировщика, ввод, буфер обмена, питание экрана), USB ADB на роутерах, особенности Safari (поддержка H.265, аппаратное декодирование) и управление перегрузкой в реальных сетях.

## Разработка

<details>
<summary>Содержимое репозитория</summary>

| Путь | Содержимое | Язык |
|---|---|---|
| `server/` | Сервер управления: страница входа + cookies (Basic-аутентификация для роутеров), REST API, WebSocket клиента просмотра/агента, встроенный TURN-ретранслятор, необязательный HTTPS с автоматическими сертификатами Let's Encrypt (ACME) | C + libev + mbedTLS |
| `agent/` | Агент роутера: встроенный клиент ADB (usbfs), надзор за телефоном, UDP-ретранслятор, клиент WebSocket, интерфейс командной строки | C + libev |
| `android/native/` | Стек WebRTC телефона: ICE + клиент TURN, DTLS-SRTP, каналы данных SCTP, RTP H.265/H.264, управление перегрузкой | C |
| `android/java/` | Сервер телефона: захват экрана, MediaCodec, инъекция ввода, буфер обмена, питание экрана, файлы, logcat | Java (app_process) |
| `web/` | Клиент iPhone (PWA): видео, звук, мультитач, меню плавающего шарика, загрузка фото, буфер обмена, все настройки; `panel.html` (вкладки Устройства, Агенты, Сервер) и `wizard.html` (первоначальная настройка) | HTML/CSS/JS |
| `worker/` | Та же панель в Cloudflare Workers + Durable Object с TURN от Cloudflare | JS |
| `wizard/` | Сайт установки (Cloudflare Pages, remodroid.pages.dev), `install_server.sh`, `install_agent.sh`, `update.sh` | HTML, sh |
| `common/` | Общий код C: формирование кадров WebSocket, кадры туннеля, STUN, оболочка JSON, ini, служебные функции | C |
| `third_party/` | Включённые в репозиторий статически связываемые библиотеки: libev, inih, cJSON, picohttpparser, crypt_blowfish, mbedTLS, libsrtp2, usrsctp | C |
| `deploy/` | Примеры `server.ini` / `agent.ini`, юниты systemd, скрипты OpenRC и procd | |
| `packaging/`, `.github/workflows/` | Определения пакетов nFPM (deb, rpm, apk, ipk) и процесс сборки/выпуска CI | |
| `python/`, `skills/`, `.claude/skills/phone/` | Управление через ИИ: SDK `remodroid` и CLI `rd`, повторно исполняемые навыки и использующий их навык Claude Code | Python |
| `tests/` | Сквозные тесты | Node, Python |
| `demo/` | Исходный макет панели (офлайн, без сервера) | HTML |

</details>

<details>
<summary>Сборка</summary>

Сборка на хосте (разработка и тесты):

```sh
make            # build/host/remodroid-server, build/host/remodroid-agent
make rtc-tests  # build/host/rtc/rtc_test, rtp_dump (стек телефона на хосте)
```

Статические бинарные файлы выпуска для роутеров и серверов (кросс-компилятор Zig, musl, все зависимости включены):

```sh
make dist                   # внутри контейнера сборки (build/Dockerfile, нужен podman/docker)
./build/dist.sh mipsel      # или напрямую с `zig` в PATH; цели: x86 x86_64 armv7 aarch64 mipsel mips
```

Результаты находятся в `build/out/<arch>/`: около 0,8 MB на бинарный файл (1,2 MB на MIPS).

Файлы телефона (`remodroid-server.jar` + `libremodroid.so` для каждого ABI):

```sh
ANDROID_JAR=.../platforms/android-34/android.jar D8=.../build-tools/34.0.0/d8 NDK=.../android-ndk-r27c \
  ./android/build.sh        # build/android/remodroid-server.jar, build/android/<abi>/libremodroid.so
```

</details>

<details>
<summary>Тестирование</summary>

| Команда | Что проверяется |
|---|---|
| `make test` | Сквозное тестирование уровня управления: настоящие сервер + агент, имитация adbd с точным соблюдением протокола (RSA-аутентификация проверяется OpenSSL), имитация телефона; аутентификация, REST, WebSocket, ADB push/shell/sync, туннель, UDP-ретрансляция, команды, изменения `agent.ini`, CLI, переподключения (48 проверок) |
| (в `make test`) | Передача уведомлений: конфигурация `[notify]` доходит до телефона, перехваченное уведомление достигает клиента просмотра по WebSocket, а сервер отправляет его в JSON на имитированную конечную точку Bark |
| `python3 tests/python_sdk.py` | SDK управления через ИИ, CLI `rd` и исполнитель навыков с имитацией API телефона: селекторы, снимки экрана в памяти, передача управления ИИ (код выхода 3), продолжение с `--from-step` |
| `QEMU=qemu-mipsel node tests/run.mjs build/mipsel` | То же для статических бинарных файлов роутера MIPS под эмуляцией пользовательского режима |
| `python android/native/test/rtc_interop.py` | Стек WebRTC телефона ↔ aiortc: ICE, DTLS-SRTP, SCTP, декодированные кадры H.264, каналы данных |
| `python android/native/test/rtp_depack.py` | Пакетизаторы H.265 и H.264: побитово точный цикл через независимый депакетизатор + FFmpeg |
| `python tests/e2e_webrtc.py [--relay]` | Полный путь: клиент просмотра → сервер → агент → туннель → стек телефона; `--relay` принудительно включает встроенный TURN |
| `python tests/e2e_browser.py` | Настоящий веб-клиент в Chrome без интерфейса с полным стеком: видео, касания, меню, протокол |
| `node tests/setup.mjs build/host` | Мастер первого запуска (токен настройки, сохранение, перезапуск на новом порту), вход, коды регистрации с `enroll` настоящего агента, настройки роутера через панель, переименование, удаление |
| `node tests/worker.mjs build/host` | Cloudflare Worker под `wrangler dev` (после `cd worker && npm install`) с настоящим агентом: настройка, вход, регистрация, WebSocket клиента просмотра и сигнализация, команды, переименование, удаление |
| `sudo tests/openwrt.sh IP [VERSION]` | OpenWrt (19.07–24.10) в podman с собственным адресом LAN (macvlan), LuCI, ipk агента из этого репозитория и сервером; откройте `http://IP/` с другого устройства и выполните сопряжение в Службы → RemoDroid |
| `node tests/tls.mjs build/host` | HTTPS, перенаправление, cookies `Secure`, клиент просмотра и агент через `wss://` (доверенные и недоверенные CA), перезагрузка сертификата по SIGHUP; с `PEBBLE_DIR` — выдача ACME, повторное использование и автоматическое обновление через Pebble (тестовый CA Let's Encrypt) |

Для тестов Python нужен `aiortc` (а для браузерного теста ещё `playwright` + Chromium).

</details>

Проектные документы: [plan.md](../../plan.md) (архитектура и решения), [skill-plan.md](../../skill-plan.md) (управление через ИИ),
[packaging/merlin/README.md](../../packaging/merlin/README.md) (плагин Merlin).
