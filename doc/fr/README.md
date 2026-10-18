<p align="center"><a href="../../README.md">English</a> | <a href="../zh/README.md">简体中文</a> | <a href="../ja/README.md">日本語</a> | <a href="../ko/README.md">한국어</a> | <a href="../es/README.md">Español</a> | <a href="../pt-BR/README.md">Português (Brasil)</a> | <a href="../de/README.md">Deutsch</a> | <b>Français</b> | <a href="../ru/README.md">Русский</a></p>

# RemoDroid

Contrôlez un téléphone Android depuis Safari sur votre iPhone avec une latence assez faible pour jouer dans le cloud.
Le téléphone reste chez vous, relié au routeur par USB. Vous pouvez voir son écran et le toucher depuis n’importe où.

<p align="center"><a href="https://remodroid.pages.dev/fr/"><img src="../../wizard/button.svg" alt="Installer RemoDroid avec l’assistant : remodroid.pages.dev" width="318" height="64"></a></p>

<p align="center"><img src="../architecture.svg" alt="Fonctionnement de RemoDroid : Safari sur l’iPhone se connecte au serveur de contrôle ; l’agent du routeur domestique pilote le téléphone Android par USB ; vidéo, son et commandes tactiles circulent par WebRTC entre Safari et le routeur, via TURN si l’UDP direct est bloqué" width="100%"></p>

**Le fonctionnement en un paragraphe.** Un petit **serveur** sur une machine publique (ou un Cloudflare Worker gratuit) héberge la page web, la connexion à votre compte et l’établissement des connexions. Un **agent** sur votre routeur communique avec le téléphone par USB grâce à son ADB intégré (sans avoir besoin d’`adb`) et lance un petit programme auxiliaire sur le téléphone. Celui-ci capture l’écran, l’encode matériellement et l’envoie par WebRTC directement à Safari ; vos gestes tactiles reviennent par le même chemin. Le routeur ne fait que transférer les paquets ; rien n’est décodé entre les deux.

<details>
<summary>Détails : chemins des médias, TURN et programmes exécutés sur le téléphone</summary>

- **Chemin des médias.** Par défaut, le téléphone envoie les médias par USB et l’agent les transmet au réseau en UDP : le téléphone n’a donc pas besoin de Wi-Fi. Avec `[phone] path = wifi` dans `agent.ini`, le téléphone utilise son propre Wi-Fi, ce qui peut être plus rapide si le routeur sert aussi de point d’accès Wi-Fi.
- **TURN.** Quand aucun chemin UDP direct n’est possible entre l’iPhone et votre domicile (NAT de l’opérateur, pare-feu stricts), les médias passent par le relais TURN intégré au serveur. Ils ne sont toujours pas décodés.
- **Sur le téléphone :** `remodroid-server.jar` (capture d’écran, MediaCodec H.265/H.264, injection d’entrées, presse-papiers, alimentation de l’écran, fichiers) et `libremodroid.so` (pile WebRTC en C : ICE, DTLS-SRTP, canaux de données SCTP, contrôle de congestion). Tous deux s’exécutent sous l’utilisateur shell via `app_process` : aucune application n’est installée. Pendant la connexion, la dalle du téléphone est éteinte pour économiser l’énergie, et le son est diffusé sur votre iPhone.
- Notes et décisions de conception : [`plan.md`](../../plan.md).

</details>

## Prérequis

RemoDroid est léger : le téléphone effectue les tâches lourdes (capture et encodage matériel), le routeur ne fait que transférer les paquets et le serveur établit les connexions. Le plus important est le **débit montant à votre domicile**.

| | Minimum | Recommandé | Ressources utilisées |
|---|---|---|---|
| **Serveur** | Toute machine Linux avec une IP publique : 1 CPU, 128 MB de RAM et quelques MB de disque. Ou un compte Cloudflare gratuit | Une petite VM dans la région cloud **la plus proche de votre domicile** (elle transporte le flux lorsque TURN est nécessaire), avec assez de trafic mensuel | Environ 2 MB de RAM et presque aucune charge CPU. Bande passante utilisée uniquement lorsqu’une session bascule sur TURN ; tout le flux le traverse alors (environ 4,5 GB par heure à 10 Mbps) |
| **Routeur** | OpenWrt, Linux (Debian, Ubuntu, CentOS, Alpine) ou ASUS Merlin avec Koolshare, un port USB et environ 2 MB de stockage libre. CPU : x86, ARM ou MIPS | Un routeur ARM (aarch64, ARMv7) ou x86. Un MIPS peu puissant fonctionne, mais peut ne pas suivre aux débits les plus élevés | Environ 2 MB de RAM. CPU utilisé seulement pour déplacer les paquets entre USB et UDP ; aucun décodage |
| **Téléphone Android** | Android 8 ou ultérieur, débogage USB, encodeur matériel H.264 | Android 12 ou ultérieur (pour le son) avec un encodeur matériel H.265 | Son propre encodeur matériel ; l’écran s’éteint pendant la connexion |
| **iPhone** | Safari | Un iOS récent pour H.265 par WebRTC (les anciens iOS reçoivent automatiquement du H.264) | Décodage vidéo matériel |
| **Réseau** | 3–5 Mbps en montant au domicile (720p) | 10–20 Mbps en montant au domicile et autant en descendant sur l’iPhone (1080p, 60 fps) | 1–20 Mbps par flux (plafond par défaut : 20 Mbps, 60 fps) ; la qualité s’adapte automatiquement |

La latence attendue est de 40–80 ms lorsque l’iPhone atteint directement votre domicile, et d’environ 70–120 ms via TURN.
Les chiffres de mémoire proviennent d’un serveur en fonctionnement et d’un agent sur RT-AC86U.

## Installation

<p align="center"><a href="https://remodroid.pages.dev/fr/"><img src="../../wizard/button.svg" alt="Installer RemoDroid avec l’assistant : remodroid.pages.dev" width="318" height="64"></a></p>

**L’assistant vous guide de bout en bout avec des boutons de copie.** Vous préférez rester ici ? Suivez ces cinq étapes, les mêmes qu’au bas de l’image.

### 1. Serveur

Sur votre serveur Linux :

```sh
curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh
```

Le script choisit le bon paquet, l’installe, démarre le serveur et affiche un lien comme `http://<ip>:5580/wizard.html?token=…`.

<!-- downloads:server -->
#### Télécharger le serveur (v0.4)

Choisissez la ligne correspondant au CPU de la machine (`uname -m` : `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) et la colonne de son système. OpenWrt : prenez la version indiquée par `opkg print-architecture`. *Tout Linux* contient le serveur et l’agent.

| Serveur | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Tout Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Serveur .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Serveur .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Serveur .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Serveur .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Serveur .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Serveur .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Serveur .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Serveur .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Serveur .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Serveur .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 bits ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Serveur .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Serveur .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Serveur .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Serveur .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Serveur .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Serveur .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Serveur .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Serveur .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Serveur .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Serveur .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Serveur .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Serveur .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Sommes de contrôle : [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:server -->

Installez-le et démarrez le service :

```sh
sudo apt install ./remodroid-server_*.deb                  # Debian, Ubuntu
sudo dnf install ./remodroid-server-*.rpm                  # CentOS, Fedora
sudo apk add --allow-untrusted ./remodroid-server_*.apk    # Alpine
opkg install ./remodroid-server_*.ipk                      # OpenWrt

sudo systemctl enable --now remodroid-server               # systemd
rc-update add remodroid-server && rc-service remodroid-server start            # Alpine
/etc/init.d/remodroid-server enable && /etc/init.d/remodroid-server start      # OpenWrt
```

Ouvrez ensuite `http://<server>:5580/wizard.html?token=…`. Le jeton se trouve dans `/var/lib/remodroid/setup-token`.

<details>
<summary>Installation manuelle sur tout Linux (.tar.gz)</summary>

L’archive contient `bin/`, `web/`, `phone/` et `etc/` (exemples de fichiers ini et unités de service). Pour le serveur :

```sh
sudo install -m 0755 bin/remodroid-server /usr/bin/
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r web /usr/share/remodroid/web
sudo install -m 0640 etc/server.ini.example /etc/remodroid/server.ini
remodroid-server -c /etc/remodroid/server.ini
```

`etc/remodroid-server.service` est une unité systemd que vous pouvez copier dans `/etc/systemd/system/`.

</details>

**Configurez-le.** Dans l’assistant, renseignez le port web, votre **domaine** (pour HTTPS automatique avec Let's Encrypt), les identifiants administrateur et le port STUN/TURN. Le serveur redémarre sur vos ports et le panneau est prêt.

Ouvrez ces ports dans le pare-feu : **TCP 5580** (configuration initiale uniquement), votre port web (**80**) et **443** si vous utilisez un domaine, **UDP 3478** et **UDP 49152–49252** (TURN).

<details>
<summary>Pas de serveur ? Utilisez Cloudflare</summary>

1. Dans le tableau de bord Cloudflare, créez une clé TURN : **Temps réel → Serveur TURN**.
2. Déployez `worker/` : `cd worker && npm install && npx wrangler deploy`, ou importez votre fork dans Cloudflare en définissant `worker` comme répertoire racine.
3. Ouvrez `https://remodroid.<you>.workers.dev`. L’assistant demande les identifiants et la clé TURN.

Le Worker propose le même panneau (connexion, routeurs, signalisation) sur un Durable Object avec stockage intégré.
Sautez l’étape du pare-feu ; le TURN de Cloudflare assure le relais.

</details>

<details>
<summary>Réglages du serveur après la configuration initiale</summary>

L’assistant écrit `/etc/remodroid/server.ini`. Ensuite, cette même page permet de modifier les réglages après connexion, et l’onglet **Serveur** du panneau permet de changer un réglage à la fois (domaine, ports, identifiants, liste STUN/TURN).
`remodroid-server -t` vérifie le fichier sans rien démarrer. Les certificats HTTPS sont renouvelés automatiquement.
Les paquets conservent votre `server.ini` lors des mises à niveau.

</details>

### 2. Agent sur le routeur

OpenWrt, Alpine :

```sh
wget -qO- https://remodroid.pages.dev/install_agent.sh | sh
```

Debian, Ubuntu, CentOS, Fedora :

```sh
curl -fsSL https://remodroid.pages.dev/install_agent.sh | sudo sh
```

<!-- downloads:agent -->
#### Télécharger l’agent (v0.4)

Choisissez la ligne correspondant au CPU de la machine (`uname -m` : `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) et la colonne de son système. OpenWrt : prenez la version indiquée par `opkg print-architecture`. *Tout Linux* contient le serveur et l’agent.

| Agent | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Tout Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Agent .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Agent .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Agent .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 bits ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Agent .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Agent .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Agent .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agent .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agent .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agent .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agent .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Serveur + Agent .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Installateurs hors ligne Koolshare / KoolCenter : [Merlin armv7](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-armv7.tar.gz) · [Merlin aarch64](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-aarch64.tar.gz). RT-AC86U et RT-AX88U : choisissez aarch64. Ce sont des extensions contenant uniquement l’agent, pas des paquets OpenWrt.

Sommes de contrôle : [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:agent -->

**Quel fichier OpenWrt ?** Trouvez votre routeur et son CPU dans [doc/openwrt.md](../../doc/openwrt.md), qui indique aussi les versions compatibles d’OpenWrt.

**OpenWrt avec LuCI, sans ligne de commande :** dans LuCI, ouvrez **Système → Logiciels → Téléverser un paquet…**, choisissez le `.ipk` de votre routeur et installez-le. Une page **Services → RemoDroid** apparaît : elle associe le routeur à votre panneau (étape 3), démarre et arrête l’agent, et indique si le panneau et le téléphone sont connectés.

Sur les autres systèmes, installez-le et démarrez le service :

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
<summary>Installation manuelle sur tout Linux (.tar.gz)</summary>

Pour l’agent, depuis l’archive décompressée :

```sh
sudo install -m 0755 bin/remodroid-agent /usr/bin/
sudo ln -s remodroid-agent /usr/bin/remodroid
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r phone/. /usr/share/remodroid/
sudo install -m 0600 etc/agent.ini.example /etc/remodroid/agent.ini
remodroid-agent -c /etc/remodroid/agent.ini
```

`etc/remodroid-agent.service` (systemd) et `etc/remodroid-agent.init` (OpenWrt procd) le lancent au démarrage.

</details>

<details>
<summary>ASUS Merlin / Koolshare</summary>

Prenez le paquet Merlin ci-dessus correspondant au CPU de votre routeur (`uname -m`) :

- `remodroid-<version>-merlin-aarch64.tar.gz` pour `aarch64` : modèles HND 64 bits tels que RT-AC86U et RT-AX88U
- `remodroid-<version>-merlin-armv7.tar.gz` pour `armv7l` : modèles ARM 32 bits tels que RT-AC68U

Installez-le dans **Centre logiciel → Installation manuelle**. L’installateur refuse les paquets destinés à un autre CPU.
Il ajoute une page au routeur pour les réglages, le démarrage/l’arrêt, les journaux et le lancement automatique facultatif. L’installation ne démarre pas l’agent. Asuswrt-Merlin d’origine sans le centre logiciel Koolshare/KoolCenter n’est pas pris en charge.
Compilation, migration et limites : [Paquetage Merlin](../../packaging/merlin/README.md).

</details>

### 3. Enregistrer le routeur

Dans le panneau, ouvrez **Agents → Ajouter**. Une commande avec un code à usage unique s’affiche ; exécutez-la sur le routeur :

```sh
remodroid enroll --panel https://your.panel --code X5SX-K3M7
```

Sur OpenWrt, vous pouvez aussi coller cette commande dans **Services → RemoDroid → Associer** dans LuCI.

Le code ne fonctionne qu’une fois, pendant 10 minutes. Le routeur apparaît ensuite en ligne ; touchez-le pour modifier ses réglages.

### 4. Brancher le téléphone

Sur le téléphone, activez **Options pour les développeurs → Débogage USB**. Branchez-le au port USB du routeur et touchez **Autoriser** à l’invite « Autoriser le débogage USB ? » (cochez « Toujours autoriser »). C’est la seule fois où vous devez manipuler le téléphone.

### 5. Ouvrir sur l’iPhone

Ouvrez votre panneau dans Safari, connectez-vous, puis choisissez **Partager → Sur l’écran d’accueil**. Lancé depuis l’écran d’accueil, il fonctionne en plein écran (Safari ne peut pas masquer ses barres pour une page normale). Touchez votre téléphone pour vous connecter.

## Utilisation

- **Le tactile** fonctionne comme sur le téléphone, y compris le multipoint. La **bulle flottante** ouvre le menu : verrouiller, reconnecter, plein écran, son, envoi de photos, presse-papiers et réglages.
- **Le son** provient du téléphone (Android 12+) et son haut-parleur devient silencieux pendant la connexion. Touchez une fois l’image pour activer le son dans Safari ; le commutateur **Son** du menu rend la lecture au téléphone.
- **Mettez à jour** le serveur ou le routeur plus tard avec une seule commande. Elle met les composants installés à la dernière version publiée, conserve `server.ini` / `agent.ini` et les redémarre :

  ```sh
  curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh    # OpenWrt: wget -qO- … | sh
  ```

<details>
<summary>Commandes du routeur</summary>

Sur le routeur, ces commandes communiquent avec l’agent en cours d’exécution (sans adb) :

```sh
remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server
```

</details>

## Fonctions supplémentaires

<details>
<summary><b>Contrôle par IA</b> : laissez une IA lire l’écran et toucher à votre place</summary>

L’IA pilote le téléphone via REST sous `/api/v1/devices/{id}/` :

- `GET …/ui` renvoie l’écran en JSON (via UiAutomation dans le processus sur le téléphone ; `uiautomator dump` en solution de repli).
- `GET …/screenshot?format=jpeg&max_edge=1280` renvoie l’image (redimensionnée par le GPU, compressée en C sur le téléphone, jamais écrite sur disque).
- `POST …/actions` touche, saisit, fait défiler et attend à l’aide de sélecteurs.

L’agent du routeur fournit l’API (`[api] listen` + `token` dans `agent.ini`) et le serveur la relaie (`remodroid-server gentoken NAME`). Un processus que l’IA a résolu une fois est enregistré sous `skills/` et rejoué par Python sans appel au modèle ; face à une situation nouvelle, il rend l’écran à l’IA.

```sh
export PYTHONPATH=python RD_URL=http://192.168.1.1:8790 RD_TOKEN=…
python3 -m remodroid ui                      # écran sous forme de plan
python3 -m remodroid tap --rid search --wait-change
python3 skills/settings-search/run.py --query wifi
```

Conception : [skill-plan.md](../../skill-plan.md). Écriture de compétences : [skills/README.md](../../skills/README.md).

</details>

<details>
<summary><b>Notifications du téléphone</b> : recevez-les sur l’iPhone ou via Bark</summary>

Le programme auxiliaire du téléphone interroge régulièrement `dumpsys notification` (le seul contenu de notification visible par l’utilisateur shell), ignore les notifications persistantes et peu importantes, puis envoie chaque nouvelle notification aux clients connectés via le WebSocket du visualiseur, ainsi qu’à [Bark](https://github.com/Finb/Bark) si le serveur possède un `bark_url`. Vous pouvez ensuite appeler l’API de contrôle par IA (ou une compétence) pour agir sur cette notification.

- Routeur, `agent.ini` : `[notify] enabled = true`, avec `min_importance` et une liste `ignore`.
- Serveur, `server.ini` : `[notify] bark_url = https://api.day.app/<key>` (ou par routeur sous `[device_notify]`).
  La clé Bark reste sur le serveur et n’est jamais journalisée. L’envoi par WebSocket ne nécessite aucune configuration du serveur.

Les exemples de configuration sont dans `deploy/`.

</details>

## État

Tous les tests du tableau ci-dessous réussissent, y compris avec un véritable Chrome comme visualiseur, et le son a été vérifié sur un Sony SO-51A (Android 12). Pas encore vérifiés sur matériel réel (jalon M0 du plan) : le serveur Java sur divers téléphones (capture, paramètres de l’encodeur, entrées, presse-papiers, alimentation de l’écran), ADB USB sur les routeurs, particularités de Safari (prise en charge H.265, décodage matériel), et contrôle de congestion sur des réseaux réels.

## Développement

<details>
<summary>Contenu du dépôt</summary>

| Chemin | Contenu | Langage |
|---|---|---|
| `server/` | Serveur de contrôle : page de connexion + cookies (authentification Basic pour les routeurs), API REST, WebSockets visualiseur/agent, relais TURN intégré, HTTPS facultatif avec certificats automatiques Let's Encrypt (ACME) | C + libev + mbedTLS |
| `agent/` | Agent du routeur : client ADB intégré (usbfs), supervision du téléphone, relais UDP, client WebSocket, interface en ligne de commande | C + libev |
| `android/native/` | Pile WebRTC du téléphone : ICE + client TURN, DTLS-SRTP, canaux de données SCTP, RTP H.265/H.264, contrôle de congestion | C |
| `android/java/` | Serveur du téléphone : capture d’écran, MediaCodec, injection d’entrées, presse-papiers, alimentation de l’écran, fichiers, logcat | Java (app_process) |
| `web/` | Client iPhone (PWA) : vidéo, son, tactile multipoint, menu à bulle flottante, envoi de photos, presse-papiers, tous les réglages ; `panel.html` (onglets Appareils, Agents, Serveur) et `wizard.html` (configuration initiale) | HTML/CSS/JS |
| `worker/` | Même panneau sur Cloudflare Workers + un Durable Object, avec le TURN de Cloudflare | JS |
| `wizard/` | Site d’installation (Cloudflare Pages, remodroid.pages.dev), `install_server.sh`, `install_agent.sh`, `update.sh` | HTML, sh |
| `common/` | Code C partagé : trames WebSocket, trames de tunnel, STUN, enveloppe JSON, ini, utilitaires | C |
| `third_party/` | Bibliothèques incluses dans le dépôt, liées statiquement : libev, inih, cJSON, picohttpparser, crypt_blowfish, mbedTLS, libsrtp2, usrsctp | C |
| `deploy/` | Exemples `server.ini` / `agent.ini`, unités systemd, scripts OpenRC et procd | |
| `packaging/`, `.github/workflows/` | Définitions de paquets nFPM (deb, rpm, apk, ipk) et processus CI de compilation/publication | |
| `python/`, `skills/`, `.claude/skills/phone/` | Contrôle par IA : SDK `remodroid` et CLI `rd`, compétences rejouables et compétence Claude Code qui les utilise | Python |
| `tests/` | Tests de bout en bout | Node, Python |
| `demo/` | Maquette du panneau d’origine (hors ligne, sans serveur) | HTML |

</details>

<details>
<summary>Compilation</summary>

Compilation sur l’hôte (développement et tests) :

```sh
make            # build/host/remodroid-server, build/host/remodroid-agent
make rtc-tests  # build/host/rtc/rtc_test, rtp_dump (pile du téléphone sur l’hôte)
```

Binaires statiques de distribution pour routeurs et serveurs (Zig comme compilateur croisé, musl, tout est lié) :

```sh
make dist                   # dans le conteneur de compilation (build/Dockerfile, nécessite podman/docker)
./build/dist.sh mipsel      # ou directement, avec `zig` dans PATH ; cibles : x86 x86_64 armv7 aarch64 mipsel mips
```

Les résultats se trouvent dans `build/out/<arch>/` : environ 0,8 MB par binaire (1,2 MB sur MIPS).

Fichiers du téléphone (`remodroid-server.jar` + `libremodroid.so` par ABI) :

```sh
ANDROID_JAR=.../platforms/android-34/android.jar D8=.../build-tools/34.0.0/d8 NDK=.../android-ndk-r27c \
  ./android/build.sh        # build/android/remodroid-server.jar, build/android/<abi>/libremodroid.so
```

</details>

<details>
<summary>Tests</summary>

| Commande | Couverture |
|---|---|
| `make test` | Plan de contrôle de bout en bout : vrais serveur + agent, faux adbd fidèle au protocole (authentification RSA vérifiée avec OpenSSL), faux téléphone ; authentification, REST, WebSockets, ADB push/shell/sync, tunnel, relais UDP, commandes, modifications d’`agent.ini`, CLI, reconnexions (48 vérifications) |
| (dans `make test`) | Transmission des notifications : la configuration `[notify]` atteint le téléphone, une notification capturée parvient au visualiseur par WebSocket et le serveur l’envoie en JSON à un point de terminaison Bark simulé |
| `python3 tests/python_sdk.py` | SDK de contrôle par IA, CLI `rd` et exécuteur de compétences avec une fausse API de téléphone : sélecteurs, captures en mémoire, transfert à l’IA (code de sortie 3), reprise avec `--from-step` |
| `QEMU=qemu-mipsel node tests/run.mjs build/mipsel` | Idem, sur les binaires statiques de routeur MIPS sous émulation en mode utilisateur |
| `python android/native/test/rtc_interop.py` | Pile WebRTC du téléphone ↔ aiortc : ICE, DTLS-SRTP, SCTP, images H.264 décodées, canaux de données |
| `python android/native/test/rtp_depack.py` | Paquetiseurs H.265 et H.264 : aller-retour exact bit à bit via un dépaquetiseur indépendant + FFmpeg |
| `python tests/e2e_webrtc.py [--relay]` | Chemin complet : visualiseur → serveur → agent → tunnel → pile du téléphone ; `--relay` impose le TURN intégré |
| `python tests/e2e_browser.py` | Vrai client web dans Chrome sans interface avec toute la pile : vidéo, tactile, menu, protocole |
| `node tests/setup.mjs build/host` | Assistant initial (jeton de configuration, enregistrement, redémarrage sur le nouveau port), connexion, codes d’enregistrement avec le vrai `enroll` de l’agent, réglages du routeur via le panneau, renommage, suppression |
| `node tests/worker.mjs build/host` | Cloudflare Worker sous `wrangler dev` (après `cd worker && npm install`) avec le vrai agent : configuration, connexion, enregistrement, WebSocket du visualiseur et signalisation, commandes, renommage, suppression |
| `sudo tests/openwrt.sh IP [VERSION]` | OpenWrt (19.07–24.10) dans podman avec sa propre adresse LAN (macvlan), LuCI, l’ipk de l’agent compilé depuis ce dépôt et un serveur ; ouvrez `http://IP/` depuis un autre appareil et associez-le dans Services → RemoDroid |
| `node tests/tls.mjs build/host` | HTTPS, redirection, cookies `Secure`, visualiseur et agent via `wss://` (CA fiables ou non), rechargement des certificats sur SIGHUP ; avec `PEBBLE_DIR`, émission ACME, réutilisation et renouvellement automatique avec Pebble (CA de test de Let's Encrypt) |

Les tests Python nécessitent `aiortc` (et `playwright` + Chromium pour le test navigateur).

</details>

Documents de conception : [plan.md](../../plan.md) (architecture et décisions), [skill-plan.md](../../skill-plan.md) (contrôle par IA),
[packaging/merlin/README.md](../../packaging/merlin/README.md) (extension Merlin).
