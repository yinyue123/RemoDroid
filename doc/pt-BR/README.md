<p align="center"><a href="../../README.md">English</a> · <a href="../zh/README.md">简体中文</a> · <a href="../ja/README.md">日本語</a> · <a href="../ko/README.md">한국어</a> · <a href="../es/README.md">Español</a> · <b>Português (Brasil)</b> · <a href="../de/README.md">Deutsch</a> · <a href="../fr/README.md">Français</a> · <a href="../ru/README.md">Русский</a></p>

# RemoDroid

Controle um celular Android pelo Safari do iPhone com latência baixa o suficiente para jogar na nuvem.
O celular fica em casa, conectado ao roteador por USB. Você pode ver a tela e tocá-la de qualquer lugar.

<p align="center"><a href="https://remodroid.pages.dev/pt-BR/"><img src="../../wizard/button.svg" alt="Instale o RemoDroid com o assistente: remodroid.pages.dev" width="318" height="64"></a></p>

<p align="center"><img src="../architecture.svg" alt="Como o RemoDroid funciona: o Safari no iPhone faz login no servidor de controle; o agente no roteador de casa controla o celular Android por USB; vídeo, som e toques trafegam por WebRTC entre o Safari e o roteador, via TURN quando o UDP direto é bloqueado" width="100%"></p>

**Como funciona, em um parágrafo.** Um pequeno **servidor** em uma máquina pública (ou um Cloudflare Worker gratuito) hospeda a página web, o login e o estabelecimento das conexões. Um **agente** no roteador de casa se comunica com o celular por USB usando seu próprio ADB integrado (sem precisar de `adb`) e inicia um pequeno programa auxiliar no celular. Ele captura a tela, codifica por hardware e envia por WebRTC diretamente ao Safari, com os toques voltando pelo mesmo caminho. O roteador apenas encaminha pacotes; nada é decodificado no meio.

<details>
<summary>Mais detalhes: caminhos de mídia, TURN e o que roda no celular</summary>

- **Caminho da mídia.** Por padrão, o celular envia a mídia pelo cabo USB e o agente a coloca na rede como UDP, então o celular não precisa de Wi-Fi. Com `[phone] path = wifi` em `agent.ini`, ele usa seu próprio Wi-Fi, o que pode ser mais rápido quando o roteador também é o ponto de acesso Wi-Fi.
- **TURN.** Quando não há um caminho UDP direto entre o iPhone e sua casa (NAT da operadora, firewalls restritivos), a mídia passa pelo relay TURN integrado ao servidor. Ela continua sem ser decodificada.
- **No celular:** `remodroid-server.jar` (captura de tela, MediaCodec H.265/H.264, injeção de entrada, área de transferência, energia da tela, arquivos) e `libremodroid.so` (uma pilha WebRTC em C: ICE, DTLS-SRTP, canais de dados SCTP, controle de congestionamento). Ambos rodam como usuário shell por meio de `app_process`, então nenhum aplicativo é instalado. Durante a conexão, o painel do celular é desligado para economizar energia e o som passa a tocar no iPhone.
- Notas e decisões de projeto: [`plan.md`](../../plan.md).

</details>

## O que você precisa

O RemoDroid é leve: o celular faz o trabalho pesado (captura e codificação por hardware), o roteador apenas encaminha pacotes e o servidor só estabelece conexões. O mais importante é a **velocidade de upload em casa**.

| | Mínimo | Recomendado | Recursos usados |
|---|---|---|---|
| **Servidor** | Qualquer máquina Linux com IP público: 1 CPU, 128 MB de RAM e alguns MB de disco. Ou uma conta gratuita do Cloudflare | Uma pequena VM na região de nuvem **mais próxima da sua casa** (ela transporta o fluxo quando o TURN é necessário), com tráfego mensal suficiente | Cerca de 2 MB de RAM e quase nenhuma CPU. Só usa banda quando a sessão recorre ao TURN; nesse caso, todo o fluxo passa por ele (cerca de 4,5 GB por hora a 10 Mbps) |
| **Roteador** | OpenWrt, Linux (Debian, Ubuntu, CentOS, Alpine) ou ASUS Merlin com Koolshare, uma porta USB e cerca de 2 MB de armazenamento livre. CPU: x86, ARM ou MIPS | Um roteador ARM (aarch64, ARMv7) ou x86. Um MIPS fraco funciona, mas pode não acompanhar as maiores taxas de bits | Cerca de 2 MB de RAM. A CPU só move pacotes entre USB e UDP; nada é decodificado |
| **Celular Android** | Android 8 ou posterior, depuração USB e codificador H.264 por hardware | Android 12 ou posterior (para som) com codificador H.265 por hardware | Seu próprio codificador por hardware; a tela fica desligada durante a conexão |
| **iPhone** | Safari | Um iOS recente para H.265 por WebRTC (iOS antigos recebem H.264 automaticamente) | Decodificação de vídeo por hardware |
| **Rede** | 3–5 Mbps de upload em casa (720p) | 10–20 Mbps de upload em casa e o mesmo download no iPhone (1080p, 60 fps) | 1–20 Mbps por fluxo (limite padrão de 20 Mbps, 60 fps); a qualidade se ajusta automaticamente |

A latência esperada é de 40–80 ms quando o iPhone chega diretamente à sua casa, e cerca de 70–120 ms pelo TURN.
Os números de memória vêm de um servidor em execução e de um agente em um RT-AC86U.

## Instalação

<p align="center"><a href="https://remodroid.pages.dev/pt-BR/"><img src="../../wizard/button.svg" alt="Instale o RemoDroid com o assistente: remodroid.pages.dev" width="318" height="64"></a></p>

**O assistente guia você em todas as etapas com botões para copiar.** Prefere continuar aqui? Siga estas cinco etapas, as mesmas da parte inferior da imagem.

### 1. Servidor

No seu servidor Linux:

```sh
curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh
```

Ele escolhe o pacote correto, instala, inicia o servidor e exibe um link como `http://<ip>:5580/wizard.html?token=…`.

<!-- downloads:server -->
#### Baixar o servidor (v0.4)

Escolha a linha da CPU da máquina (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) e a coluna do sistema. No OpenWrt, use a compilação listada por `opkg print-architecture`. *Qualquer Linux* contém tanto o servidor quanto o agente.

| Servidor | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Qualquer Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Servidor .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Servidor .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Servidor .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Servidor .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Servidor .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Servidor .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Servidor .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Servidor .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Servidor .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Servidor .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 bits ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Servidor .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Servidor .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Servidor .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Servidor .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Servidor .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Servidor .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Servidor .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Servidor .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Servidor .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Servidor .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Servidor .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Servidor .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Somas de verificação: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:server -->

Instale e inicie o serviço:

```sh
sudo apt install ./remodroid-server_*.deb                  # Debian, Ubuntu
sudo dnf install ./remodroid-server-*.rpm                  # CentOS, Fedora
sudo apk add --allow-untrusted ./remodroid-server_*.apk    # Alpine
opkg install ./remodroid-server_*.ipk                      # OpenWrt

sudo systemctl enable --now remodroid-server               # systemd
rc-update add remodroid-server && rc-service remodroid-server start            # Alpine
/etc/init.d/remodroid-server enable && /etc/init.d/remodroid-server start      # OpenWrt
```

Depois abra `http://<server>:5580/wizard.html?token=…`. O token está em `/var/lib/remodroid/setup-token`.

<details>
<summary>Instalação manual em qualquer Linux (.tar.gz)</summary>

O arquivo contém `bin/`, `web/`, `phone/` e `etc/` (arquivos ini de exemplo e unidades de serviço). Para o servidor:

```sh
sudo install -m 0755 bin/remodroid-server /usr/bin/
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r web /usr/share/remodroid/web
sudo install -m 0640 etc/server.ini.example /etc/remodroid/server.ini
remodroid-server -c /etc/remodroid/server.ini
```

`etc/remodroid-server.service` é uma unidade systemd que você pode copiar para `/etc/systemd/system/`.

</details>

**Configure.** No assistente, informe a porta web, seu **domínio** (para HTTPS automático pelo Let's Encrypt), as credenciais do administrador e a porta STUN/TURN. O servidor reinicia nas suas portas e o painel fica pronto.

Abra estas portas no firewall: **TCP 5580** (apenas para a configuração inicial), sua porta web (**80**) e **443** se usar um domínio, **UDP 3478** e **UDP 49152–49252** (TURN).

<details>
<summary>Sem servidor? Use o Cloudflare</summary>

1. No painel do Cloudflare, crie uma chave TURN: **Tempo real → Servidor TURN**.
2. Implante `worker/`: `cd worker && npm install && npx wrangler deploy`, ou importe seu fork no Cloudflare com o diretório raiz `worker`.
3. Abra `https://remodroid.<you>.workers.dev`. O assistente solicita as credenciais e a chave TURN.

O Worker oferece o mesmo painel (login, roteadores, sinalização), rodando em um Durable Object com armazenamento integrado.
Pule a etapa do firewall; o TURN do Cloudflare faz o relay.

</details>

<details>
<summary>Configurações do servidor após a configuração inicial</summary>

O assistente grava `/etc/remodroid/server.ini`. Depois, a mesma página permite editar as configurações após o login, e a aba **Servidor** do painel altera uma configuração por vez (domínio, portas, credenciais, lista STUN/TURN).
`remodroid-server -t` verifica o arquivo sem iniciar nada. Os certificados HTTPS são renovados automaticamente.
Os pacotes preservam seu `server.ini` nas atualizações.

</details>

### 2. Agente no roteador

OpenWrt, Alpine:

```sh
wget -qO- https://remodroid.pages.dev/install_agent.sh | sh
```

Debian, Ubuntu, CentOS, Fedora:

```sh
curl -fsSL https://remodroid.pages.dev/install_agent.sh | sudo sh
```

<!-- downloads:agent -->
#### Baixar o agente (v0.4)

Escolha a linha da CPU da máquina (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) e a coluna do sistema. No OpenWrt, use a compilação listada por `opkg print-architecture`. *Qualquer Linux* contém tanto o servidor quanto o agente.

| Agente | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Qualquer Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agente .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agente .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agente .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agente .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agente .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agente .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agente .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="Agente .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="Agente .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="Agente .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32 bits ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agente .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agente .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agente .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="Agente .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="Agente .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="Agente .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32 bits</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="Agente .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="Agente .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="Agente .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agente .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agente .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="Agente .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="Servidor + Agente .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Instaladores offline do Koolshare / KoolCenter: [Merlin armv7](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-armv7.tar.gz) · [Merlin aarch64](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-aarch64.tar.gz). RT-AC86U e RT-AX88U: escolha aarch64. São plugins que contêm apenas o agente, não pacotes OpenWrt.

Somas de verificação: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads:agent -->

**Qual arquivo OpenWrt?** Encontre seu roteador e sua CPU em [doc/openwrt.md](../../doc/openwrt.md), que também lista as versões compatíveis do OpenWrt.

**OpenWrt com LuCI, sem linha de comando:** no LuCI, abra **Sistema → Software → Enviar pacote…**, escolha o `.ipk` do seu roteador e instale. Surgirá uma página **Serviços → RemoDroid**: ela pareia o roteador com seu painel (etapa 3), inicia e para o agente e mostra se o painel e o celular estão conectados.

Nos outros sistemas, instale e inicie o serviço:

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
<summary>Instalação manual em qualquer Linux (.tar.gz)</summary>

Para o agente, a partir do arquivo descompactado:

```sh
sudo install -m 0755 bin/remodroid-agent /usr/bin/
sudo ln -s remodroid-agent /usr/bin/remodroid
sudo mkdir -p /usr/share/remodroid /etc/remodroid
sudo cp -r phone/. /usr/share/remodroid/
sudo install -m 0600 etc/agent.ini.example /etc/remodroid/agent.ini
remodroid-agent -c /etc/remodroid/agent.ini
```

`etc/remodroid-agent.service` (systemd) e `etc/remodroid-agent.init` (OpenWrt procd) iniciam o agente durante a inicialização.

</details>

<details>
<summary>ASUS Merlin / Koolshare</summary>

Use o pacote Merlin acima que corresponde à CPU do seu roteador (`uname -m`):

- `remodroid-<version>-merlin-aarch64.tar.gz` para `aarch64`: modelos HND de 64 bits, como RT-AC86U e RT-AX88U
- `remodroid-<version>-merlin-armv7.tar.gz` para `armv7l`: modelos ARM de 32 bits, como RT-AC68U

Instale em **Central de software → Instalação manual**. O instalador recusa pacotes para outra CPU.
Ele adiciona uma página ao roteador para configurações, iniciar/parar, logs e inicialização automática opcional. A instalação não inicia o agente. O Asuswrt-Merlin original sem a central de software Koolshare/KoolCenter não é compatível.
Compilação, migração e limitações: [Empacotamento Merlin](../../packaging/merlin/README.md).

</details>

### 3. Registrar o roteador

No painel, abra **Agentes → Adicionar**. Ele mostra um comando com um código de uso único; execute-o no roteador:

```sh
remodroid enroll --panel https://your.panel --code X5SX-K3M7
```

No OpenWrt, você também pode colar o mesmo comando em **Serviços → RemoDroid → Parear** no LuCI.

O código funciona uma vez, por 10 minutos. O roteador aparecerá online; toque nele para alterar as configurações.

### 4. Conectar o celular

No celular, ative **Opções do desenvolvedor → Depuração USB**. Conecte-o à porta USB do roteador e toque em **Permitir** no aviso “Permitir depuração USB?” (marque “Sempre permitir”). Essa é a única vez que você precisa mexer no celular.

### 5. Abrir no iPhone

Abra o painel no Safari, faça login e selecione **Compartilhar → Adicionar à Tela de Início**. Ao iniciar pela Tela de Início, ele roda em tela cheia (o Safari não consegue ocultar suas barras em uma página normal). Toque no seu celular para conectar.

## Uso

- **Os toques** funcionam como no celular, incluindo multitoque. A **bola flutuante** abre o menu: bloquear, reconectar, tela cheia, som, envio de fotos, área de transferência e configurações.
- **O som** vem do celular (Android 12+) e o alto-falante dele fica mudo durante a conexão. Toque uma vez na imagem para ativar o som no Safari; o botão **Som** do menu devolve a reprodução ao celular.
- **Atualize** o servidor ou roteador depois com um único comando. Ele atualiza os componentes instalados para a versão mais recente, preserva `server.ini` / `agent.ini` e os reinicia:

  ```sh
  curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh    # OpenWrt: wget -qO- … | sh
  ```

<details>
<summary>Comandos do roteador</summary>

No roteador, estes comandos se comunicam com o agente em execução (sem precisar de adb):

```sh
remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server
```

</details>

## Extras

<details>
<summary><b>Controle por IA</b>: deixe uma IA ler a tela e tocar por você</summary>

A IA opera o celular por REST em `/api/v1/devices/{id}/`:

- `GET …/ui` retorna a tela em JSON (via UiAutomation dentro do processo no celular; `uiautomator dump` como alternativa).
- `GET …/screenshot?format=jpeg&max_edge=1280` retorna a imagem (redimensionada pela GPU, comprimida em C no celular, nunca gravada em disco).
- `POST …/actions` toca, digita, rola e aguarda usando seletores.

O agente do roteador fornece a API (`[api] listen` + `token` em `agent.ini`) e o servidor a retransmite (`remodroid-server gentoken NAME`). Um fluxo que a IA resolve uma vez é salvo em `skills/` e reproduzido pelo Python sem chamadas ao modelo; ao encontrar algo novo, ele devolve a tela à IA.

```sh
export PYTHONPATH=python RD_URL=http://192.168.1.1:8790 RD_TOKEN=…
python3 -m remodroid ui                      # a tela como estrutura hierárquica
python3 -m remodroid tap --rid search --wait-change
python3 skills/settings-search/run.py --query wifi
```

Projeto: [skill-plan.md](../../skill-plan.md). Como escrever habilidades: [skills/README.md](../../skills/README.md).

</details>

<details>
<summary><b>Notificações do celular</b>: receba no iPhone ou pelo Bark</summary>

O programa auxiliar no celular consulta periodicamente `dumpsys notification` (o único conteúdo de notificações visível ao usuário shell), descarta as persistentes e de baixa importância e envia cada nova notificação aos clientes conectados pelo WebSocket do visualizador, e ao [Bark](https://github.com/Finb/Bark) quando o servidor tem um `bark_url`. A partir daí, você pode chamar a API de controle por IA (ou uma habilidade) para agir sobre ela.

- Roteador, `agent.ini`: `[notify] enabled = true`, além de `min_importance` e uma lista `ignore`.
- Servidor, `server.ini`: `[notify] bark_url = https://api.day.app/<key>` (ou por roteador em `[device_notify]`).
  A chave do Bark permanece no servidor e nunca é registrada em logs. O envio por WebSocket dispensa configuração no servidor.

As configurações de exemplo estão em `deploy/`.

</details>

## Estado

Todos os testes da tabela abaixo passam, inclusive com Chrome real como visualizador, e o som foi verificado em um Sony SO-51A (Android 12). Ainda não verificados em hardware real (marco M0 do plano): o servidor Java em vários celulares (captura, configurações do codificador, entrada, área de transferência, energia da tela), ADB USB nos roteadores, particularidades do Safari (suporte a H.265, decodificação por hardware) e controle de congestionamento em redes reais.

## Desenvolvimento

<details>
<summary>Conteúdo do repositório</summary>

| Caminho | Conteúdo | Linguagem |
|---|---|---|
| `server/` | Servidor de controle: página de login + cookies (autenticação Basic para roteadores), API REST, WebSockets do visualizador/agente, relay TURN integrado, HTTPS opcional com certificados automáticos Let's Encrypt (ACME) | C + libev + mbedTLS |
| `agent/` | Agente do roteador: cliente ADB integrado (usbfs), supervisor do celular, relay UDP, cliente WebSocket, interface de linha de comando | C + libev |
| `android/native/` | Pilha WebRTC do celular: ICE + cliente TURN, DTLS-SRTP, canais de dados SCTP, RTP H.265/H.264, controle de congestionamento | C |
| `android/java/` | Servidor do celular: captura de tela, MediaCodec, injeção de entrada, área de transferência, energia da tela, arquivos, logcat | Java (app_process) |
| `web/` | Cliente iPhone (PWA): vídeo, som, multitoque, menu da bola flutuante, envio de fotos, área de transferência, todas as configurações; `panel.html` (abas Dispositivos, Agentes, Servidor) e `wizard.html` (configuração inicial) | HTML/CSS/JS |
| `worker/` | O mesmo painel no Cloudflare Workers + um Durable Object, com TURN do Cloudflare | JS |
| `wizard/` | Site de instalação (Cloudflare Pages, remodroid.pages.dev), `install_server.sh`, `install_agent.sh`, `update.sh` | HTML, sh |
| `common/` | Código C compartilhado: enquadramento WebSocket, quadros de túnel, STUN, envelope JSON, ini, utilitários | C |
| `third_party/` | Bibliotecas incluídas no repositório e vinculadas estaticamente: libev, inih, cJSON, picohttpparser, crypt_blowfish, mbedTLS, libsrtp2, usrsctp | C |
| `deploy/` | Exemplos de `server.ini` / `agent.ini`, unidades systemd, scripts OpenRC e procd | |
| `packaging/`, `.github/workflows/` | Definições de pacotes nFPM (deb, rpm, apk, ipk) e fluxo de compilação/publicação de CI | |
| `python/`, `skills/`, `.claude/skills/phone/` | Controle por IA: SDK `remodroid` e CLI `rd`, habilidades reproduzíveis e a habilidade Claude Code que as utiliza | Python |
| `tests/` | Testes de ponta a ponta | Node, Python |
| `demo/` | Painel simulado original (offline, sem servidor) | HTML |

</details>

<details>
<summary>Compilação</summary>

Compilação no host (desenvolvimento e testes):

```sh
make            # build/host/remodroid-server, build/host/remodroid-agent
make rtc-tests  # build/host/rtc/rtc_test, rtp_dump (pilha do celular no host)
```

Binários estáticos de distribuição para roteadores e servidores (Zig como compilador cruzado, musl, tudo vinculado):

```sh
make dist                   # dentro do contêiner de compilação (build/Dockerfile, requer podman/docker)
./build/dist.sh mipsel      # ou diretamente, com `zig` no PATH; destinos: x86 x86_64 armv7 aarch64 mipsel mips
```

Os resultados ficam em `build/out/<arch>/`: cerca de 0,8 MB por binário (1,2 MB em MIPS).

Arquivos do celular (`remodroid-server.jar` + `libremodroid.so` por ABI):

```sh
ANDROID_JAR=.../platforms/android-34/android.jar D8=.../build-tools/34.0.0/d8 NDK=.../android-ndk-r27c \
  ./android/build.sh        # build/android/remodroid-server.jar, build/android/<abi>/libremodroid.so
```

</details>

<details>
<summary>Testes</summary>

| Comando | Cobertura |
|---|---|
| `make test` | Plano de controle de ponta a ponta: servidor + agente reais, adbd simulado fiel ao protocolo (autenticação RSA verificada com OpenSSL), celular simulado; autenticação, REST, WebSockets, ADB push/shell/sync, túnel, relay UDP, comandos, edições em `agent.ini`, CLI, reconexões (48 verificações) |
| (em `make test`) | Encaminhamento de notificações: a configuração `[notify]` chega ao celular, uma notificação capturada chega ao visualizador por WebSocket e o servidor a envia em JSON a um endpoint Bark simulado |
| `python3 tests/python_sdk.py` | SDK de controle por IA, CLI `rd` e executor de habilidades com uma API de celular simulada: seletores, capturas em memória, transferência para a IA (saída 3), retomada com `--from-step` |
| `QEMU=qemu-mipsel node tests/run.mjs build/mipsel` | O mesmo, com binários estáticos de roteador MIPS sob emulação em modo usuário |
| `python android/native/test/rtc_interop.py` | Pilha WebRTC do celular ↔ aiortc: ICE, DTLS-SRTP, SCTP, quadros H.264 decodificados, canais de dados |
| `python android/native/test/rtp_depack.py` | Empacotadores H.265 e H.264: ida e volta exata bit a bit por um desempacotador independente + FFmpeg |
| `python tests/e2e_webrtc.py [--relay]` | Caminho completo: visualizador → servidor → agente → túnel → pilha do celular; `--relay` força o TURN integrado |
| `python tests/e2e_browser.py` | Cliente web real no Chrome sem interface com a pilha completa: vídeo, toques, menu, protocolo |
| `node tests/setup.mjs build/host` | Assistente inicial (token de configuração, salvamento, reinício na nova porta), login, códigos de registro com o `enroll` do agente real, configurações do roteador pelo painel, renomear, remover |
| `node tests/worker.mjs build/host` | Cloudflare Worker em `wrangler dev` (após `cd worker && npm install`) com o agente real: configuração, login, registro, WebSocket do visualizador e sinalização, comandos, renomear, remover |
| `sudo tests/openwrt.sh IP [VERSION]` | OpenWrt (19.07–24.10) no podman com endereço LAN próprio (macvlan), LuCI, ipk do agente compilado deste repositório e servidor; abra `http://IP/` em outro dispositivo e pareie em Serviços → RemoDroid |
| `node tests/tls.mjs build/host` | HTTPS, redirecionamento, cookies `Secure`, visualizador e agente por `wss://` (CAs confiáveis e não confiáveis), recarga de certificado ao receber SIGHUP; com `PEBBLE_DIR`, emissão ACME, reutilização e renovação automática com Pebble (CA de testes do Let's Encrypt) |

Os testes Python precisam de `aiortc` (e `playwright` + Chromium para o teste de navegador).

</details>

Documentos de projeto: [plan.md](../../plan.md) (arquitetura e decisões), [skill-plan.md](../../skill-plan.md) (controle por IA),
[packaging/merlin/README.md](../../packaging/merlin/README.md) (plugin Merlin).
