# RemoDroid en OpenWrt

Qué archivo descargar para tu router, qué versiones de OpenWrt funcionan y una lista de routers por CPU.
Para la configuración completa, empieza por el [README](README.md#instalación).

## Encuentra tu archivo en 10 segundos

Tu router necesita un **puerto USB** (para el teléfono) y una de estas arquitecturas de paquetes. Puedes identificarla de tres maneras:

- **LuCI:** **Estado → Vista general** muestra la *Plataforma de destino*, por ejemplo `ramips/mt7621`. Búscala en la [tabla de plataformas](#by-target-platform).
- **SSH:** ejecuta `opkg print-architecture`. La última línea indica la arquitectura del paquete, por ejemplo `mipsel_24kc`.
- **Por modelo:** busca tu router en la [lista de routers](#router-list).

Después descarga el `.ipk` de esa arquitectura desde la [última versión](https://github.com/yinyue123/RemoDroid/releases/latest), o desde la tabla del agente en el [README](README.md#2-agente-en-el-router):

| Descarga | CPU | Habitual en |
|---|---|---|
| `aarch64_cortex-a53` | 64 bits ARM | MediaTek Filogic (MT7981, MT7986, MT7988), MT7622, Qualcomm IPQ807x, Broadcom BCM4908, Raspberry Pi 3 |
| `aarch64_cortex-a72` | 64 bits ARM | Raspberry Pi 4, Marvell Armada 7k/8k |
| `aarch64_generic` | 64 bits ARM | Rockchip (NanoPi R2S, R4S, R5S, R6S), NXP Layerscape |
| `arm_cortex-a7_neon-vfpv4` | 32 bits ARM | Qualcomm IPQ40xx, MT7623, Raspberry Pi 2, Allwinner |
| `arm_cortex-a9_vfpv3-d16` | 32 bits ARM | Marvell Armada 38x / XP (serie Linksys WRT, Turris Omnia) |
| `arm_cortex-a15_neon-vfpv4` | 32 bits ARM | Qualcomm IPQ806x (Netgear R7800, TP-Link C2600) |
| `mipsel_24kc` | MIPS, little-endian | MediaTek MT7621, MT7620, MT7628 |
| `mips_24kc` | MIPS, big-endian | Qualcomm Atheros (ath79: QCA9558, QCA9563, QCA9531, AR9344), Lantiq |
| `x86_64` | 64 bits PC | Cualquier PC, mini PC o máquina virtual |
| `i386_pentium4` | 32 bits PC | PC antiguos |

### Routers populares

| Router | CPU | Descarga | OpenWrt desde |
|---|---|---|---|
| GL.iNet GL-MT6000 (Flint 2), GL-MT3000 (Beryl AX) | MediaTek MT7986 / MT7981, ARM64 | `aarch64_cortex-a53` | 23.05 |
| Linksys E8450, Belkin RT3200 | MediaTek MT7622, ARM64 | `aarch64_cortex-a53` | 22.03 |
| Banana Pi BPI-R3 | MediaTek MT7986, ARM64 | `aarch64_cortex-a53` | 23.05 |
| Cudy TR3000 | MediaTek MT7981, ARM64 | `aarch64_cortex-a53` | 23.05 |
| Dynalink DL-WRX36 | Qualcomm IPQ8072A, ARM64 | `aarch64_cortex-a53` | 23.05 |
| FriendlyElec NanoPi R4S | Rockchip RK3399, ARM64 | `aarch64_generic` | 22.03 |
| Raspberry Pi 4 | Broadcom BCM2711, ARM64 | `aarch64_cortex-a72` | 21.02 |
| Netgear R7800 | Qualcomm IPQ8065, 32 bits ARM | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Linksys WRT3200ACM, Turris Omnia | Marvell Armada 385, 32 bits ARM | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| GL.iNet GL-B1300, AVM FRITZ!Box 4040 | Qualcomm IPQ4028 / IPQ4018, 32 bits ARM | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Xiaomi Mi Router 3G, Newifi D2 | MediaTek MT7621, MIPS LE | `mipsel_24kc` | 21.02 |
| GL.iNet GL-MT300N V2 | MediaTek MT7628, MIPS LE | `mipsel_24kc` | 21.02 |
| TP-Link Archer C7 v5, TL-WR1043N/ND v3 | Qualcomm QCA9563 / QCA9558, MIPS BE | `mips_24kc` | 21.02 |
| GL.iNet GL-AR750S | Qualcomm QCA9563, MIPS BE | `mips_24kc` | 21.02 |
| Cualquier PC o mini PC | x86-64 | `x86_64` | 21.02 |

¿No aparece? Busca en la [lista completa](#router-list). Estos modelos populares **no tienen** puerto USB y no pueden conectar el teléfono:
Xiaomi AX3000T, Redmi AX6S, Redmi AC2100, TP-Link Archer C6 v3.

> **Un router MIPS funciona, pero es la opción más lenta.** Su CPU mueve cada paquete de vídeo entre USB y la red, y un núcleo MIPS de 580–880 MHz puede no alcanzar las tasas de bits más altas (20 Mbps). Los routers ARM tienen margen de sobra.

## Versiones de OpenWrt

| OpenWrt | Agente `.ipk` | Página LuCI (Servicios → RemoDroid) |
|---|---|---|
| 25.12 y posteriores | No: OpenWrt cambió de paquetes `opkg` a `apk`. Usa el `.tar.gz` de *Cualquier Linux* para tu CPU | No |
| 24.10 | Sí | Sí |
| 23.05 | Sí (probado: 23.05.5, x86_64) | Sí (probado) |
| 22.03, 21.02 | Sí | Sí |
| 19.07 y anteriores | Debería instalarse, sin probar | No (necesita LuCI de 21.02 o posterior) |

Los nombres de arquitectura de los paquetes son iguales en todas las versiones, así que la tabla anterior se aplica a todas ellas.

## Instalación

1. En LuCI: **Sistema → Software → Subir paquete…**, elige el `.ipk`, después **Subir** e **Instalar**.
   O por SSH: `opkg install ./remodroid-agent_*.ipk`.
2. Abre **Servicios → RemoDroid**, pega el comando de vinculación del panel (**Agentes → Añadir**) y pulsa **Vincular**.
3. Conecta el teléfono con la depuración USB activada y toca **Permitir** en él. La página muestra cuándo está conectado.

## Sin compatibilidad

Estas CPU no pueden ejecutar el agente: ARM de 32 bits sin unidad de coma flotante (`arm_cortex-a9`, por ejemplo Broadcom BCM4708/4709 de Netgear R7000 y ASUS RT-AC68U en OpenWrt), ARMv5/ARMv6 (`arm_arm926ej-s`, `arm_arm1176jzf-s_vfp`, Raspberry Pi 1), MIPS de 64 bits, PowerPC y RISC-V. Para routers ASUS con Asuswrt-Merlin, usa el [paquete Merlin](../../packaging/merlin/README.md).

<a id="by-target-platform"></a>

## Por plataforma de destino

<!-- targets -->
| Plataforma de destino | Descarga | Routers con USB |
|---|---|---|
| `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 7 |
| `ath79/generic` | `mips_24kc` | 183 |
| `ath79/mikrotik` | `mips_24kc` | 8 |
| `ath79/nand` | `mips_24kc` | 31 |
| `ath79/tiny` | `mips_24kc` | 3 |
| `bcm27xx/bcm2709` | `arm_cortex-a7_neon-vfpv4` | 1 |
| `bcm27xx/bcm2710` | `aarch64_cortex-a53` | 1 |
| `bcm27xx/bcm2711` | `aarch64_cortex-a72` | 1 |
| `bcm27xx/bcm2712` | ARM64 `.tar.gz` (`aarch64_cortex-a76`, sin probar) | 1 |
| `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 17 |
| `bcm4908/generic` | `aarch64_cortex-a53` | 2 |
| `imx/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 1 |
| `imx/cortexa9` | ARMv7 `.tar.gz` (`arm_cortex-a9_neon`, sin probar) | 1 |
| `ipq40xx/chromium` | `arm_cortex-a7_neon-vfpv4` | 1 |
| `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 67 |
| `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 9 |
| `ipq806x/chromium` | `arm_cortex-a15_neon-vfpv4` | 2 |
| `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 28 |
| `lantiq/xrx200` | `mips_24kc` | 25 |
| `lantiq/xrx200_legacy` | `mips_24kc` | 2 |
| `lantiq/xway` | `mips_24kc` | 13 |
| `layerscape/armv7` | `arm_cortex-a7_neon-vfpv4` | 3 |
| `layerscape/armv8_64b` | `aarch64_generic` | 17 |
| `mediatek/filogic` | `aarch64_cortex-a53` | 49 |
| `mediatek/mt7622` | `aarch64_cortex-a53` | 7 |
| `mediatek/mt7623` | `arm_cortex-a7_neon-vfpv4` | 3 |
| `mvebu/cortexa53` | `aarch64_cortex-a53` | 9 |
| `mvebu/cortexa72` | `aarch64_cortex-a72` | 10 |
| `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 31 |
| `omap/generic` | ARMv7 `.tar.gz` (`arm_cortex-a8_vfpv3`, sin probar) | 2 |
| `qualcommax/ipq807x` | `aarch64_cortex-a53` | 31 |
| `ramips/mt7620` | `mipsel_24kc` | 82 |
| `ramips/mt7621` | `mipsel_24kc` | 139 |
| `ramips/mt76x8` | `mipsel_24kc` | 46 |
| `ramips/rt305x` | `mipsel_24kc` | 5 |
| `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 7 |
| `rockchip/armv8` | `aarch64_generic` | 36 |
| `stm32/stm32mp1` | `arm_cortex-a7_neon-vfpv4` | 1 |
| `sunxi/cortexa53` | `aarch64_cortex-a53` | 1 |
| `sunxi/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 4 |
| `tegra/generic` | `arm_cortex-a9_vfpv3-d16` | 1 |
| `x86/64` | `x86_64` | 1 |
| `x86/generic` | `i386_pentium4` | 1 |
<!-- /targets -->

<a id="router-list"></a>

## Lista de routers

<!-- routers -->
978 routers con puerto USB en OpenWrt 24.10.8. *Desde* indica la primera versión de OpenWrt (21.02 o posterior) con una imagen para ese modelo. Usa la búsqueda del navegador (Ctrl+F) después de abrir un grupo.

<details>
<summary><b>ARM64</b>: 178 routers</summary>

| Router | Destino | Descarga | Desde |
|---|---|---|---|
| Acer Predator Connect W6 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Acer Predator Connect W6d | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Adtran SDG-8733 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Adtran SDG-8734 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Arcadyan AW1000 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| ArmSoM Sige7 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| ASUS GT-AC5300 | `bcm4908/generic` | `aarch64_cortex-a53` | 21.02 |
| ASUS RT-AX59U | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Asus RT-AX89X | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| ASUS TUF-AX4200 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| ASUS TUF-AX4200Q | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| ASUS TUF-AX6000 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Bananapi BPi-M7 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Bananapi BPi-R3 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Bananapi BPi-R3 Mini | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Bananapi BPi-R4 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Bananapi BPi-R4 2.5GE | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Bananapi BPi-R64 | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| Belkin RT3200 | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| Belkin RT3200 UBI | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| Buffalo WXR-5950AX12 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| CMCC RAX3000M | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| CMCC RAX3000Me | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| CMCC RM2-6 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Compex WPQ873 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Confiabits MT7981 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Cudy TR3000 256mb v1 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Cudy TR3000 v1 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Cudy TR3000 v1 (OpenWrt U-Boot layout) | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Cudy WR3000P v1 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Dynalink DL-WRX36 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Edgecore EAP102 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Edimax CAX1800 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Elecom WRC-2533GENT | `mediatek/mt7622` | `aarch64_cortex-a53` | 21.02 |
| Firefly ROC-RK3328-CC | `rockchip/armv8` | `aarch64_generic` | 23.05 |
| FriendlyARM NanoPC T4 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPC T6 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM Nanopi R1S H5 | `sunxi/cortexa53` | `aarch64_cortex-a53` | 22.03 |
| FriendlyARM NanoPi R2C | `rockchip/armv8` | `aarch64_generic` | 23.05 |
| FriendlyARM NanoPi R2C Plus | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPi R2S | `rockchip/armv8` | `aarch64_generic` | 21.02 |
| FriendlyARM NanoPi R3S | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPi R4S 4GB LPDDR4 | `rockchip/armv8` | `aarch64_generic` | 22.03 |
| FriendlyARM NanoPi R4S Enterprise Edition 4GB LPDDR4 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPi R5C | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPi R5S | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPi R6C | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| FriendlyARM NanoPi R6S | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| GatoNetworks gdsp | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| GL.iNet GL-MT2500 Airoha PHY | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| GL.iNet GL-MT2500 MaxLinear PHY | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| GL.iNet GL-MT3000 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| GL.iNet GL-MT6000 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| GL.iNet GL-MV1000 | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| GL.iNet GL-X3000 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| GL.iNet GL-XE3000 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Globalscale MOCHAbin | `mvebu/cortexa72` | `aarch64_cortex-a72` | 22.03 |
| Huasifei WH3000 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Huasifei WH3000 Pro | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| iEi Puzzle-M901 | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| iEi Puzzle-M902 | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| ipTIME AX7800M-6E | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Keenetic KN-3811 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| LinkEase EasePi R1 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Linksys E8450 | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| Linksys E8450 UBI | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| Linksys HomeWRK | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Linksys MX4200 v1 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Linksys MX4200 v2 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Linksys MX4300 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Linksys MX5300 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Linksys MX8500 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Lunzn FastRhino R66S | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Marvell Armada 3700 Community Board eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell Armada 3700 Community Board Non-eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell Armada 3700 Community Board V7 eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell Armada 3700 Community Board V7 Non-eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell Armada 3720 Development Board (DB-88F3720-DDR3) | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell Armada 7040 Development Board | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| Marvell Armada 8040 Development Board | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| Marvell ESPRESSObin eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell ESPRESSObin Non-eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell ESPRESSObin Ultra | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell ESPRESSObin V7 eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| Marvell ESPRESSObin V7 Non-eMMC | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| MediaTek MT7981 rfb | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| MediaTek MTK7622 rfb1 AP | `mediatek/mt7622` | `aarch64_cortex-a53` | 21.02 |
| MediaTek MTK7622 rfb1 AP (UBI) | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| Methode eDPU | `mvebu/cortexa53` | `aarch64_cortex-a53` | 23.05 |
| Methode micro-DPU (uDPU) | `mvebu/cortexa53` | `aarch64_cortex-a53` | 21.02 |
| MikroTik RB5009 | `mvebu/cortexa72` | `aarch64_cortex-a72` | 24.10 |
| Netgear R8000P | `bcm4908/generic` | `aarch64_cortex-a53` | 21.02 |
| Netgear RAX120v2 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Netgear SXR80 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Netgear SXS80 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Netgear WAX218 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Netgear WAX620 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Netgear WAX630 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| NXP FRDM-LS1012A | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP FRWY-LS1012A | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP FRWY-LS1046A Default | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP FRWY-LS1046A SD Card Boot | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1012A-RDB | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1028A-RDB Default | `layerscape/armv8_64b` | `aarch64_generic` | 23.05 |
| NXP LS1028A-RDB SD Card Boot | `layerscape/armv8_64b` | `aarch64_generic` | 23.05 |
| NXP LS1043A-RDB Default | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1043A-RDB SD Card Boot | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1046A-RDB Default | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1046A-RDB SD Card Boot | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1088A-RDB Default | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS1088A-RDB SD Card Boot | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LS2088ARDB | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LX2160A-RDB Rev2.0 silicon | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| NXP LX2160A-RDB Rev2.0 silicon SD Card Boot | `layerscape/armv8_64b` | `aarch64_generic` | 21.02 |
| OpenEmbed SOM7981 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| OpenFi 6C | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| OpenWrt One | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Pine64 Rock64 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Pine64 RockPro64 | `rockchip/armv8` | `aarch64_generic` | 21.02 |
| prpl Foundation Haze | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| QNAP 301w | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Radxa CM3 IO | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa E25 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 3A | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 3B | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 3C | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 4C+ | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 4SE | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 5A | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK 5B | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK Pi 4A | `rockchip/armv8` | `aarch64_generic` | 22.03 |
| Radxa ROCK Pi E | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK Pi E v3.0 | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ROCK Pi S | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ZERO 3E | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Radxa ZERO 3W | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| Raspberry Pi 2B-1.2 (64bit) | `bcm27xx/bcm2710` | `aarch64_cortex-a53` | 21.02 |
| Raspberry Pi 3A+/3B/3B+/CM3/Zero2/Zero2W (64bit) | `bcm27xx/bcm2710` | `aarch64_cortex-a53` | 21.02 |
| Raspberry Pi 4B/400/CM4 (64bit) | `bcm27xx/bcm2711` | `aarch64_cortex-a72` | 21.02 |
| Raspberry Pi 5/500/CM5 | `bcm27xx/bcm2712` | ARM64 `.tar.gz` (`aarch64_cortex-a76`, sin probar) | 24.10 |
| Redmi AX6 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Routerich AX3000 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Routerich AX3000 (OpenWrt U-Boot layout) | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Routerich AX3000 v1 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Sinovoip Bananapi-R2 Pro | `rockchip/armv8` | `aarch64_generic` | 24.10 |
| SolidRun Armada 8040 Community Board Double Shot | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| SolidRun Armada 8040 Community Board Single Shot | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| SolidRun Clearfog GT-8K | `mvebu/cortexa72` | `aarch64_cortex-a72` | 23.05 |
| SolidRun ClearFog Pro | `mvebu/cortexa72` | `aarch64_cortex-a72` | 24.10 |
| SolidRun MACCHIATObin Double Shot | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| SolidRun MACCHIATObin Single Shot | `mvebu/cortexa72` | `aarch64_cortex-a72` | 21.02 |
| Spectrum SAX1V1K | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| TOTOLINK A8000RU | `mediatek/mt7622` | `aarch64_cortex-a53` | 22.03 |
| TP-Link Archer AX80V1 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| TP-Link EAP660 HD v1 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| TP-Link TL-XDR4288 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| TP-Link TL-XDR6086 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| TP-Link TL-XDR6088 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| TP-Link TL-XTR8488 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Traverse Ten64 (NAND boot) | `layerscape/armv8_64b` | `aarch64_generic` | 23.05 |
| Unielec U7981-01 (EMMC) | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Unielec U7981-01 (NAND) | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| WAVLINK WL-WN536AX6 Rev a | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| Xiaomi AX3600 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Xiaomi AX9000 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Xunlong Orange Pi R1 Plus | `rockchip/armv8` | `aarch64_generic` | 23.05 |
| Xunlong Orange Pi R1 Plus LTS | `rockchip/armv8` | `aarch64_generic` | 23.05 |
| Yuncore AX880 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Zbtlink ZBT-Z800AX | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Zbtlink ZBT-Z8102AX | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Zbtlink ZBT-Z8102AX-V2 | `mediatek/filogic` | `aarch64_cortex-a53` | 24.10 |
| ZTE MF269 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 24.10 |
| Zyxel EX5601-T0 (OpenWrt U-Boot layout) | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Zyxel EX5601-T0 (stock layout) | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Zyxel EX5601-T1 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| Zyxel EX5700 (Telenor) | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |
| ZYXEL NBG7815 | `qualcommax/ipq807x` | `aarch64_cortex-a53` | 23.05 |
| Zyxel T-56 | `mediatek/filogic` | `aarch64_cortex-a53` | 23.05 |

</details>

<details>
<summary><b>ARMv7</b>: 178 routers</summary>

| Router | Destino | Descarga | Desde |
|---|---|---|---|
| 8devices Habanero DVK | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| 8devices Jalapeno | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| ALFA Network AP120C-AC | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Arris TR4400 v2 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 22.03 |
| Aruba AP-303 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Aruba AP-303H | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Aruba AP-365 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Askey RT4230W REV6 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 22.03 |
| ASRock G10 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| ASUS Lyra (MAP-AC2200) | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| ASUS OnHub SRT-AC1900 | `ipq806x/chromium` | `arm_cortex-a15_neon-vfpv4` | 23.05 |
| ASUS RT-AC2200 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| ASUS RT-AC42U | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| ASUS RT-AC58U | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| ASUS RT-ACRH13 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| ASUS RT-ACRH17 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| AVM FRITZ!Box 4040 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| AVM FRITZ!Box 7520 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| AVM FRITZ!Box 7530 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| AVM FRITZ!Repeater 1200 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| AVM FRITZ!Repeater 3000 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Bananapi BPi-R2 | `mediatek/mt7623` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| BeagleBoard.org OMAP3 TI beagleboard | `omap/generic` | ARMv7 `.tar.gz` (`arm_cortex-a8_vfpv3`, sin probar) | 21.02 |
| Buffalo LinkStation LS220DE | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 23.05 |
| Buffalo LinkStation LS421DE | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Buffalo WTR-M2133HP | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Buffalo WXR-2533DHP | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Cell C RTL30VW | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Cisco Meraki MR33 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Cisco Meraki MR74 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Compex WPQ864 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| CompuLab TrimSlice | `tegra/generic` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Ctera C200 V2 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 22.03 |
| CZ.NIC Turris Omnia | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| D-Link DAP-2610 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Edgecore ECW5211 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Edgecore ECW5410 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| EnGenius EAP1300 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| EnGenius EAP1300EXT | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Extreme Networks AP3935 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 24.10 |
| Extreme Networks WS-AP3915i | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Extreme Networks WS-AP391x | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| EZVIZ CS-W3-WD1200G EUP | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Fortinet FAP-421E | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 24.10 |
| Fortinet FortiGate 30E | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| Fortinet FortiGate 50E | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 23.05 |
| Fortinet FortiGate 51E | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| Fortinet FortiGate 52E | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| Fortinet FortiWiFi 50E-2R | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| Fortinet FortiWiFi 51E | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| FriendlyARM NanoPi R1 | `sunxi/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| GL.iNet GL-A1300 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| GL.iNet GL-AP1300 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| GL.iNet GL-B1300 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| GL.iNet GL-B2200 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| Globalscale Mirabox | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Google WiFi (Gale) | `ipq40xx/chromium` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| IIJ SA-W2 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| ipTIME NAS1dual | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 22.03 |
| Kobol Helios4 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys Caiman | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys Cobra | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys E8350 v1 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 24.10 |
| Linksys EA6350 v3 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Linksys EA7500 v1 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Linksys EA8300 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Linksys EA8500 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Linksys Mamba | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys MR8300 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Linksys Rango | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys Shelby | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys Venom | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WHW01 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Linksys WHW03 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| Linksys WHW03 V2 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Linksys WRT1200AC | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WRT1900AC v1 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WRT1900AC v2 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WRT1900ACS v1 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WRT1900ACS v2 | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WRT3200ACM | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Linksys WRT32X | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Luma Home WRTQ-329ACN | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Marvell Armada 370 Development Board (DB-88F6710-BP-DDR3) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Marvell Armada 370 RD (RD-88F6710-A1) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Marvell Armada 385 Development Board AP (DB-88F6820-AP) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Marvell Armada 388 RD (RD-88F6820-AP) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Marvell Armada Armada XP GP (DB-MV784MP-GP) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Marvell Armada XP Development Board (DB-78460-BP) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Microchip SAMA5D2 ICP | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 22.03 |
| Microchip SAMA5D2 PTC Ek | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 21.02 |
| Microchip SAMA5D2 Xplained | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 21.02 |
| Microchip SAMA5D27 SOM1 Ek | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 21.02 |
| Microchip SAMA5D27 WSOM1 Ek | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 22.03 |
| Microchip SAMA5D3 Xplained | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 21.02 |
| Microchip SAMA5D4 Xplained | `at91/sama5` | ARMv7 `.tar.gz` (`arm_cortex-a5_vfpv4`, sin probar) | 21.02 |
| MikroTik cAP ac | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| MikroTik hAP ac2 | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| MikroTik hAP ac3 | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| MikroTik hAP ac3 LTE6 kit | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| MikroTik SXTsq 5 ac (RBSXTsqG-5acD) | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| MikroTik wAP ac | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| MikroTik wAP ac LTE | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Mikrotik wAP ac LTE6 | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| MikroTik wAP R ac | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| MikroTik Wireless Wire Dish LHGG-60ad | `ipq40xx/mikrotik` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| MobiPromo CM520-79F | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| NEC Aterm WG2600HP | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| NETGEAR EX6100 v2 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| NETGEAR EX6150 v2 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| NETGEAR LBR20 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| NETGEAR Nighthawk X4 D7800 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| NETGEAR Nighthawk X4 R7500 v1 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| NETGEAR Nighthawk X4 R7500 v2 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| NETGEAR Nighthawk X4S R7800 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| NETGEAR Nighthawk XR450 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 24.10 |
| NETGEAR Nighthawk XR500 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 22.03 |
| NETGEAR RBR40 v1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| NETGEAR RBR50 v1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| NETGEAR RBS40 v1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| NETGEAR RBS50 v1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| NETGEAR SRR60 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| NETGEAR SRS60 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| Netgear WAC510 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| Nokia AC400i | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 23.05 |
| NXP LS1021A-IoT SD Card Boot | `layerscape/armv7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| NXP TWR-LS1021A Default | `layerscape/armv7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| NXP TWR-LS1021A SD Card Boot | `layerscape/armv7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Olimex A20-OLinuXino-LIME2 | `sunxi/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Olimex A20-OLinuXino-LIME2 eMMC | `sunxi/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| OpenMesh A42 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| OpenMesh A62 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| P&W R619AC 128M NAND | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| P&W R619AC 64M NAND | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| Pakedge WR-1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| PandaBoard.org OMAP4 TI pandaboard | `omap/generic` | ARMv7 `.tar.gz` (`arm_cortex-a8_vfpv3`, sin probar) | 21.02 |
| Plasma Cloud PA1200 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Plasma Cloud PA2200 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Plat'Home OpenBlocks AX3 4 ports | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| Qualcomm AP148 legacy | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Qualcomm AP148 standard | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Qualcomm AP161 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Qualcomm DB149 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Qxwlan E2600AC C1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Qxwlan E2600AC C2 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Raspberry Pi 2B/2B 1.2 (32bit) | `bcm27xx/bcm2709` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Raspberry Pi 3A+/3B/3B+/CM3/Zero2/Zero2W (32bit) | `bcm27xx/bcm2709` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Raspberry Pi 4B/400/CM4 (32bit) | `bcm27xx/bcm2709` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| SKSpruce WIA3300-20 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| SolidRun ClearFog Base | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| SolidRun ClearFog Pro | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 21.02 |
| SolidRun CuBox-i | `imx/cortexa9` | ARMv7 `.tar.gz` (`arm_cortex-a9_neon`, sin probar) | 22.03 |
| Sony NCP-HG100/Cellular | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| STMicroelectronics STM32MP135F-DK | `stm32/stm32mp1` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| Synology DS213j | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| TechNexion PICO-PI-IMX7D | `imx/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Teltonika RUTX50 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| TP-Link AD7200 v1/v2 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| TP-Link Archer C2600 v1 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| TP-Link Archer VR2600v v1 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| TP-Link OnHub AC1900 Cloud Router | `ipq806x/chromium` | `arm_cortex-a15_neon-vfpv4` | 23.05 |
| TP-Link Talon AD7200 v1/v2 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| Ubiquiti UniFi AC HD | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |
| UniElec U7623-02 | `mediatek/mt7623` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| UniElec U7623-02 eMMC/512MiB RAM (legacy image) | `mediatek/mt7623` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Wallys DR40X9 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| Western Digital MyCloud Mirror Gen 2 (BWVZ/Grand Teton) | `mvebu/cortexa9` | `arm_cortex-a9_vfpv3-d16` | 24.10 |
| Xunlong Orange Pi R1 | `sunxi/cortexa7` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| YYeTs LE1 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| ZTE MF18A | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| ZTE MF282Plus | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 24.10 |
| ZTE MF286D | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| ZTE MF287 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| ZTE MF287Plus | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| ZTE MF287Pro | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 23.05 |
| ZTE MF289F | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 22.03 |
| Zyxel NBG6617 | `ipq40xx/generic` | `arm_cortex-a7_neon-vfpv4` | 21.02 |
| Zyxel NBG6817 | `ipq806x/generic` | `arm_cortex-a15_neon-vfpv4` | 21.02 |

</details>

<details>
<summary><b>MIPS LE</b>: 314 routers</summary>

| Router | Destino | Descarga | Desde |
|---|---|---|---|
| AFOUNDRY EW1200 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Aigale Ai-BR100 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ALFA Network AC1200RM | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ALFA Network Quad-E4G | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ALFA Network R36M-E4G | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ALFA Network Tube-E4G | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Allnet ALL5002 | `ramips/rt305x` | `mipsel_24kc` | 21.02 |
| Amped Wireless ALLY-R1900K | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| AsiaRF AP7621-001 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| AsiaRF AP7621-NV1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ASUS RT-AC1200 | `ramips/mt76x8` | `mipsel_24kc` | 22.03 |
| ASUS RT-AC1200GU | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| ASUS RT-AC51U | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ASUS RT-AC53U | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| ASUS RT-AC54U | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ASUS RT-AC57U v1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| ASUS RT-AC65P | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ASUS RT-AC85P | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ASUS RT-AX1800U | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| ASUS RT-AX53U | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| ASUS RT-N14u | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ASUS RT-N14UHP | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| ASUS RT-N15U | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| ASUS RT-N16 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| ASUS RT-N56U | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| ASUS RT-N56U B1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ASUS RT-N600 | `ramips/mt76x8` | `mipsel_24kc` | 22.03 |
| ASUS RT-N66U | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| ASUS RT-N66W | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| BDCOM WAP2100-SK (ZTE ZXECS EBG3130) | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Beeline SmartBox Flash | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Beeline SmartBox GIGA | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Beeline SmartBox PRO | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Beeline SmartBox TURBO | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Beeline SmartBox TURBO+ | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Belkin F9K1109 Version 1.0 | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Belkin RT1800 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Bolt BL100 | `ramips/mt7620` | `mipsel_24kc` | 24.10 |
| CreativeBox v1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Cudy C200P | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| Cudy TR1200 v1 | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| Cudy WR1300 v1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| D-Link DIR-1935 A1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| D-Link DIR-1960 A1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DIR-2055 A1 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| D-Link DIR-2150 A1 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| D-Link DIR-2150 R1 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| D-Link DIR-2640 A1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DIR-2660 A1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DIR-2660 A2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DIR-3040 A1 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| D-Link DIR-3060 A1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| D-Link DIR-510L | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DIR-645 | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| D-Link DIR-853 A1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| D-Link DIR-853 A3 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| D-Link DIR-853 R1 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| D-Link DIR-860L B1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DIR-882 A1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DIR-882 R1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| D-Link DWR-118 A1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DWR-118 A2 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DWR-921 C1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DWR-921 C3 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DWR-922 E2 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DWR-960 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| D-Link DWR-961 A1 | `ramips/mt7620` | `mipsel_24kc` | 22.03 |
| D-Team Newifi D2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| DNA Valokuitu Plus EX400 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| DomyWifi DM202 | `ramips/mt7620` | `mipsel_24kc` | 22.03 |
| DomyWifi DM203 | `ramips/mt7620` | `mipsel_24kc` | 22.03 |
| DomyWifi DW22D | `ramips/mt7620` | `mipsel_24kc` | 22.03 |
| Dovado Tiny AC | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Dual-Q H721 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| DuZun DM06 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Edimax BR-6208AC V2 | `ramips/mt7620` | `mipsel_24kc` | 23.05 |
| Edimax BR-6478AC V2 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Edimax EW-7478APC | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Elecom WRH-300CR | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| EnGenius EPG600 | `ramips/mt7620` | `mipsel_24kc` | 23.05 |
| EnGenius ESR600 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| EnGenius ESR600H | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Etisalat S3 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Firefly FireWRT | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Fon FON2601 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Fon Fonera 2.0N | `ramips/rt305x` | `mipsel_24kc` | 21.02 |
| GeHua GHL-R-001 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Gemtek WVRTM-127ACN | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| Gemtek WVRTM-130ACN | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| Genexis Pulse EX400 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| GL.iNet GL-MT1300 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| GL.iNet GL-MT300A | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| GL.iNet GL-MT300N | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| GL.iNet GL-MT300N V2 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| GL.iNet GL-MT750 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| GnuBee GB-PC1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| GnuBee GB-PC2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Hak5 WiFi Pineapple Mark 7 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Hanyang CJ-Hello HYC-G920 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Hauppauge Broadway | `ramips/rt305x` | `mipsel_24kc` | 21.02 |
| Head Weblink HDRM2000 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Hi-Link HLK-7688A | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| HiLink HLK-7621A evaluation board | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| HiWiFi HC5611 | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| HiWiFi HC5761 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| HiWiFi HC5761A | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| HiWiFi HC5861 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| HiWiFi HC5962 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| HNET C108 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Hongdian H7920 v40 | `ramips/mt76x8` | `mipsel_24kc` | 24.10 |
| Hongdian H8850 v20 | `ramips/mt76x8` | `mipsel_24kc` | 24.10 |
| Hongdian H8922 v30 | `ramips/mt7620` | `mipsel_24kc` | 24.10 |
| HooToo HT-TM05 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Huasifei WS1208V2 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| HUMAX E10 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| HUMAX QUANTUM E10 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Inteno Pulse EX400 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| ipTIME A1004ns | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ipTIME A104ns | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| ipTIME A3004NS-dual | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| ipTIME A3004T | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| ipTIME A6004NS-M | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| ipTIME A6ns-M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ipTIME A8004T | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ipTIME AX2004M | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| JCG JHR-AC876M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| JCG Y2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Jotale JS76x8 16M | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Jotale JS76x8 32M | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Jotale JS76x8 8M | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Kimax U25AWF H1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Kimax U35WF | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Kingston MLW221 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Kingston MLWG2 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Kroks KNdRt31R16 | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| Kroks KNdRt31R19 | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| Kroks Rt-Cse5 UW DRSIM | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| Kroks Rt-Pot mXw DS RSIM | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| LAVA LR-25G001 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| LB-Link BL-W1200 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Lenovo Newifi D1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Lenovo Y1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Lenovo Y1S | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Linksys E1550 v1 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Linksys E4200 v1 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Linksys E7350 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Linksys EA6350 v4 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Linksys EA7300 v1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Linksys EA7300 v2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Linksys EA7500 v2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Linksys EA8100 v1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Linksys EA8100 v2 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Mediatek AP-MT7621A-V60 EVB | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| MediaTek LinkIt Smart 7688 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| MediaTek MT7628 EVB | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| MeiG SLT866 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Microduino MicroWRT | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| MikroTik LtAP-2HnD | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| MikroTik RouterBOARD 750Gr3 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| MikroTik RouterBOARD 760iGS | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Minew G1-C | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| MQmaker WiTi | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| MTC Wireless Router WR1201 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Netcore NW5212 | `ramips/mt7620` | `mipsel_24kc` | 23.05 |
| NETGEAR Nighthawk AC2100 v1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR Nighthawk AC2400 v1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR PR2000 | `ramips/mt7620` | `mipsel_24kc` | 23.05 |
| NETGEAR R6120 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| NETGEAR R6200 v1 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR R6220 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR R6260 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR R6350 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR R6700 v2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR R6800 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR R6850 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR R6900 v2 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| NETGEAR R7200 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| NETGEAR R7450 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| NETGEAR WAC104 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR WAC124 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR WNDR3400 v1 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNDR3400 v2 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNDR3400 v3 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNDR3700 v3 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNDR3700 v5 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| NETGEAR WNDR4000 v1 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNR3500L v1 (NA) | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNR3500L v1 (ROW) | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| NETGEAR WNR3500L v2 | `bcm47xx/mips74k` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| netis N6 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| NETIS WF2881 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Nexx WT3020 8M | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Nokia A-040W-Q | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Oh Yeah OY-0001 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Omnima HPM | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Onion Omega2 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Onion Omega2+ | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| OpenFi 5Pro  | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| OrayBox X1 | `ramips/mt76x8` | `mipsel_24kc` | 23.05 |
| PandoraBox PBR-D1 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| PandoraBox PBR-M1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Planex CS-QR10 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Planex VR500 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| RAISECOM MSG1500 X.00 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Rakwireless RAK633 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| RAVPower RP-WD009 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| RAVPower RP-WD03 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Rostelecom RT-SF-1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Routerich AX1800 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| SamKnows Whitebox 8 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Samsung CY-SWR1100 | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Sanlinking Technologies D240 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| SERCOMM NA502 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| SERCOMM NA502S | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Sercomm NA930 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Sercomm S1500 AWI | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Sercomm S1500 BUC | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Sercomm S3 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Sitecom WLR-4100 v1 002 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Sitecom WLR-6000 | `ramips/rt3883` | MIPS LE `.tar.gz` (`mipsel_74kc`, sin probar) | 21.02 |
| Skylab SKW92A | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| SNR SNR-CPE-ME1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| SNR SNR-CPE-ME2-SFP | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| STORYLiNK SAP-G3200U3 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Tama W06 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Telco Electronics X1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Tenbay T-MB5EU-V01 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Thunder Timecloud | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| TOTOLINK LR1200 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TOZED ZLT S12 PRO | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| TP-Link Archer C2 v1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| TP-Link Archer C20 v1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| TP-Link Archer C20i | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| TP-Link Archer C5 v4 | `ramips/mt7620` | `mipsel_24kc` | 24.10 |
| TP-Link Archer C50 v1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| TP-Link Archer C6U v1 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| TP-Link Archer C9ERT | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| TP-Link Archer MR200 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| TP-Link Archer MR200 v5 | `ramips/mt76x8` | `mipsel_24kc` | 24.10 |
| TP-Link EC330-G5u v1 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| TP-Link ER605 v2 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| TP-Link MR600 v2 (EU) | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| TP-Link TL-MR3020 v3 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TP-Link TL-MR3420 v5 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TP-Link TL-MR6400 v4 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TP-Link TL-MR6400 v5 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TP-Link TL-WR842N v5 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TP-Link TL-WR902AC v3 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| TP-Link TL-WR902AC v4 | `ramips/mt76x8` | `mipsel_24kc` | 24.10 |
| UniElec U7621-01 16M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| UniElec U7621-06 16M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| UniElec U7621-06 32M | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| UniElec U7621-06 64M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| UniElec U7628-01 16M | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| VoCore VoCore2 | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| VoCore VoCore2-Lite | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Wansview NCS601W | `ramips/rt305x` | `mipsel_24kc` | 21.02 |
| Wavlink QUANTUM D4 | `ramips/mt76x8` | `mipsel_24kc` | 22.03 |
| Wavlink WL-WN531A3 | `ramips/mt76x8` | `mipsel_24kc` | 22.03 |
| Wavlink WL-WN531A6 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Wavlink WL-WN531G3 | `ramips/mt7620` | `mipsel_24kc` | 24.10 |
| Wavlink WL-WN531G3-A2 | `ramips/mt7620` | `mipsel_24kc` | 24.10 |
| Wavlink WL-WN533A8 | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Wavlink WS-WN572HP3 4G | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| WeVO 11AC NAS Router | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| WeVO AIR DUO | `ramips/mt7620` | `mipsel_24kc` | 22.03 |
| WeVO W2914NS v2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Widora Widora-NEO 16M | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Widora Widora-NEO 32M | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| WiFire S1500.NBN | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Wiflyer WF3526-P | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Winstars WS-WN536P3 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| WRTNode WRTNode | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| WRTnode WRTnode 2P | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| WRTnode WRTnode 2R | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Xiaomi Mi Router 3 Pro | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Xiaomi Mi Router 3G | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Xiaomi MiWiFi Mini | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| XiaoYu XY-C5 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| YouHua WR1200JS | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Youku X2 | `ramips/mt7620` | `mipsel_24kc` | 23.05 |
| Youku YK-L1 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Youku YK-L1c | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Youku YK-L2 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Z-ROUTER ZR-2660 | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Zbtlink ZBT-CPE102 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WA05 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE1026-5G 16M | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE1026-H 32M | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE1326 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE2426-B | `ramips/mt76x8` | `mipsel_24kc` | 24.10 |
| Zbtlink ZBT-WE3526 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE826 16M | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE826 32M | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WE826-E | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WG108 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| Zbtlink ZBT-WG1602 16M | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Zbtlink ZBT-WG1602-V04 16M | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Zbtlink ZBT-WG1602-V04 32M | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Zbtlink ZBT-WG1608 16M | `ramips/mt7621` | `mipsel_24kc` | 22.03 |
| Zbtlink ZBT-WG1608 32M | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Zbtlink ZBT-WG2626 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WG3526 16M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| Zbtlink ZBT-WG3526 32M | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ZIO FREEZIO | `ramips/mt7621` | `mipsel_24kc` | 21.02 |
| ZTE MF283+ | `ramips/rt305x` | `mipsel_24kc` | 21.02 |
| ZTE Q7 | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zyxel Keenetic Extra II | `ramips/mt76x8` | `mipsel_24kc` | 21.02 |
| Zyxel Keenetic Omni | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zyxel Keenetic Omni II | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zyxel Keenetic Viva | `ramips/mt7620` | `mipsel_24kc` | 21.02 |
| Zyxel LTE3301-PLUS | `ramips/mt7621` | `mipsel_24kc` | 23.05 |
| Zyxel LTE5398-M904 | `ramips/mt7621` | `mipsel_24kc` | 24.10 |
| Zyxel NR7101 | `ramips/mt7621` | `mipsel_24kc` | 21.02 |

</details>

<details>
<summary><b>MIPS BE</b>: 306 routers</summary>

| Router | Destino | Descarga | Desde |
|---|---|---|---|
| 1&1 HomeServer | `lantiq/xway` | `mips_24kc` | 21.02 |
| 8devices Carambola2 | `ath79/generic` | `mips_24kc` | 21.02 |
| 8devices Carambola3 | `ath79/generic` | `mips_24kc` | 24.10 |
| 8devices Lima | `ath79/generic` | `mips_24kc` | 21.02 |
| 8devices Rambutan | `ath79/nand` | `mips_24kc` | 21.02 |
| ADT Pulse Gateway | `ath79/nand` | `mips_24kc` | 23.05 |
| Aerohive HiveAP 121 | `ath79/nand` | `mips_24kc` | 21.02 |
| AirTight Networks C-75 | `ath79/generic` | `mips_24kc` | 21.02 |
| Alcatel HH40V | `ath79/generic` | `mips_24kc` | 23.05 |
| ALFA Network AP121F | `ath79/generic` | `mips_24kc` | 21.02 |
| ALFA Network AP121FE | `ath79/generic` | `mips_24kc` | 21.02 |
| ALFA Network N2Q | `ath79/generic` | `mips_24kc` | 21.02 |
| ALFA Network Pi-WiFi4 | `ath79/generic` | `mips_24kc` | 21.02 |
| ALFA Network R36A | `ath79/generic` | `mips_24kc` | 21.02 |
| ALFA Network WiFi CampPro Nano Duo | `ath79/generic` | `mips_24kc` | 24.10 |
| Arcadyan ARV7510PW22 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Arcadyan ARV7518PW | `lantiq/xway` | `mips_24kc` | 21.02 |
| Arcadyan ARV7519PW | `lantiq/xway` | `mips_24kc` | 21.02 |
| Arcadyan ARV752DPW | `lantiq/xway` | `mips_24kc` | 21.02 |
| Arcadyan ARV752DPW22 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Arcadyan ARV8539PW22 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Arcadyan VGV7510KW22 BRN | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Arcadyan VGV7510KW22 NOR | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Arcadyan VGV7519 BRN | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Arcadyan VGV7519 NOR | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Arcadyan VRV9510KWAC23 | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| Arduino Yun | `ath79/generic` | `mips_24kc` | 21.02 |
| Aruba AP-115 | `ath79/generic` | `mips_24kc` | 23.05 |
| Astoria Networks ARV7510PW22 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Astoria Networks ARV7518PW | `lantiq/xway` | `mips_24kc` | 21.02 |
| Astoria Networks ARV7519PW | `lantiq/xway` | `mips_24kc` | 21.02 |
| ASUS RT-AC1200GE | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC1300G PLUS v3 | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC1500G PLUS | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC1500UHP | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC57U v2 | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC57U v3 | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC58U v2 | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC58U v3 | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC59U | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-AC59U v2 | `ath79/generic` | `mips_24kc` | 24.10 |
| ASUS RT-ACRH12 | `ath79/generic` | `mips_24kc` | 24.10 |
| Atheros DB120 | `ath79/generic` | `mips_24kc` | 22.03 |
| AudioCodes MediaPack MP-252 | `lantiq/xway` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 3370 Rev. 2 (Hynix NAND) | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 3370 Rev. 2 (Micron NAND) | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 3390 | `lantiq/xrx200` | `mips_24kc` | 22.03 |
| AVM FRITZ!Box 3490 Micron NAND | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| AVM FRITZ!Box 3490 Other NAND | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| AVM FRITZ!Box 5490 Micron NAND | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| AVM FRITZ!Box 5490 Other NAND | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| AVM FRITZ!Box 5491 | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| AVM FRITZ!Box 7320 | `lantiq/xway` | `mips_24kc` | 21.02 |
| AVM Fritz!Box 7330 | `lantiq/xway` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 7360 SL | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 7360 v2 | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 7362 SL | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 7430 | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| AVM FRITZ!Box 7490 Micron NAND | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| AVM FRITZ!Box 7490 Other NAND | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| Belkin F9J1108 v2 (AC1750 DB Wi-Fi) | `ath79/generic` | `mips_24kc` | 21.02 |
| Belkin F9K1115 v2 (AC1750 DB Wi-Fi) | `ath79/generic` | `mips_24kc` | 21.02 |
| British Telecom (BT) Business Hub 5 Type A | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| British Telecom (BT) Home Hub 5 Type A | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Buffalo BHR-4GRV | `ath79/generic` | `mips_24kc` | 21.02 |
| Buffalo WBMR-300HPD | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Buffalo WBMR-HP-G300H A | `lantiq/xway` | `mips_24kc` | 21.02 |
| Buffalo WBMR-HP-G300H B | `lantiq/xway` | `mips_24kc` | 21.02 |
| Buffalo WZR-600DHP | `ath79/generic` | `mips_24kc` | 21.02 |
| Buffalo WZR-HP-AG300H | `ath79/generic` | `mips_24kc` | 21.02 |
| Buffalo WZR-HP-G300NH (RTL8366RB switch) | `ath79/generic` | `mips_24kc` | 21.02 |
| Buffalo WZR-HP-G300NH (RTL8366S switch) | `ath79/generic` | `mips_24kc` | 21.02 |
| Buffalo WZR-HP-G302H A1A0 | `ath79/generic` | `mips_24kc` | 21.02 |
| Buffalo WZR-HP-G450H/WZR-450HP | `ath79/generic` | `mips_24kc` | 21.02 |
| COMFAST CF-E380AC v2 | `ath79/generic` | `mips_24kc` | 24.10 |
| COMFAST CF-E5/E7 | `ath79/generic` | `mips_24kc` | 21.02 |
| COMFAST CF-E560AC | `ath79/generic` | `mips_24kc` | 21.02 |
| COMFAST CF-EW71 v2 | `ath79/generic` | `mips_24kc` | 24.10 |
| COMFAST CF-EW72 | `ath79/generic` | `mips_24kc` | 21.02 |
| COMFAST CF-WR752AC v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| Compex WPJ344 16M | `ath79/generic` | `mips_24kc` | 21.02 |
| Compex WPJ531 16M | `ath79/generic` | `mips_24kc` | 21.02 |
| Compex WPJ563 | `ath79/generic` | `mips_24kc` | 21.02 |
| D-Link DCH-G020 A1 | `ath79/generic` | `mips_24kc` | 21.02 |
| D-Link DIR-505 | `ath79/generic` | `mips_24kc` | 21.02 |
| D-Link DIR-825 B1 | `ath79/generic` | `mips_24kc` | 21.02 |
| D-Link DIR-825 C1 | `ath79/generic` | `mips_24kc` | 21.02 |
| D-Link DIR-835 A1 | `ath79/generic` | `mips_24kc` | 21.02 |
| D-Link DIR-842 C2 | `ath79/generic` | `mips_24kc` | 21.02 |
| Dell SonicPoint ACe (APL26-0AE) | `ath79/generic` | `mips_24kc` | 24.10 |
| devolo WiFi pro 1750e | `ath79/generic` | `mips_24kc` | 21.02 |
| DomyWifi DW33D | `ath79/nand` | `mips_24kc` | 21.02 |
| Dongwon T&I DW02-412H (128M) | `ath79/nand` | `mips_24kc` | 22.03 |
| Dongwon T&I DW02-412H (64M) | `ath79/nand` | `mips_24kc` | 22.03 |
| ELECOM WAB-I1750-PS | `ath79/generic` | `mips_24kc` | 24.10 |
| ELECOM WAB-S1167-PS | `ath79/generic` | `mips_24kc` | 24.10 |
| ELECOM WAB-S600-PS | `ath79/generic` | `mips_24kc` | 24.10 |
| Embedded Wireless Balin | `ath79/generic` | `mips_24kc` | 22.03 |
| Embedded Wireless Dorin | `ath79/generic` | `mips_24kc` | 21.02 |
| EnGenius EPG5000 | `ath79/generic` | `mips_24kc` | 21.02 |
| EnGenius ESR1200 | `ath79/generic` | `mips_24kc` | 23.05 |
| EnGenius ESR1750 | `ath79/generic` | `mips_24kc` | 23.05 |
| EnGenius ESR900 | `ath79/generic` | `mips_24kc` | 23.05 |
| eTactica EG200 | `ath79/generic` | `mips_24kc` | 21.02 |
| Fortinet FAP-220-B | `ath79/generic` | `mips_24kc` | 24.10 |
| GL.iNet 6408 | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet 6416 | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR150 | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR300M Lite | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR300M NAND | `ath79/nand` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR300M NOR | `ath79/nand` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR300M16 | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR750 | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR750S NOR | `ath79/nand` | `mips_24kc` | 21.02 |
| GL.iNet GL-AR750S NOR/NAND | `ath79/nand` | `mips_24kc` | 21.02 |
| GL.iNet GL-E750 | `ath79/nand` | `mips_24kc` | 21.02 |
| GL.iNET GL-MiFi | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-S200 NOR | `ath79/nand` | `mips_24kc` | 24.10 |
| GL.iNet GL-S200 NOR/NAND | `ath79/nand` | `mips_24kc` | 24.10 |
| GL.iNet GL-X1200 NOR | `ath79/nand` | `mips_24kc` | 23.05 |
| GL.iNet GL-X1200 NOR/NAND | `ath79/nand` | `mips_24kc` | 23.05 |
| GL.iNet GL-X300B | `ath79/generic` | `mips_24kc` | 22.03 |
| GL.iNet GL-X750 | `ath79/generic` | `mips_24kc` | 21.02 |
| GL.iNet GL-XE300 | `ath79/nand` | `mips_24kc` | 22.03 |
| Hak5 LAN Turtle | `ath79/generic` | `mips_24kc` | 21.02 |
| Hak5 Packet Squirrel | `ath79/generic` | `mips_24kc` | 21.02 |
| Hak5 WiFi Pineapple NANO | `ath79/generic` | `mips_24kc` | 21.02 |
| HiWiFi HC6361 | `ath79/generic` | `mips_24kc` | 23.05 |
| I-O DATA WN-AC1167DGR | `ath79/generic` | `mips_24kc` | 21.02 |
| I-O DATA WN-AC1600DGR | `ath79/generic` | `mips_24kc` | 21.02 |
| I-O DATA WN-AC1600DGR2/DGR3 | `ath79/generic` | `mips_24kc` | 21.02 |
| I-O DATA WN-AG300DGR | `ath79/generic` | `mips_24kc` | 21.02 |
| KPN Experiabox 8 BRN | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| KPN Experiabox 8 NOR | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| KT GiGA WiFi home (128M) | `ath79/nand` | `mips_24kc` | 22.03 |
| KT GiGA WiFi home (64M) | `ath79/nand` | `mips_24kc` | 22.03 |
| KuWFi C910 | `ath79/generic` | `mips_24kc` | 23.05 |
| Lantiq VR9 EASY80920 NAND | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Lantiq VR9 EASY80920 NOR | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Librerouter LibreRouter v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| Linksys EA4500 v3 | `ath79/nand` | `mips_24kc` | 22.03 |
| Livebox Next | `lantiq/xrx200` | `mips_24kc` | 24.10 |
| Mercury MW4530R v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| MikroTik RouterBOARD 493G | `ath79/mikrotik` | `mips_24kc` | 23.05 |
| MikroTik RouterBOARD 912UAG-2HPnD | `ath79/mikrotik` | `mips_24kc` | 23.05 |
| Mikrotik RouterBOARD 912UAG-5HPnD | `ath79/mikrotik` | `mips_24kc` | 23.05 |
| MikroTik RouterBOARD 922UAGS-5HPacD | `ath79/mikrotik` | `mips_24kc` | 23.05 |
| MikroTik RouterBOARD 951G-2HnD | `ath79/mikrotik` | `mips_24kc` | 23.05 |
| MikroTik RouterBOARD 951Ui-2HnD | `ath79/mikrotik` | `mips_24kc` | 23.05 |
| MikroTik RouterBOARD 962UiGS-5HacT2HnT (hAP ac) | `ath79/mikrotik` | `mips_24kc` | 22.03 |
| MikroTik RouterBOARD mAP-2nD (mAP) | `ath79/mikrotik` | `mips_24kc` | 21.02 |
| MikroTik RouterBOARD wAPR-2nD (wAP R) | `ath79/mikrotik` | `mips_24kc` | 21.02 |
| Mojo Networks C-75 | `ath79/generic` | `mips_24kc` | 21.02 |
| NEC Aterm WG1400HP | `ath79/generic` | `mips_24kc` | 24.10 |
| NEC Aterm WG1800HP | `ath79/generic` | `mips_24kc` | 24.10 |
| NEC Aterm WG1800HP2 | `ath79/generic` | `mips_24kc` | 24.10 |
| NEC Aterm WG600HP | `ath79/tiny` | `mips_24kc` | 24.10 |
| NEC Aterm WR8750N | `ath79/tiny` | `mips_24kc` | 24.10 |
| NEC Aterm WR9500N | `ath79/tiny` | `mips_24kc` | 24.10 |
| NETGEAR DGN3500 | `lantiq/xway` | `mips_24kc` | 21.02 |
| NETGEAR DGN3500B | `lantiq/xway` | `mips_24kc` | 21.02 |
| NETGEAR PGZNG1 | `ath79/nand` | `mips_24kc` | 23.05 |
| NETGEAR R6100 | `ath79/nand` | `mips_24kc` | 22.03 |
| NETGEAR WNDR3700 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNDR3700 v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNDR3700 v4 | `ath79/nand` | `mips_24kc` | 21.02 |
| NETGEAR WNDR3800 | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNDR3800CH | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNDR4300 | `ath79/nand` | `mips_24kc` | 21.02 |
| NETGEAR WNDR4300 v2 | `ath79/nand` | `mips_24kc` | 21.02 |
| NETGEAR WNDR4300SW | `ath79/nand` | `mips_24kc` | 21.02 |
| NETGEAR WNDR4300TN | `ath79/nand` | `mips_24kc` | 21.02 |
| NETGEAR WNDR4500 v3 | `ath79/nand` | `mips_24kc` | 21.02 |
| NETGEAR WNDRMAC v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNDRMAC v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNR2200 16M | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNR2200 8M | `ath79/generic` | `mips_24kc` | 21.02 |
| NETGEAR WNR2200 CN/RU | `ath79/generic` | `mips_24kc` | 21.02 |
| o2 Box 6431 BRN | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| o2 Box 6431 NOR | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| Onion Omega | `ath79/generic` | `mips_24kc` | 21.02 |
| OpenMesh A40 | `ath79/generic` | `mips_24kc` | 22.03 |
| OpenMesh A60 | `ath79/generic` | `mips_24kc` | 22.03 |
| PISEN Cloud Easy Power (WMM003N) | `ath79/generic` | `mips_24kc` | 21.02 |
| PISEN TS-D084 | `ath79/generic` | `mips_24kc` | 21.02 |
| PISEN WMB001N | `ath79/generic` | `mips_24kc` | 21.02 |
| Plusnet Hub One | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| PowerCloud Systems CR5000 | `ath79/generic` | `mips_24kc` | 21.02 |
| Qihoo C301 | `ath79/generic` | `mips_24kc` | 21.02 |
| Qualcomm Atheros AP143 (16M) | `ath79/generic` | `mips_24kc` | 22.03 |
| Qualcomm Atheros AP143 (8M) | `ath79/generic` | `mips_24kc` | 22.03 |
| Qxwlan E1700AC v2 (16M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E1700AC v2 (8M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E558 v2 (16M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E558 v2 (8M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E600G v2 (16M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E600G v2 (8M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E750A v4 (16M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E750A v4 (8M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E750G v8 (16M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Qxwlan E750G v8 (8M) | `ath79/generic` | `mips_24kc` | 21.02 |
| Rosinson WR818 | `ath79/generic` | `mips_24kc` | 21.02 |
| Ruckus ZoneFlex 7321[-U] | `ath79/generic` | `mips_24kc` | 22.03 |
| Ruckus ZoneFlex 7341[-U] | `ath79/generic` | `mips_24kc` | 23.05 |
| Ruckus ZoneFlex 7343[-U] | `ath79/generic` | `mips_24kc` | 23.05 |
| Ruckus ZoneFlex 7351[-U] | `ath79/generic` | `mips_24kc` | 23.05 |
| Ruckus ZoneFlex 7352/7372[-E/-U] | `ath79/generic` | `mips_24kc` | 22.03 |
| Ruckus ZoneFlex 7363[-U] | `ath79/generic` | `mips_24kc` | 23.05 |
| Samsung WAM250 | `ath79/generic` | `mips_24kc` | 21.02 |
| Sitecom WLR-7100 | `ath79/generic` | `mips_24kc` | 21.02 |
| Sitecom WLR-8100 | `ath79/generic` | `mips_24kc` | 21.02 |
| Sitecom X8 AC1750 | `ath79/generic` | `mips_24kc` | 21.02 |
| SonicWall SonicPoint ACe (APL26-0AE) | `ath79/generic` | `mips_24kc` | 24.10 |
| Sophos AP100 | `ath79/generic` | `mips_24kc` | 22.03 |
| Sophos AP55 | `ath79/generic` | `mips_24kc` | 22.03 |
| Telco T1 | `ath79/generic` | `mips_24kc` | 21.02 |
| Telekom Speedport W504V Typ A | `lantiq/xway` | `mips_24kc` | 21.02 |
| Teltonika RUT230 v1 | `ath79/generic` | `mips_24kc` | 22.03 |
| Teltonika RUT300 | `ath79/generic` | `mips_24kc` | 23.05 |
| Teltonika RUT955 | `ath79/generic` | `mips_24kc` | 21.02 |
| Teltonika RUT955 H7V3C0 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer A7 v5 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer A9 v6 | `ath79/generic` | `mips_24kc` | 22.03 |
| TP-Link Archer C5 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer C59 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer C59 v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer C7 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer C7 v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer C7 v4 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer C7 v5 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer D50 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer D7 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer D7b v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link Archer VR200 v1 | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| TP-Link Archer VR200v v1 | `lantiq/xrx200` | `mips_24kc` | 21.02 |
| TP-Link TD-W8970 v1 | `lantiq/xrx200_legacy` | `mips_24kc` | 21.02 |
| TP-Link TD-W8980 v1 | `lantiq/xrx200_legacy` | `mips_24kc` | 21.02 |
| TP-LINK TD-W9980 v1 | `lantiq/xrx200_legacy` | `mips_24kc` | 21.02 |
| TP-LINK TD-W9980B v1 | `lantiq/xrx200_legacy` | `mips_24kc` | 21.02 |
| TP-Link TL-MR6400 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR3500 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR3600 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR4300 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR4300 v1 (IL) | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR4310 v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR4900 v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WDR6500 v2 | `ath79/generic` | `mips_24kc` | 24.10 |
| TP-Link TL-WDR7500 v3 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR1043N/ND v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR1043N/ND v3 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR1043N/ND v4 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR1045ND v2 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR2543N/ND v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR810N v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR842N v3 | `ath79/generic` | `mips_24kc` | 21.02 |
| TP-Link TL-WR902AC v1 | `ath79/generic` | `mips_24kc` | 21.02 |
| Trendnet TEW-673GRU v1.0R | `ath79/generic` | `mips_24kc` | 22.03 |
| Trendnet TEW-823DRU v1.0R | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti airCube AC | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti airCube ISP | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti AirGrid M5 HP XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti AmpliFi Router HD | `ath79/generic` | `mips_24kc` | 24.10 |
| Ubiquiti Bullet AC | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Bullet M XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti EdgeSwitch 5XP | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti EdgeSwitch 8XP | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti LiteAP ac (LAP-120) | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti NanoBeam AC Gen1 | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti NanoBeam AC Gen1 (XC) | `ath79/generic` | `mips_24kc` | 22.03 |
| Ubiquiti NanoBeam AC Gen2 | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti NanoBeam M5 XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Nanostation AC | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Nanostation AC loco | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Nanostation Loco M XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Nanostation M XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti PowerBeam M2 XW | `ath79/generic` | `mips_24kc` | 22.03 |
| Ubiquiti PowerBeam M5 300 XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti PowerBeam M5 XW | `ath79/generic` | `mips_24kc` | 22.03 |
| Ubiquiti PowerBridge M XM | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Rocket M XM | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti Rocket M XW | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti RouterStation | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti RouterStation Pro | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti UniFi AC Pro | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti UniFi AP | `ath79/generic` | `mips_24kc` | 23.05 |
| Ubiquiti UniFi AP LR | `ath79/generic` | `mips_24kc` | 23.05 |
| Ubiquiti UniFi AP Outdoor+ | `ath79/generic` | `mips_24kc` | 21.02 |
| Ubiquiti UniFi AP Pro | `ath79/generic` | `mips_24kc` | 21.02 |
| Vodafone Easybox 802 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Vodafone Easybox 803 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Wallys DR531 | `ath79/generic` | `mips_24kc` | 21.02 |
| WatchGuard AP320 | `ath79/generic` | `mips_24kc` | 21.02 |
| Western Digital My Net N600 | `ath79/generic` | `mips_24kc` | 22.03 |
| Western Digital My Net N750 | `ath79/generic` | `mips_24kc` | 21.02 |
| Winchannel WB2000 | `ath79/generic` | `mips_24kc` | 21.02 |
| ZBT WD323 | `ath79/generic` | `mips_24kc` | 21.02 |
| ZTE MF281 | `ath79/nand` | `mips_24kc` | 22.03 |
| ZTE MF282 | `ath79/nand` | `mips_24kc` | 23.05 |
| ZTE MF286 | `ath79/nand` | `mips_24kc` | 22.03 |
| ZTE MF286A | `ath79/nand` | `mips_24kc` | 22.03 |
| ZTE MF286R | `ath79/nand` | `mips_24kc` | 22.03 |
| Zyxel EMG2926-Q10A | `ath79/nand` | `mips_24kc` | 22.03 |
| Zyxel NBG6616 | `ath79/generic` | `mips_24kc` | 21.02 |
| Zyxel NBG6716 | `ath79/nand` | `mips_24kc` | 21.02 |
| Zyxel P-2601HN F1/F3 | `lantiq/xway` | `mips_24kc` | 21.02 |
| Zyxel P-2812HNU F1 | `lantiq/xrx200` | `mips_24kc` | 21.02 |

</details>

<details>
<summary><b>x86_64</b>: 1 router</summary>

| Router | Destino | Descarga | Desde |
|---|---|---|---|
| Generic x86/64 | `x86/64` | `x86_64` | 21.02 |

</details>

<details>
<summary><b>x86</b>: 1 router</summary>

| Router | Destino | Descarga | Desde |
|---|---|---|---|
| Generic x86 | `x86/generic` | `i386_pentium4` | 21.02 |

</details>
<!-- /routers -->

Generado a partir de los metadatos de compilación de OpenWrt mediante `doc/openwrt.py --doc doc/openwrt.md`. Un router aparece cuando su imagen de OpenWrt incluye controladores USB.
