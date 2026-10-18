<p align="center"><a href="../../README.md">English</a> · <a href="../zh/README.md">简体中文</a> · <a href="../ja/README.md">日本語</a> · <a href="../ko/README.md">한국어</a> · <a href="../es/README.md">Español</a> · <a href="../pt-BR/README.md">Português (Brasil)</a> · <b>Deutsch</b> · <a href="../fr/README.md">Français</a> · <a href="../ru/README.md">Русский</a></p>

# RemoDroid

Steuere ein Android-Handy mit niedriger Latenz aus Safari auf deinem iPhone, schnell genug für Cloud-Gaming.
Das Handy bleibt zu Hause per USB-Kabel am Router. Du siehst und bedienst es von überall.

<p align="center"><a href="https://remodroid.pages.dev/de/"><img src="../../wizard/button.svg" alt="RemoDroid mit dem Assistenten installieren: remodroid.pages.dev" width="318" height="64"></a></p>

<p align="center"><img src="../architecture.svg" alt="So funktioniert RemoDroid: Safari auf dem iPhone meldet sich am Steuerserver an; der Agent auf dem Router zu Hause steuert das Android-Handy über USB; Video, Ton und Berührungen laufen per WebRTC zwischen Safari und dem Router, über TURN, wenn direktes UDP blockiert ist" width="100%"></p>

**So funktioniert es, in einem Absatz.** Ein kleiner **Server** auf einem öffentlichen Rechner (oder ein kostenloser
Cloudflare Worker) stellt die Webseite, deine Anmeldung und den Verbindungsaufbau bereit. Ein **Agent** auf deinem
Router zu Hause spricht über USB mit dem Handy, mit eigenem eingebautem ADB (kein `adb` nötig), und startet einen kleinen
Helfer auf dem Handy. Der Helfer nimmt den Bildschirm auf, kodiert ihn in Hardware und schickt ihn per WebRTC direkt an
Safari; deine Berührungen gehen denselben Weg zurück. Der Router leitet nur Pakete weiter; unterwegs wird nichts dekodiert.

<details>
<summary>Mehr Details: Medienwege, TURN, was auf dem Handy läuft</summary>

- **Medienweg.** Standardmäßig schickt das Handy seine Medien über das USB-Kabel, und der Agent gibt sie als UDP ins
  Netz. Das Handy braucht also kein WLAN. Mit `[phone] path = wifi` in `agent.ini` nutzt das Handy stattdessen sein
  eigenes WLAN; das kann schneller sein, wenn der Router auch der WLAN-Zugangspunkt ist.
- **TURN.** Findet sich kein direkter UDP-Weg zwischen iPhone und Zuhause (Carrier-NAT, strenge Firewalls), laufen die
  Medien über das eingebaute TURN-Relay des Servers. Auch dort wird nichts dekodiert.
- **Auf dem Handy:** `remodroid-server.jar` (Bildschirmaufnahme, MediaCodec H.265/H.264, Eingabe-Injektion,
  Zwischenablage, Bildschirmstrom, Dateien) und `libremodroid.so` (ein WebRTC-Stack in C: ICE, DTLS-SRTP,
  SCTP-Datenkanäle, Staukontrolle). Beide laufen als Shell-Benutzer über `app_process`, es wird also keine App
  installiert. Solange du verbunden bist, wird das Display des Handys abgeschaltet, um Strom zu sparen, und der Ton
  kommt aus deinem iPhone.
- Designnotizen und Entscheidungen: [`plan.md`](../../plan.md).

</details>

## Was du brauchst

RemoDroid ist leicht: Das Handy erledigt die schwere Arbeit (Aufnahme und Hardware-Kodierung), der Router leitet nur
Pakete weiter, und der Server baut nur Verbindungen auf. Am wichtigsten ist die **Upload-Geschwindigkeit zu Hause**.

| | Minimum | Empfohlen | Was es nutzt |
|---|---|---|---|
| **Server** | Jeder Linux-Rechner mit öffentlicher IP: 1 CPU, 128 MB RAM, ein paar MB Platte. Oder stattdessen ein kostenloses Cloudflare-Konto | Eine kleine VM in der Cloud-Region **am nächsten zu deinem Zuhause** (sie trägt den Stream, wenn TURN nötig ist), mit genug monatlichem Datenvolumen | Etwa 2 MB RAM und kaum CPU. Bandbreite nur, wenn eine Sitzung auf TURN ausweicht; dann läuft der ganze Stream hindurch (etwa 4,5 GB pro Stunde bei 10 Mbit/s) |
| **Router** | OpenWrt, Linux (Debian, Ubuntu, CentOS, Alpine) oder ASUS Merlin mit Koolshare, ein USB-Port und etwa 2 MB freier Speicher. CPU: x86, ARM oder MIPS | Ein ARM- (aarch64, ARMv7) oder x86-Router. Ein schwacher MIPS-Router funktioniert, kommt aber bei den höchsten Bitraten eventuell nicht mit | Etwa 2 MB RAM. CPU nur, um Pakete zwischen USB und UDP zu verschieben; nichts wird dekodiert |
| **Android-Handy** | Android 8 oder neuer, USB-Debugging, Hardware-H.264-Encoder | Android 12 oder neuer (für Ton) mit Hardware-H.265-Encoder | Sein eigener Hardware-Encoder; das Display ist aus, solange du verbunden bist |
| **iPhone** | Safari | Ein aktuelles iOS für H.265 über WebRTC (ältere iOS-Versionen bekommen automatisch H.264) | Hardware-Videodekodierung |
| **Netzwerk** | 3–5 Mbit/s Upload zu Hause (720p) | 10–20 Mbit/s Upload zu Hause und derselbe Download auf dem iPhone (1080p, 60 fps) | 1–20 Mbit/s pro Stream (Standardgrenze 20 Mbit/s, 60 fps); die Qualität passt sich automatisch an |

Die erwartete Verzögerung liegt bei 40–80 ms, wenn das iPhone dein Zuhause direkt erreicht, und bei etwa 70–120 ms über TURN.
Die Speicherangaben stammen von einem laufenden Server und einem Agent auf einem RT-AC86U.

## Installation

<p align="center"><a href="https://remodroid.pages.dev/de/"><img src="../../wizard/button.svg" alt="RemoDroid mit dem Assistenten installieren: remodroid.pages.dev" width="318" height="64"></a></p>

**Der Assistent führt dich mit Kopier-Buttons durch alles.** Lieber hier bleiben? Folge diesen fünf Schritten,
denselben fünf wie unten im Bild.

### 1. Server

Auf deinem Linux-Server:

```sh
curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh
```

Es wählt das passende Paket, installiert es, startet den Server und gibt einen Link wie
`http://<ip>:5580/wizard.html?token=…` aus.

<!-- downloads:server -->
#### Den Server herunterladen (v0.4)

Wähle die Zeile für die CPU des Rechners (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) und die Spalte für sein System. OpenWrt: Nimm den Build, den `opkg print-architecture` anzeigt. *Jedes Linux* enthält Server und Agent.

| Server | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Jedes Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 Bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Server .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Server .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Server .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Server .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Server .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Server .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Server .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Server .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Server .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Server .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32-Bit-ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Server .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Server .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Server .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Server .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Server .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Server .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 Bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Server .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Server .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Server .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Server .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Server .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Server .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Prüfsummen: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:server -->

Installieren und den Dienst starten:

```sh
sudo apt install ./remodroid-server_*.deb                  # Debian, Ubuntu
sudo dnf install ./remodroid-server-*.rpm                  # CentOS, Fedora
sudo apk add --allow-untrusted ./remodroid-server_*.apk    # Alpine
opkg install ./remodroid-server_*.ipk                      # OpenWrt

sudo systemctl enable --now remodroid-server               # systemd
rc-update add remodroid-server && rc-service remodroid-server start            # Alpine
/etc/init.d/remodroid-server enable && /etc/init.d/remodroid-server start      # OpenWrt
```

Öffne dann `http://<server>:5580/wizard.html?token=…`. Das Token steht in `/var/lib/remodroid/setup-token`.

<details>
<summary>Jedes Linux (.tar.gz) von Hand</summary>

Das Tarball enthält `bin/`, `web/`, `phone/` und `etc/` (Beispiel-ini-Dateien und Dienst-Units). Für den Server:

```sh
sudo install -m 0755 bin/remodroid-server /usr/bin/
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r web /usr/share/remodroid/web
sudo install -m 0640 etc/server.ini.example /etc/remodroid/server.ini
remodroid-server -c /etc/remodroid/server.ini
```

`etc/remodroid-server.service` ist eine systemd-Unit, die du nach `/etc/systemd/system/` kopieren kannst.

</details>

**Einrichten.** Trage im Assistenten den Web-Port, deine **Domain** (für automatisches HTTPS von Let's Encrypt), den
Admin-Login und den STUN/TURN-Port ein. Der Server startet auf deinen Ports neu, und das Panel ist bereit.

Öffne diese Ports in der Firewall: **TCP 5580** (nur für die Einrichtung), deinen Web-Port (**80**) und **443**, wenn du
eine Domain nutzt, **UDP 3478** und **UDP 49152–49252** (TURN).

<details>
<summary>Kein Server? Nimm Cloudflare</summary>

1. Erstelle im Cloudflare-Dashboard einen TURN-Schlüssel: **Realtime → TURN Server**.
2. Stelle `worker/` bereit: `cd worker && npm install && npx wrangler deploy`, oder importiere deinen Fork mit
   Root-Verzeichnis `worker` in Cloudflare.
3. Öffne `https://remodroid.<you>.workers.dev`. Sein Assistent fragt nach Login und TURN-Schlüssel.

Der Worker ist dasselbe Panel (Login, Router, Signalisierung) auf einem Durable Object mit eingebautem Speicher.
Überspringe den Firewall-Schritt; das Weiterleiten übernimmt Cloudflares TURN.

</details>

<details>
<summary>Servereinstellungen nach der Einrichtung</summary>

Der Assistent schreibt `/etc/remodroid/server.ini`. Danach ändert dieselbe Seite (hinter dem Login) die Einstellungen,
und der Tab **Server** im Panel ändert jeweils eine Einstellung (Domain, Ports, Login, STUN/TURN-Liste).
`remodroid-server -t` prüft die Datei, ohne etwas zu starten. HTTPS-Zertifikate verlängern sich automatisch.
Pakete behalten deine `server.ini` beim Upgrade.

</details>

### 2. Agent auf dem Router

OpenWrt, Alpine:

```sh
wget -qO- https://remodroid.pages.dev/install_agent.sh | sh
```

Debian, Ubuntu, CentOS, Fedora:

```sh
curl -fsSL https://remodroid.pages.dev/install_agent.sh | sudo sh
```

<!-- downloads:agent -->
#### Den Agent herunterladen (v0.4)

Wähle die Zeile für die CPU des Rechners (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) und die Spalte für sein System. OpenWrt: Nimm den Build, den `opkg print-architecture` anzeigt. *Jedes Linux* enthält Server und Agent.

| Agent | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Jedes Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 Bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Agent .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Agent .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Agent .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32-Bit-ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Agent .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Agent .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Agent .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 Bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Server + Agent .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Offline-Installer für Koolshare / KoolCenter: [Merlin armv7](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-armv7.tar.gz) · [Merlin aarch64](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-aarch64.tar.gz). RT-AC86U und RT-AX88U: aarch64 wählen. Das sind reine Agent-Plugins, keine OpenWrt-Pakete.

Prüfsummen: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:agent -->

**Welche OpenWrt-Datei?** Finde deinen Router und seine CPU in [doc/openwrt.md](../openwrt.md); dort stehen auch
die OpenWrt-Versionen, die funktionieren.

**OpenWrt mit LuCI, ganz ohne Kommandozeile:** Öffne in LuCI **System → Software → Upload Package…**, wähle das
`.ipk` für deinen Router und installiere es. Eine Seite **Services → RemoDroid** erscheint: Sie koppelt den Router mit
deinem Panel (Schritt 3), startet und stoppt den Agent und zeigt, ob Panel und Handy verbunden sind.

Auf anderen Systemen installieren und den Dienst starten:

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
<summary>Jedes Linux (.tar.gz) von Hand</summary>

Für den Agent, aus dem entpackten Tarball:

```sh
sudo install -m 0755 bin/remodroid-agent /usr/bin/
sudo ln -s remodroid-agent /usr/bin/remodroid
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r phone/. /usr/share/remodroid/
sudo install -m 0600 etc/agent.ini.example /etc/remodroid/agent.ini
remodroid-agent -c /etc/remodroid/agent.ini
```

`etc/remodroid-agent.service` (systemd) und `etc/remodroid-agent.init` (OpenWrt procd) starten ihn beim Booten.

</details>

<details>
<summary>ASUS Merlin / Koolshare</summary>

Nimm das Merlin-Paket von oben, das zur CPU deines Routers passt (`uname -m`):

- `remodroid-<version>-merlin-aarch64.tar.gz` für `aarch64`: 64-Bit-HND-Modelle wie RT-AC86U und RT-AX88U
- `remodroid-<version>-merlin-armv7.tar.gz` für `armv7l`: 32-Bit-ARM-Modelle wie RT-AC68U

Installiere es unter **Software Center → Manual installation**. Der Installer lehnt ein Paket für die falsche CPU ab.
Er fügt eine Router-Seite für Einstellungen, Start/Stopp, Logs und optionalen Autostart hinzu. Die Installation startet
den Agent nicht. Ein unverändertes Asuswrt-Merlin ohne das Software-Center von Koolshare/KoolCenter wird nicht unterstützt.
Bauen, Umzug und Grenzen: [Merlin-Paket](../../packaging/merlin/README.md).

</details>

### 3. Den Router koppeln

Öffne im Panel **Agents → Add**. Es zeigt einen Befehl mit einem Einmalcode; führe ihn auf dem Router aus:

```sh
remodroid enroll --panel https://your.panel --code X5SX-K3M7
```

Unter OpenWrt kannst du denselben Befehl stattdessen in LuCI unter **Services → RemoDroid → Pair** einfügen.

Der Code gilt einmal, für 10 Minuten. Danach erscheint der Router als online; tippe ihn an, um seine Einstellungen zu ändern.

### 4. Das Handy anschließen

Schalte auf dem Handy **Entwickleroptionen → USB-Debugging** ein. Steck es in den USB-Port des Routers und tippe bei
„USB-Debugging zulassen?“ auf **Zulassen** (Haken bei „Immer zulassen“). Das ist das einzige Mal, dass du das Handy anfasst.

### 5. Auf dem iPhone öffnen

Öffne dein Panel in Safari, melde dich an, dann **Teilen → Zum Home-Bildschirm**. Vom Home-Bildschirm gestartet läuft es
im Vollbild (Safari kann seine Leisten bei einer normalen Seite nicht ausblenden). Tippe auf dein Handy, um zu verbinden.

## Benutzung

- **Berührungen** funktionieren wie auf dem Handy, Multi-Touch inklusive. Die **schwebende Kugel** öffnet das Menü:
  sperren, neu verbinden, Vollbild, Ton, Foto-Upload, Zwischenablage und Einstellungen.
- Der **Ton** kommt vom Handy (Android 12+), dessen eigener Lautsprecher verstummt, solange du verbunden bist. Tippe einmal
  auf das Bild, um ihn in Safari einzuschalten; der Schalter **Sound** im Menü gibt ihn ans Handy zurück.
- **Aktualisiere** Server oder Router später mit einem Befehl. Er bringt alle installierten Teile auf die neueste Version,
  behält `server.ini` / `agent.ini` und startet sie neu:

  ```sh
  curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh    # OpenWrt: wget -qO- … | sh
  ```

<details>
<summary>Router-Befehle</summary>

Auf dem Router sprechen diese Befehle mit dem laufenden Agent (kein adb nötig):

```sh
remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server
```

</details>

## Extras

<details>
<summary><b>KI-Steuerung</b>: Lass eine KI den Bildschirm lesen und für dich tippen</summary>

Die KI bedient das Handy per REST unter `/api/v1/devices/{id}/`:

- `GET …/ui` liefert den Bildschirm als JSON (von einer UiAutomation im Prozess auf dem Handy; `uiautomator dump` als Rückfall).
- `GET …/screenshot?format=jpeg&max_edge=1280` liefert das Bild (per GPU skaliert, in C auf dem Handy komprimiert, nie auf die Platte geschrieben).
- `POST …/actions` tippt, schreibt, scrollt und wartet per Selektor.

Der Router-Agent stellt es bereit (`[api] listen` + `token` in `agent.ini`), der Server leitet es weiter
(`remodroid-server gentoken NAME`). Einen Ablauf, den die KI einmal herausgefunden hat, speichert `skills/`, und Python
spielt ihn ohne Modellaufrufe ab; trifft er auf etwas Neues, gibt er den Bildschirm an die KI zurück.

```sh
export PYTHONPATH=python RD_URL=http://192.168.1.1:8790 RD_TOKEN=…
python3 -m remodroid ui                      # the screen as an outline
python3 -m remodroid tap --rid search --wait-change
python3 skills/settings-search/run.py --query wifi
```

Design: [skill-plan.md](../../skill-plan.md). Skills schreiben: [skills/README.md](../../skills/README.md).

</details>

<details>
<summary><b>Handy-Benachrichtigungen</b>: Die Benachrichtigungen des Handys auf dem iPhone oder über Bark</summary>

Der Helfer auf dem Handy fragt `dumpsys notification` ab (der einzige Benachrichtigungsinhalt, den der Shell-Benutzer
sieht), verwirft laufende und unwichtige Benachrichtigungen und schickt jede neue über den Viewer-WebSocket an verbundene
Clients, und an [Bark](https://github.com/Finb/Bark), wenn der Server eine `bark_url` hat. Von dort kannst du die
KI-Steuerungs-API (oder einen Skill) aufrufen, um darauf zu reagieren.

- Router, `agent.ini`: `[notify] enabled = true`, dazu `min_importance` und eine `ignore`-Liste.
- Server, `server.ini`: `[notify] bark_url = https://api.day.app/<key>` (oder pro Router unter `[device_notify]`).
  Der Bark-Schlüssel bleibt auf dem Server und wird nie geloggt. Die Zustellung per WebSocket braucht keine Serverkonfiguration.

Beispielkonfigurationen liegen in `deploy/`.

</details>

## Status

Alle Tests in der Tabelle unten bestehen, auch mit echtem Chrome als Viewer, und der Ton wurde auf einem
Sony SO-51A (Android 12) geprüft. Noch nicht auf echter Hardware geprüft (Plan-Meilenstein M0): der Java-Handy-Server auf
verschiedenen Handys (Aufnahme, Encoder-Einstellungen, Eingabe, Zwischenablage, Bildschirmstrom), USB-ADB auf Routern,
Safari-Eigenheiten (H.265-Unterstützung, Hardware-Dekodierung) und Staukontrolle in echten Netzen.

## Entwicklung

<details>
<summary>Was in diesem Repository steckt</summary>

| Pfad | Was | Sprache |
|---|---|---|
| `server/` | Steuerserver: Login-Seite + Cookies (Basic Auth für Router), REST-API, Viewer-/Agent-WebSockets, eingebautes TURN-Relay, optionales HTTPS mit automatischen Let's-Encrypt-Zertifikaten (ACME) | C + libev + mbedTLS |
| `agent/` | Router-Agent: eingebauter ADB-Client (usbfs), Handy-Überwachung, UDP-Relay, WebSocket-Client, CLI | C + libev |
| `android/native/` | WebRTC-Stack des Handys: ICE- + TURN-Client, DTLS-SRTP, SCTP-Datenkanäle, H.265/H.264-RTP, Staukontrolle | C |
| `android/java/` | Handy-Server: Bildschirmaufnahme, MediaCodec, Eingabe-Injektion, Zwischenablage, Bildschirmstrom, Dateien, logcat | Java (app_process) |
| `web/` | iPhone-Client (PWA): Video, Ton, Multi-Touch, Menü mit schwebender Kugel, Foto-Upload, Zwischenablage, alle Einstellungen; `panel.html` (Tabs Devices, Agents, Server) und `wizard.html` (Ersteinrichtung) | HTML/CSS/JS |
| `worker/` | Dasselbe Panel auf Cloudflare Workers + einem Durable Object, mit Cloudflare TURN | JS |
| `wizard/` | Die Installationsseite (Cloudflare Pages, remodroid.pages.dev), `install_server.sh`, `install_agent.sh`, `update.sh` | HTML, sh |
| `common/` | Gemeinsames C: WebSocket-Framing, Tunnel-Frames, STUN, JSON-Umschlag, ini, Hilfsfunktionen | C |
| `third_party/` | Mitgelieferte Bibliotheken, statisch gelinkt: libev, inih, cJSON, picohttpparser, crypt_blowfish, mbedTLS, libsrtp2, usrsctp | C |
| `deploy/` | Beispiel-`server.ini` / `agent.ini`, systemd-Units, OpenRC- und procd-Skripte | |
| `packaging/`, `.github/workflows/` | nFPM-Paketdefinitionen (deb, rpm, apk, ipk) und der CI-Build-/Release-Workflow | |
| `python/`, `skills/`, `.claude/skills/phone/` | KI-Steuerung: das `remodroid`-SDK und die `rd`-CLI, abspielbare Skills und der Claude-Code-Skill, der sie nutzt | Python |
| `tests/` | End-to-End-Tests | Node, Python |
| `demo/` | Das ursprüngliche Mock-Panel (offline, ohne Server) | HTML |

</details>

<details>
<summary>Bauen</summary>

Host-Build (Entwicklung und Tests):

```sh
make            # build/host/remodroid-server, build/host/remodroid-agent
make rtc-tests  # build/host/rtc/rtc_test, rtp_dump (phone stack on the host)
```

Statische Release-Binaries für Router und Server (Zig als Cross-Compiler, musl, alles gelinkt):

```sh
make dist                   # inside the build container (build/Dockerfile, needs podman/docker)
./build/dist.sh mipsel      # or directly, with `zig` on PATH; targets: x86 x86_64 armv7 aarch64 mipsel mips
```

Die Ergebnisse landen in `build/out/<arch>/`: etwa 0,8 MB pro Binary (1,2 MB auf MIPS).

Handy-Dateien (`remodroid-server.jar` + `libremodroid.so` pro ABI):

```sh
ANDROID_JAR=.../platforms/android-34/android.jar D8=.../build-tools/34.0.0/d8 NDK=.../android-ndk-r27c \
  ./android/build.sh        # build/android/remodroid-server.jar, build/android/<abi>/libremodroid.so
```

</details>

<details>
<summary>Tests</summary>

| Befehl | Deckt ab |
|---|---|
| `make test` | Steuerebene Ende zu Ende: echter Server + Agent, ein protokolltreuer Fake-adbd (RSA-Authentifizierung mit OpenSSL geprüft), Fake-Handy; Authentifizierung, REST, WebSockets, ADB push/shell/sync, Tunnel, UDP-Relay, Befehle, `agent.ini`-Änderungen, CLI, Wiederverbindungen (48 Prüfungen) |
| (in `make test`) | Weiterleitung von Benachrichtigungen: Die `[notify]`-Konfiguration erreicht das Handy, eine erfasste Benachrichtigung erreicht einen Viewer per WebSocket, und der Server schickt sie als JSON an einen Bark-Stub |
| `python3 tests/python_sdk.py` | Das KI-Steuerungs-SDK, die `rd`-CLI und der Skill-Runner gegen eine Fake-Handy-API: Selektoren, Screenshots im Speicher, Übergabe an die KI (exit 3), Fortsetzen mit `--from-step` |
| `QEMU=qemu-mipsel node tests/run.mjs build/mipsel` | Dasselbe mit den statischen MIPS-Router-Binaries unter User-Mode-Emulation |
| `python android/native/test/rtc_interop.py` | WebRTC-Stack des Handys ↔ aiortc: ICE, DTLS-SRTP, SCTP, dekodierte H.264-Frames, Datenkanäle |
| `python android/native/test/rtp_depack.py` | H.265- und H.264-Paketierer: bitgenauer Rundlauf durch einen unabhängigen Depaketierer + FFmpeg |
| `python tests/e2e_webrtc.py [--relay]` | Ganzer Weg: Viewer → Server → Agent → Tunnel → Handy-Stack; `--relay` erzwingt das eingebaute TURN |
| `python tests/e2e_browser.py` | Der echte Web-Client in Headless-Chrome gegen den ganzen Stack: Video, Touch, Menü, Protokoll |
| `node tests/setup.mjs build/host` | Ersteinrichtungs-Assistent (Setup-Token, Speichern, Neustart auf dem neuen Port), Login, Kopplungscodes mit dem `enroll` des echten Agents, Router-Einstellungen über das Panel, Umbenennen, Entfernen |
| `node tests/worker.mjs build/host` | Der Cloudflare Worker unter `wrangler dev` (nach `cd worker && npm install`) mit dem echten Agent: Einrichtung, Login, Kopplung, Viewer-WebSocket und Signalisierung, Befehle, Umbenennen, Entfernen |
| `sudo tests/openwrt.sh IP [VERSION]` | OpenWrt (19.07–24.10) in podman mit eigener LAN-Adresse (macvlan), mit LuCI, dem aus diesem Repository gebauten Agent-ipk und einem Server; öffne `http://IP/` von einem anderen Gerät und koppele unter Services → RemoDroid |
| `node tests/tls.mjs build/host` | HTTPS, Weiterleitung, `Secure`-Cookies, Viewer und Agent über `wss://` (vertrauenswürdige und nicht vertrauenswürdige CA), Zertifikat-Neuladen bei SIGHUP; mit `PEBBLE_DIR` ACME-Ausstellung, Wiederverwendung und automatische Verlängerung gegen Pebble (die Test-CA von Let's Encrypt) |

Die Python-Tests brauchen `aiortc` (und `playwright` + Chromium für den Browsertest).

</details>

Designdokumente: [plan.md](../../plan.md) (Architektur und Entscheidungen), [skill-plan.md](../../skill-plan.md) (KI-Steuerung),
[packaging/merlin/README.md](../../packaging/merlin/README.md) (Merlin-Plugin).
