# Zephyr RTOS на FPGA-платі PYNQ-Z1 (повна технічна версія)

*Повна інженерна документація до [статті на DOU](https://dou.ua/forums/topic/63691/): запуск Zephyr RTOS 4.5.0 на процесорній частині (PS) Zynq-7000 без встановлення Vivado, карта регістрів, out-of-tree HWMv2 порт плати, Ethernet, двоступеневий TFTP netboot, асемблерний патч GICv1/v2 (`#122600`) та експериментальний 2-ядерний SMP.*

Плати на базі **Xilinx (AMD) Zynq-7000** дуже популярні серед інженерів: такі SoC ставлять у свої рішення багато виробників — від Digilent (PYNQ-Z1, Zybo, Arty Z7) та Avnet (ZedBoard, MicroZed) до ALINX та інших. Причина проста: в одному чипі поєднано повноцінний двоядерний процесор **ARM Cortex-A9** (частина **PS** — *Processing System*) і матрицю програмованої логіки **FPGA** (частина **PL** — *Programmable Logic*).

У мене під рукою є **Digilent PYNQ-Z1** на чипі `XC7Z020`. Окрім двох ядер Cortex-A9 (650 МГц), 512 МБ DDR3 та гігабітного Ethernet, її FPGA-частина класу Artix-7 має близько 85 тисяч логічних комірок (53 200 LUT), ~4.9 Мбіт блочної пам’яті BRAM і 220 DSP-блоків. На самій платі виведені HDMI In/Out, аудіо, два роз’єми Pmod і колодка формату Arduino/chipKIT, підключені до програмованої логіки. Це дає змогу обробляти відео, високошвидкісні потоки з АЦП або генерувати апаратні сигнали (наприклад, для керування моторами дрона) прямо в логіці, не навантажуючи процесор.

![Плата Digilent PYNQ-Z1 (Xilinx Zynq-7000 XC7Z020)](img/pynq-z1.png)

Для embedded-програмістів із базовим досвідом STM32/ESP32 та Linux/Git такий перехід відкриває ширший простір для інженерної творчості без різкої зміни інструментів. Замість фіксованого набору периферії та кількох сотень кілобайтів ОЗП ви отримуєте 512 МБ DDR3, гігабітний Ethernet, два ядра з MMU (де можна на практиці розібратися з кешами, віртуальною пам’яттю, багатоядерним плануванням або запуском RTOS поруч із Linux) і матрицю FPGA, у якій за потреби можна «добудувати» власну периферію. І все це — на тому самому Zephyr RTOS із Devicetree, Kconfig і `west`. Повний покроковий рецепт налаштування хост-машини я виніс у [`README.md`](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/blob/main/README.md) та [`setup_and_build.sh`](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/blob/main/setup_and_build.sh), а тут зосереджуся на інженерних рішеннях і граблях, на які натрапив на живому залізі.

Зазвичай із PYNQ-Z1 працюють у Linux із Python/Jupyter, а для низькорівневої розробки ставлять AMD Vivado/Vitis на десятки гігабайтів. Я перевірив інший підхід: запустив **Zephyr RTOS 4.5.0** на процесорній частині PS без встановлення Vivado (синтез бітстрімів `.bit` для PL тут не розглядається — це окремий наступний етап). Далі — шлях від порожньої папки до консолі UART0, Ethernet, TFTP-завантаження, підтвердженого бага в upstream Zephyr і експериментального 2-ядерного SMP.

---

## 1. Чому Zynq-7000 не стартує як STM32, і де ламаються готові гайди

На мікроконтролері на кшталт STM32 прошивка виконується прямо з вбудованої Flash. У Zynq-7000 старт складніший: після подачі живлення внутрішній BootROM шукає на microSD файл `BOOT.BIN`, у якому запакований початковий завантажувач **FSBL** (*First Stage Boot Loader*) та U-Boot. Саме FSBL піднімає контролер зовнішньої пам’яті **DDR3 (512 МБ, 32-bit)**, тактові генератори PLL і мультиплексор виводів **MIO** (*Multiplexed I/O*). Лише після цього в DDR можна завантажувати `zephyr.bin`.

Чимало інструкцій у мережі радять зібрати `BOOT.BIN` самостійно з upstream U-Boot командою `make DEVICE_TREE="zynq-pynq-z1"` або взяти `ps7_init_gpl.c` із репозиторію `Xilinx/PYNQ` і покласти його в `board/xilinx/zynq/custom_hw_platform/`. На актуальних версіях обидва рецепти не спрацювали:

- в upstream U-Boot (`xilinx_zynq_virt_defconfig`) немає `zynq-pynq-z1`, а чужі конфіги (`zynq-zed` або `zynq-zybo`) мають іншу розводку DDR і виводять консоль на `UART1` (MIO 48/49) замість `UART0` (MIO 14/15);
- у репозиторії `Xilinx/PYNQ` лежать Vivado Tcl-скрипти дизайну, а не готовий `ps7_init_gpl.c`.

Тому я пішов найпростішим робочим шляхом: зробив бекап розділу microSD і використав офіційний `BOOT.BIN` з образу **PYNQ v3.1**. Він уже коректно ініціалізує DDR3, `UART0` на MIO 14/15 та гігабітний Ethernet `gem0` у режимі `rgmii-id`.

### Ключові фізичні адреси PS Zynq-7000 на PYNQ-Z1

| Фізична адреса | Блок / регістр | Призначення в проєкті |
|---|---|---|
| `0x00000000 – 0x000fffff` | Нижній 1 МБ DDR3 | Таблиця векторів ARM (`0x0`), стек/буфери U-Boot |
| `0x00100000 – 0x1fffffff` | DDR3 (`sram0`, 511 МБ) | Виконувана пам’ять Zephyr (`LOAD_ADDR = 0x00100000`) |
| `0xE0000000` | `uart0` (`xuartps`) | USB-UART консоль (`MIO14/15`, 100 МГц, 115200 8N1) |
| `0xE000B000` | `gem0` (`cdns,gem`) | 1G Ethernet MAC + MDIO (`phyaddr 0`, `rgmii-id`) |
| `0xF8000000` | `SLCR` | Системні регістри тактування, pinmux та скиду |
| `0xF8F00000` | `SCU` | Snoop Control Unit (когерентність L1 між CPU0/CPU1) |
| `0xF8F00200` | `GTC` (`arch_timer`) | 64-бітний Cortex-A9 Global Timer (`325 МГц` standalone) |
| `0xF8F01000` / `0xF8F01F00` | `GICD` / `GICD_SGIR` | GICv1 Distributor і генерація між’ядерних SGI (`0..15`) |
| `0xFFFFFFF0` | `OCM` wake vector | Адреса старту CPU1 після виходу з BootROM `WFE` (`sev`) |

```mermaid
flowchart LR
    A[Zynq BootROM] --> B[BOOT.BIN: FSBL + U-Boot]
    B --> C[boot.scr на microSD]
    C -->|TFTP доступний| D[netboot.scr + zephyr.bin з ПК]
    C -->|Fallback за 3 с| E[zephyr.bin з FAT32 microSD]
    D --> F["dcache/icache off → go ENTRY_ADDR"]
    E --> F
    F --> G[Zephyr RTOS 4.5.0]
```

---

## 2. Розгортання Zephyr 4.5.0 і збірка образу під PYNQ-Z1

Усі інструменти я ізолював всередині папки проєкту скриптом [`setup_and_build.sh`](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/blob/main/setup_and_build.sh) (`.venv/`, `.tools/`, `zephyr-ws/`), щоб не засмічувати хост-систему. Повний відтворюваний процес на Ubuntu/Debian виглядає так:

1. **Встановити базові пакети хоста:**
   ```sh
   sudo apt install -y git cmake ninja-build gperf ccache device-tree-compiler \
       wget xz-utils unzip u-boot-tools mtools python3-venv picocom
   ```
2. **Розгорнути робочий простір `west` із Zephyr `4.5.0` і мінімальними модулями:**
   ```sh
   python3 -m venv .venv && source .venv/bin/activate
   pip install west
   west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.5.0 zephyr-ws
   cd zephyr-ws
   west config manifest.project-filter -- "-.*,+cmsis,+picolibc"
   west update --narrow -o=--depth=1
   pip install -r zephyr/scripts/requirements-base.txt
   ```
   *(У маніфесті Zephyr немає окремого модуля `hal_xilinx` — код для Zynq-7000 лежить прямо в основному дереві `soc/xlnx/zynq7000/`. Зверніть увагу, що `project-filter` приймає регулярний вираз, а не glob: `-*` падає з помилкою.)*
3. **Завантажити Zephyr SDK `1.0.1` (`arm-zephyr-eabi`):** у Zephyr `4.5.0` скрипт `FindZephyr-sdk.cmake` відхиляє старіший SDK `0.17.0` і вимагає `1.0.x`. Достатньо мінімального бандлу `1.0.1` лише з архітектурою `arm-zephyr-eabi` (у 1.0.1 шлях до бінарників змінився на `gnu/arm-zephyr-eabi/bin/`).
4. **Застосувати локальні патчі з `patches/` і зібрати образ під наш out-of-tree порт `pynq_z1`:**
   ```sh
   export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
   export ZEPHYR_SDK_INSTALL_DIR="$PWD/.tools/zephyr-sdk-1.0.1"
   west build -p always -b pynq_z1 . -d build -- -DBOARD_ROOT=.
   ```
   Або запустити весь цикл (завантаження SDK, `west`, патчі, збірка `zephyr.bin` та генерація `boot.scr`) однією командою:
   ```sh
   ./setup_and_build.sh                 # автономна збірка під microSD
   ./setup_and_build.sh --tftp-server 192.168.0.10  # з TFTP netboot
   ```

Окремий важливий нюанс під час формування завантажувального скрипту U-Boot: **`LOAD_ADDR` і `ENTRY_ADDR` — це різні адреси.** Сирий `zephyr.bin` не містить ELF-заголовків. Якщо завантажити його командою `fatload` на адресу точки входу, усі абсолютні посилання з’їдуть. Тож `fatload` (або `tftpboot`) має класти бінарник на базову адресу першого сегмента `LOAD`, а `go` — стрибати на `Entry point address`:

$$A_{\text{load}} = \texttt{0x00100000}, \quad A_{\text{entry}} = \texttt{0x00102BBC}, \quad \Delta = \texttt{0x2BBC}\text{ байт}$$

У скрипті обидві адреси автоматично дістаються з `zephyr.elf`:

```sh
LOAD_ADDR=$("$READELF" -lW build/zephyr/zephyr.elf | awk '$1 == "LOAD" {print $4; exit}')
ENTRY_ADDR=$("$READELF" -h build/zephyr/zephyr.elf | awk '/Entry point address:/ {print $4}')
```

Частота системного таймера Cortex-A9 Global Timer (GTC) на PYNQ-Z1 також відрізняється від ZedBoard: процесор працює на $650\text{ МГц}$, а GTC тактується половинною частотою (`CPU_3x2x`):

$$f_{\text{GTC}} = \frac{f_{\text{CPU}}}{2} = \frac{650\text{ МГц}}{2} = 325\text{ МГц}$$

---

## 3. Перший запуск: U-Boot відпрацьовує, а консоль мовчить

Спершу я збирав проєкт на базі готової плати `zedboard` (там той самий `XC7Z020`) з overlay, який перемикав консоль із `uart1` на `uart0` (`0xe0000000`, 100 МГц). Записав `zephyr.bin` і `boot.scr` на microSD, підключив micro-USB до порту `PROG UART`, відкрив `picocom -b 115200 /dev/ttyUSB1`, клацнув живлення `POWER` — і отримав:

```text
## Executing script at 03000000
=== Starting Zephyr RTOS on PYNQ-Z1 ===
32924 bytes read in 14 ms (2.2 MiB/s)
## Starting application at 0x00102BBC ...
```

І далі — тиша.

Без JTAG-дебагера в такій ситуації починаю зі статичної перевірки артефактів збірки. За досвідом роботи з іншими контролерами (наприклад, **AURIX TC499**), коли консоль «мовчить» одразу після передачі керування від завантажувача, проблема майже завжди в одному з трьох місць: порядок ініціалізації драйверів, тактування периферії (UART) або конфігурація виводів (pinmux/pinctrl).

Спершу я перевірив згенеровані `build/zephyr/.config` і `build/zephyr/zephyr.dts` — `CONFIG_UART_XLNX_PS=y`, `CONFIG_UART_CONSOLE=y`, `CONFIG_PINCTRL=y`, а вузол `uart0` (`0xe0000000`) увімкнений і вказаний у `chosen/zephyr,console`. Далі звірив порядок ініціалізації `PRE_KERNEL_1` у `build/zephyr/zephyr.map` (`intc_gic` → `syscon` → `uart_xlnx_ps` → `uart_console` — тут усе було на місці). Після цього відкрив код самого драйвера `drivers/serial/uart_xlnx_ps.c`, і причина стала очевидною: коли в конфігурації увімкнено `CONFIG_PINCTRL=y` (а для Zynq-7000 це за замовчуванням), `uart_xlnx_ps_init()` на самому старті викликає:

```c
err = pinctrl_apply_state(dev_cfg->pincfg, PINCTRL_STATE_DEFAULT);
if (err < 0) {
    return err;
}
```

У згенерованому `build/zephyr/zephyr.dts` (після накладання мого `boards/zedboard.overlay`) у вузла `uart0` не було властивості `pinctrl-0`. У Devicetree ця властивість вказує на блок налаштувань фізичних виводів чипа (альтернативна функція UART, стандарт I/O-буфера під апаратну напругу банку плати — `LVCMOS33` або `LVCMOS18`, підтяжка, швидкість фронту; аналог налаштування *Alternate Function* у STM32CubeMX), які драйвер застосовує перед запуском периферії. Я помилково розраховував, що MIO 14/15 уже налаштував U-Boot, тому `pinctrl_lookup_state()` повертала `-ENOENT`, і драйвер виходив ще до ввімкнення передавача.

Щойно я додав `pinctrl_uart0_default` (`groups = "uart0_1_grp"`, `MIO14`/`MIO15`, `IO_STANDARD_LVCMOS33` — відповідно до апаратної напруги Bank 0 = 3.3 В на PYNQ-Z1, на відміну від Bank 1 = 1.8 В), консоль одразу ожила:

```text
## Starting application at 0x00102BBC ...
*** Booting Zephyr OS build v4.5.0 ***
Hello from Zephyr on PYNQ-Z1
```

---

## 4. Out-of-tree порт `boards/digilent/pynq_z1/` замість `zedboard`

Щоб не залежати від конфігурації чужої плати та додаткового overlay-файла, я виніс опис PYNQ-Z1 в окремий out-of-tree HWMv2 board-порт у каталозі `boards/digilent/pynq_z1/`:

- `board.yml`, `Kconfig.pynq_z1`, `Kconfig.defconfig`, `pynq_z1_defconfig`, `pynq_z1.yaml`;
- `pynq_z1-pinctrl.dtsi` (`uart0_1_grp` на `MIO14/15`, `LVCMOS33`);
- `pynq_z1.dts` (`sram0` 511 МБ із `0x00100000` — у Devicetree Zephyr вузол `chosen/zephyr,sram` описує основну виконувану ОЗП незалежно від того, чи це внутрішня SRAM, чи зовнішня DDR3; `arch_timer` 325 МГц, `uart0` 100 МГц, `psgpio`, `gem0` + `gem0_mdio`).

Після цього збірка запускається прямо під `pynq_z1`:

```sh
west build -p always -b pynq_z1 . -d build -- -DBOARD_ROOT=.
```

Ключові вузли `boards/digilent/pynq_z1/pynq_z1.dts`:

```dts
/ {
    chosen {
        zephyr,sram = &sram0;
        zephyr,console = &uart0;
        zephyr,shell-uart = &uart0;
    };
    sram0: memory@100000 {
        compatible = "mmio-sram";
        reg = <0x00100000 DT_SIZE_M(511)>;
    };
};

&arch_timer { clock-frequency = <325000000>; };

&uart0 {
    current-speed = <115200>;
    clock-frequency = <100000000>;
    pinctrl-0 = <&pinctrl_uart0_default>;
    pinctrl-names = "default";
    status = "okay";
};

&gem0 {
    phy-handle = <&gem0_phy>;
    clock-frequency = <1000000000>;
    local-mac-address = [9e 5c 1a 8a bf 53];
    status = "okay";
};
```

---

## 5. Гігабітний Ethernet, DHCPv4 і тест через `zperf`

Лог U-Boot підказав дві важливі речі про мережу на PYNQ-Z1:

```text
ZYNQ GEM: e000b000, mdio bus e000b000, phyaddr 0, interface rgmii-id
Error: ethernet@e000b000 No valid MAC address found.
```

На платі немає EEPROM із зашитою MAC-адресою, тому без ручного налаштування адреса щоразу генерується випадково. У `pynq_z1.dts` я зафіксував власну локально-адміністровану MAC-адресу (`9e:5c:1a:8a:bf:53`), а в `prj.conf` увімкнув `CONFIG_NET_L2_ETHERNET=y`, `CONFIG_ETH_DRIVER=y`, `CONFIG_NET_DHCPV4=y`, `CONFIG_NET_HOSTNAME="pynq1"` та `CONFIG_NET_SHELL=y`.

Лінк на гігабіт піднявся одразу:

```text
[00:00:00.057,000] <inf> phy_mii: PHY (0) ID 1CC916
[00:00:02.764,000] <inf> phy_mii: PHY (0) Link speed 1000 Mb, full duplex
DHCPv4 lease acquired: IP=192.168.0.11 mask=255.255.255.0 gw=192.168.0.1
```

Мені стало цікаво перевірити реальну пропускну здатність цього каналу під Zephyr — надалі я планую поекспериментувати з пакетним стрімінгом сирими Ethernet-фреймами без IP-стеку, тож хотілося мати базову точку відліку. У самому Zephyr для цього вже є вбудована підсистема **`zperf`** (`CONFIG_NET_ZPERF=y`, `CONFIG_NET_ZPERF_SERVER=y`), яка сумісна зі стандартним Linux-клієнтом `iperf`.

Але при першій спробі `zperf tcp download 5001` консоль відповіла `Failed to start TCP server!`. Перевіривши `subsys/net/lib/sockets/sockets_service.c`, я знайшов причину: сервіс моніторингу сокетів рахує суму poll-дескрипторів усіх увімкнених сервісів (`zperf` UDP + `zperf` TCP + `mDNS` + `eventfd`). Загалом виходило понад 9, тому при `CONFIG_ZVFS_POLL_MAX=9` потік `socket_service` тихо завершувався ще при старті.

Окрім базових мережевих параметрів, у робочому `prj.conf` зібрані всі ключові Kconfig-опції проєкту (консоль/логи, 2-ядерний SMP, мережевий стек, DHCPv4, mDNS, `zperf` та shell):

```ini
CONFIG_PRINTK=y
CONFIG_LOG=y
CONFIG_LOG_MODE_IMMEDIATE=y

# 2-core SMP (CONFIG_FPU=n обходить конфлікт FPU_SHARING + USE_SWITCH у 4.5.0)
CONFIG_USE_SWITCH=y
CONFIG_SMP=y
CONFIG_MP_MAX_NUM_CPUS=2
CONFIG_PM_CPU_OPS=y
CONFIG_SCHED_CPU_MASK=y
CONFIG_KERNEL_SHELL=y
CONFIG_FPU=n

# 1G Ethernet + DHCPv4 + mDNS + zperf
CONFIG_NETWORKING=y
CONFIG_NET_L2_ETHERNET=y
CONFIG_ETH_DRIVER=y
CONFIG_NET_IPV4=y
CONFIG_NET_ARP=y
CONFIG_NET_UDP=y
CONFIG_NET_TCP=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_HOSTNAME_ENABLE=y
CONFIG_NET_HOSTNAME="pynq1"
CONFIG_MDNS_RESPONDER=y
CONFIG_NET_ZPERF=y
CONFIG_NET_ZPERF_SERVER=y
CONFIG_NET_MAX_CONTEXTS=10
CONFIG_ZVFS_POLL_MAX=16
CONFIG_SHELL=y
CONFIG_NET_SHELL=y
```

Після збільшення `CONFIG_ZVFS_POLL_MAX=16` (та `CONFIG_NET_MAX_CONTEXTS=10`) тест `iperf` із ноутбука (через Wi-Fi 6 → роутер → 2.5G-свіч → PYNQ-Z1) пройшов чисто:

```text
uart:~$ zperf tcp download 5001
TCP server started on port 5001
New TCP session started.
TCP session ended
 Duration:        10.05 s
 rate:            21.38 Mbps
```

Цей тест перевіряв стабільність мережевого стеку Zephyr через реальну домашню мережу з Wi-Fi-ділянкою (а не граничну пропускну здатність 1G-лінку по кабелю). Як орієнтовна оцінка порядку величини, для дефолтного TCP-вікна $W = 16\text{ КБ}$ і типової затримки $\text{RTT} \approx 6\text{ мс}$ формула Bandwidth-Delay Product дає близьке значення:

$$R_{\max} \approx \frac{W}{\text{RTT}} = \frac{16 \times 1024 \times 8\text{ біт}}{0.006\text{ с}} \approx 21.8\text{ Мбіт/с}$$

Щоб довести точне вузьке місце (вікно TCP, Wi-Fi чи буфери драйвера GEM), потрібен окремий кабельний тест зі збільшеним TCP-вікном і UDP-профілем `zperf`.

---

## 6. Як я перестав смикати microSD: двоступеневий TFTP netboot

Витягати microSD після кожної перекомпіляції — найшвидший спосіб розхитати слот. Оскільки в U-Boot на PYNQ-Z1 уже є робочий Ethernet, я поставив на ноутбук `atftpd` (каталог `/srv/tftp/pynq1/`) і розділив завантаження на два скрипти:

1. **`boot.scr` (записується на microSD один раз):** задає `ethaddr`, статичні `ipaddr`/`serverip`, короткий таймаут (3 с) і завантажує `pynq1/netboot.scr` на адресу `0x03100000` (щоб не затерти себе в `0x03000000`). Якщо TFTP-сервер вимкнений, скрипт автоматично відкочується на `zephyr.bin` із SD-карти.
2. **`netboot.scr` (оновлюється автоматично на кожному `./setup_and_build.sh`):** завжди містить актуальні `LOAD_ADDR` та `ENTRY_ADDR` свіжої збірки, завантажує `pynq1/zephyr.bin` по TFTP і запускає його.

Критичний момент перед командою `go`: U-Boot працює з увімкненим D-cache/I-cache, тоді як стартап Zephyr (`__start`) очікує чистий стан до ініціалізації MMU, тому `dcache off; icache off` перед `go` обов’язковий:

```sh
# Скрипт boot.cmd (компілюється через: mkimage -A arm -T script -C none -d boot.cmd boot.scr)
setenv ethaddr 9e:5c:1a:8a:bf:53
setenv ipaddr 192.168.0.11
setenv serverip 192.168.0.10
setenv tftptimeout 3000
if tftpboot 0x03100000 pynq1/netboot.scr; then
    source 0x03100000
fi
# Fallback на microSD, якщо TFTP недоступний
fatload mmc 0:1 0x00100000 zephyr.bin
dcache off; icache off
go 0x0010FA8C
```

Тепер весь цикл перевірки нової прошивки звівся до `./setup_and_build.sh` і натискання кнопки **`SRST`** на платі:

```text
=== PYNQ-Z1 Boot: trying TFTP (192.168.0.10) ===
TFTP from server 192.168.0.10; our IP address is 192.168.0.11
Filename 'pynq1/netboot.scr'.
Load address: 0x3100000
Bytes transferred = 210 (d2 hex)
## Executing script at 03100000
=== TFTP Netboot: Zephyr RTOS on PYNQ-Z1 ===
Filename 'pynq1/zephyr.bin'.
Load address: 0x100000
Loading: ####################
     960 KiB/s
Bytes transferred = 293104 (478f0 hex)
## Starting application at 0x0010FA8C ...
```

---

## 7. Друге ядро: баг у GICv1/v2 і експериментальний 2-ядерний SMP

У `soc/xlnx/zynq7000/xc7zxxx/Kconfig.defconfig` у Zephyr 4.5.0 прописано `CONFIG_MP_MAX_NUM_CPUS = 1` із коментарем, що SMP на AArch32 Zynq-7000 не підтримується. На Zynq-7000 немає PSCI: після старту друге ядро спить у циклі `WFE` (*Wait For Event*) у BootROM і чекає, поки CPU0 запише адресу входу в `0xFFFFFFF0` і подасть інструкцію `sev` (*Send Event*).

Щоб перевірити друге ядро окремо, я додав у прошивку невеликий ARM-трамплін (`src/cpu1_trampoline.S`) і shell-команди `cpu1 start` / `cpu1 stop` / `cpu1 status` / `cpu1 ping`. CPU0 записує адресу `cpu1_entry` у `0xFFFFFFF0` і дає `sev`, а CPU1 читає свій `MPIDR`, крутить лічильник `heartbeat` у спільній некешованій пам’яті `__nocache` (`0x00147000`), відповідає на `cpu1 ping <N>` апаратним перериванням **GIC SGI #7** і за командою `cpu1 stop` паркується в `WFE`:

```text
uart:~$ cpu1 start
Waking CPU1 -> entry=0x0010433c (shm=0x147000)...
0xFFFFFFF0 = 0x0010433c
cpu1_shm @ 0x147000: magic=0x43505531 mpidr=0x80000001 (core=1) heartbeat=1 state=RUNNING sgi_irqs=0
CPU1 is ALIVE (RUNNING)
uart:~$ cpu1 ping 42
CPU1 SGI#7 reply: 42^2 = 1764 (seq=1, sgi_irqs=1, rtt=16 us)
uart:~$ cpu1 stop
cpu1_shm @ 0x147000: magic=0x43505531 mpidr=0x80000001 (core=1) heartbeat=49 state=STOPPED sgi_irqs=1
CPU1 is PARKED (WFE)
uart:~$ cpu1 ping 123
CPU1 SGI#7 reply: 123^2 = 15129 (seq=2, sgi_irqs=2, rtt=7 us)
```

Під час цього експерименту та наступної спроби ввімкнути SMP я зіткнувся з двома проблемами у Zephyr 4.5.0 (`41d10b597304`):

1. **Підтверджений баг у релізі `4.5.0` — втрата між’ядерних SGI у GICv1/v2 (`arch/arm/core/cortex_a_r/isr_wrapper.S`).** Коли CPU1 шле переривання SGI #7 на CPU0, апаратний регістр `GICC_IAR` повертає номер ядра-відправника (`CPUID=1`) у бітах `[12:10]`, тобто `0x407` (`1031`). У `isr_wrapper.S` (в обох гілках — із `CONFIG_USE_SWITCH` і без нього) це значення без маскування порівнювалося з `CONFIG_NUM_IRQS` (`96`), і переривання тихо відкидалося як spurious — хоча в `arch/arm64/core/isr_wrapper.S:71-73` маска `bic x0, x0, #0x1c00` уже є. Одна інструкція `ubfx r0, r0, #0, #10` після `push {r0, r1}` (щоб зберегти повний `IAR` на стеку для `arm_gic_eoi`) одразу оживила SGI #7 із затримкою **7 мкс** із режиму `WFE`:

```diff
--- a/arch/arm/core/cortex_a_r/isr_wrapper.S
+++ b/arch/arm/core/cortex_a_r/isr_wrapper.S
@@ -183,6 +183,10 @@ _idle_state_cleared:
 	push {r0, r1}
+#if defined(CONFIG_GIC_V1) || defined(CONFIG_GIC_V2)
+	/* Keep full CPUID|INTID on stack for arm_gic_eoi(), mask r0 to 10-bit INTID */
+	ubfx r0, r0, #0, #10
+#endif
```

   Перевірка upstream показала, що цей самий баг уже зафіксовано в [issue #122600](https://github.com/zephyrproject-rtos/zephyr/issues/122600) і виправлено на гілці `main` після `4.5.0` у [PR #123040](https://github.com/zephyrproject-rtos/zephyr/pull/123040) (маскою `bic r0, r0, #0x1c00`), але для релізної збірки `4.5.0` локальний патч усе ще потрібен. Повний розбір із мінімальним відтворенням лежить у [`docs/zephyr-gic-v1-v2-sgi-bug.md`](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/blob/main/docs/zephyr-gic-v1-v2-sgi-bug.md) та `patches/0001-...patch`.
2. **Конфлікт конфігурації `CONFIG_USE_SWITCH=y` + `CONFIG_FPU_SHARING=y` (`thread.c:160` vs `exc.S:256`).** При ввімкненні `CONFIG_SMP=y` (який вимагає `CONFIG_USE_SWITCH=y`) функція `arch_new_thread()` кладе знизу стеку 232 байти нулів `struct __fpu_sf`, але `z_arm_cortex_ar_exit_exc` їх не знімає — і перший перехід у `main` стрибає на `pc=0x0`. Наразі я обійшов цей конфлікт вимкненням `CONFIG_FPU=n` (повноцінне виправлення FPU-фрейму для `CONFIG_USE_SWITCH` потребує окремої перевірки).

### Експериментальний прототип: 2-ядерний SMP на Zynq-7000

Після цих кроків я зібрав експериментальний прототип 2-ядерного SMP (`patches/0002-...patch` + `src/zynq7000_smp.c`). У `soc_reset_hook()` (виконується на обох ядрах до ввімкнення MMU) додав таку послідовність: інвалідація L1 I-cache, BTAC і L1 D-cache → інвалідація тегів і ввімкнення **SCU** (`0xF8F00000`) на первинному ядрі CPU0 → встановлення `ACTLR.SMP | ACTLR.FW` із бар’єрами `DSB`/`ISB` на кожному ядрі. Для запуску CPU1 реалізував `pm_cpu_on()` через запис точки входу в `0xFFFFFFF0` + `DSB` + `SEV`. Обидва ядра Cortex-A9 запрацювали під єдиним планувальником Zephyr:

```text
uart:~$ smp info
CONFIG_SMP=y, num_cpus=2, current_cpu=1, MPIDR=0x80000001, ACTLR=0x00000041
uart:~$ smp test
Worker[CPU0]: ran on cpu=0 MPIDR=0x80000000 ACTLR=0x00000047 time=4136 us
Worker[CPU1]: ran on cpu=1 MPIDR=0x80000001 ACTLR=0x00000041 time=11552 us
Total wall time (parallel): 11574 us
```

Загальний час `11574 us` практично дорівнює часу повільнішого воркера, тобто обидва потоки реально виконалися паралельно, а сама команда `smp info` при цьому виконалася вже на CPU1. *(Різниця між `4136 us` і `11552 us` пояснюється бітами prefetch у `ACTLR`: U-Boot залишив на CPU0 `0x47`, а на CPU1 виставлено `0x41`. Це поки експериментальний прототип без стрес-тестування тривалого навантаження та FPU, а не готова основа для продакшену.)*

---

## 8. Де доречний 2-ядерний SMP на Zephyr (і коли краще обрати AMP)

Попри популярність зв’язки Linux + RTOS, чистий **2-ядерний SMP на Zephyr** на Zynq-7000 має чітку інженерну нішу, де Linux був би зайвим баластом:

- **Системи з миттєвим стартом (< 1 с) і без файлової системи.** Уся прошивка `zephyr.bin` займає ~300 КБ, не потребує rootfs, не боїться раптового зняття живлення і готова до роботи менш ніж за секунду після U-Boot.
- **Високошвидкісний збір даних (DAQ) і стрімінг по 1G Ethernet.** Одне ядро може обслуговувати DMA-буфери з FPGA PL (наприклад, швидкісні АЦП, SDR або відеокадри в 511 МБ DDR3), а друге — паралельно пакувати й відправляти сирі Ethernet/UDP-фрейми через `gem0` без затримок мережевого стеку Linux.
- **Багатопотокові промислові шлюзи й контролери руху.** Коли потрібні сотні мегабайтів ОЗП для буферів/траєкторій, кілька незалежних шин (Ethernet, SPI, I2C, CAN/PWM у PL) і єдина кодова база на C з прогнозованою затримкою переривань, але без накладних витрат на підтримку Linux-дистрибутиву.
- **Швидке прототипування драйверів FPGA PL.** З одного проєкту Zephyr зручно тестувати власні IP-блоки в PL (AXI MMIO, переривання, DMA) без написання kernel-модулів Linux і без перезбірки ядра.

Водночас, якщо проєкту потрібні SSH, веб-інтерфейс, Python, запис файлів на диск, комп’ютерний зір або готова мережева інфраструктура — краще розділити два ядра в режимі **AMP** (Linux на CPU0 + Zephyr на CPU1), чому присвячена [друга частина](Zephyr-RTOS-on-PYNQ-Z1-AMP-OpenAMP-Part2-UA.md).

---

## 9. Підтримана периферія PYNQ-Z1 у Zephyr 4.5.0

Перед стартом реального проєкту на Zynq-7000 важливо розуміти, яка саме периферія PYNQ-Z1 уже має готові драйвери в дереві Zephyr `4.5.0`, а що доведеться залишати на боці U-Boot/Linux або писати самотужки.

### Процесорна частина (PS)

| Периферія PYNQ-Z1 (PS) | Стан у Zephyr 4.5.0 | Драйвер / примітка |
|---|---|---|
| `UART0` / `UART1` (USB-UART консоль) | Підтримується (перевірено) | `uart_xlnx_ps.c` (потрібен `pinctrl-0`) |
| 1G Ethernet (`gem0` + MDIO, RTL8211F) | Підтримується (перевірено) | `eth_xlnx_gem.c`, `mdio_xlnx_gem.c` (MDIO — шина керування PHY) |
| `pinctrl` (MIO/EMIO, банки 3.3 В / 1.8 В) | Підтримується (перевірено) | `pinctrl_xlnx_zynq.c` (EMIO — виводи PS, прокинуті через PL) |
| Таймери (Global Timer, TTC) і GICv1 | Підтримується (перевірено) | `arm_arch_timer`, `counter_xlnx_ttc.c` (TTC — *Triple Timer Counter*, + патч SGI `#122600`) |
| PS GPIO (MIO/EMIO) | Підтримується | `gpio_xlnx_ps.c`, `gpio_xlnx_ps_bank.c` (лінії загального призначення) |
| I2C / SPI (Cadence PS) | Підтримується | `i2c_cdns.c`, `spi_cdns.c` (послідовні шини сенсорів і периферії) |
| microSD (`sdhci0`, Arasan) | Підтримується | `xlnx_sdhc.c` (SDHCI — контролер SD-карт, `xlnx,versal-8.9a` у `zynq7000.dtsi`) |
| USB Host / OTG (ChipIdea) | Не підтримується | Драйвера Zynq-7000 USB (*On-The-Go*) у Zephyr 4.5.0 немає |
| QSPI Flash (16 МБ на платі) | Не підтримується | У Zephyr є `spi_xlnx_zynqmp_gqspi.c` (лише для ZynqMP; QSPI — *Quad SPI*) |
| XADC (температура кристала, АЦП) | Не підтримується | Вбудований АЦП Zynq; потрібен Linux (`xilinx-xadc`) або окремий драйвер |
| PS DMA (`PL330`) і PS Watchdog | Не підтримується | У Zephyr є лише PL-версії (`AXI DMA` — прямий доступ до пам’яті, `AXI WDT` — сторожовий таймер) |
| DevCfg FPGA Manager (прошивка PL) | Не підтримується в upstream 4.5.0 | *Device Configuration Interface*; в upstream є лише `fpga_zynqmp.c` (для Zynq-7000 потрібен окремий драйвер або прошивка через U-Boot/Linux) |

### Програмована логіка (FPGA PL — AXI IP-блоки)

Якщо у FPGA PL прошитий дизайн зі стандартними IP-ядрами AMD/Xilinx (наприклад, базовий `base.bin` або власний бітстрім), Zephyr `4.5.0` уже має готові драйвери для ключових блоків на шині AXI:

- **GPIO та таймери/PWM:** `gpio_xlnx_axi.c` (AXI GPIO), `counter_xlnx_axi_timer.c`, `pwm_xlnx_axi_timer.c` (апаратний PWM на базі AXI Timer).
- **Послідовні шини:** `i2c_xilinx_axi.c` (AXI IIC), `spi_xlnx_axi_quadspi.c` (AXI Quad SPI), `uart_xlnx_uartlite.c` (AXI UARTLite).
- **Швидкісні канали й контроль:** `dma_xilinx_axi_dma.c` (AXI DMA), `eth_xilinx_axienet.c` / `eth_xilinx_axi_ethernet_lite.c`, `wdt_xilinx_axi.c` (AXI Watchdog).
- **HDMI In/Out та аудіокодек ADAU1761 на PYNQ-Z1:** готової підтримки в Zephyr немає — ці інтерфейси зав’язані на складні відео/аудіо конвеєри в PL і значно природніше працюють під Linux.

### Практичні варіанти роботи з USB на Zynq-7000

Оскільки в Zephyr `4.5.0` немає вбудованого драйвера для контролера Zynq-7000 PS USB (ядро ChipIdea з інтерфейсом ULPI), на практиці є чотири робочі варіанти:

1. **Кастомний UDC-драйвер під новий стек USB (`usbd_next`):** реалізувати обгортку в `drivers/usb/udc/`, перенісши низькорівневу ініціалізацію регістрів, дескрипторів DMA та ULPI PHY з офіційного bare-metal драйвера `usbps` із бібліотеки Xilinx/AMD `embeddedsw`.
2. **Інтеграція стороннього стеку TinyUSB:** швидший шлях без глибокої прив’язки до `usbd_next` — TinyUSB уже має готовий порт для контролерів ChipIdea / Zynq (режими Device і Host) і запускається як окремий потік Zephyr.
3. **Зовнішній SPI USB-контролер (`MAX3421E`):** якщо апаратна частина дозволяє підключення через Pmod або GPIO, у Zephyr `4.5.0` є готовий драйвер `drivers/usb/uhc/uhc_max3421e.c` для режиму USB Host поверх звичайної шини SPI.
4. **Гетерогенна конфігурація (AMP):** для повноцінної роботи складних USB-пристроїв або OTG-режиму найпростіше запустити Linux на CPU0 (де драйвер ChipIdea USB працює «з коробки»), а Zephyr — на CPU1 через OpenAMP / `remoteproc` (детально в [другій частині](Zephyr-RTOS-on-PYNQ-Z1-AMP-OpenAMP-Part2-UA.md)).

Такий розподіл добре підсвічує архітектурну логіку платформи: **чистий Zephyr** ідеально закриває 1G Ethernet, UART, SPI/I2C, microSD та пряму роботу з AXI-периферією в FPGA PL, а коли додаються USB, XADC, PS Watchdog, файлова система чи SSH — найефективніше віддати їх **Linux на CPU0** в режимі AMP, залишивши **Zephyr на CPU1** для жорсткого реального часу.

---

## Висновки

- **Запуск Zephyr на Zynq-7000 без Vivado повністю робочий:** перевикористання офіційного `BOOT.BIN` із PYNQ v3.1 знімає залежність від десятків гігабайтів AMD Vivado/Vitis, коли достатньо процесорної частини PS (або готового бітстріму PL).
- **Zynq-7000 суттєво відрізняється від класичних MCU:** замість лінійного старту з Flash тут працюють BootROM → FSBL → U-Boot, окремі `LOAD_ADDR` і `ENTRY_ADDR`, обов’язкове вимкнення кешів перед `go`, `pinctrl` із прив’язкою до напруг банків MIO та MMU з когерентністю SCU.
- **Out-of-tree HWMv2 board-порт (`boards/digilent/pynq_z1/`) окупається одразу:** замість крихких overlay-файлів поверх чужої плати `zedboard` проєкт отримує чисту конфігурацію саме під залізо PYNQ-Z1 (`325 МГц` GTC, `uart0` 3.3 В, `511 МБ` DDR3, `gem0` 1G Ethernet).
- **Двоступеневий TFTP netboot змінює темп розробки:** один `boot.scr` на microSD і автооновлення `netboot.scr` + `zephyr.bin` по мережі (`960 KiB/s`) прибирають ручне перекидання карти й перетворюють тест прошивки на одну команду та натискання `SRST`.
- **Перевірка на живому залізі виявила реальні баги релізу `4.5.0`:** відсутність маскування `GICC_IAR.CPUID` у `isr_wrapper.S` (підтверджений upstream `#122600` / `PR #123040`, після патча SGI #7 працює за **7 мкс**) та конфлікт стекового фрейму `CONFIG_USE_SWITCH` + `CONFIG_FPU_SHARING`.
- **Чистий 2-ядерний SMP на Zephyr — сильна база для автономних RT-систем, а AMP — логічний наступний крок:** експериментальний SMP довів паралельну роботу обох Cortex-A9 під одним планувальником, а далі ті самі знання про `SCU`, `GIC` і `VBAR` стали фундаментом для запуску **Linux (CPU0) + Zephyr (CPU1) + FPGA PL** у [другій частині](Zephyr-RTOS-on-PYNQ-Z1-AMP-OpenAMP-Part2-UA.md).

---

## Скорочення

- **AMP** (*Asymmetric Multiprocessing*) — асиметрична багатопроцесорність, коли на різних ядрах одного чипа працюють різні ОС.
- **AXI** (*Advanced eXtensible Interface*) — системна шина зв’язку між процесорною частиною PS і програмованою логікою FPGA (PL).
- **BDP** (*Bandwidth-Delay Product*) — добуток пропускної здатності каналу на затримку (RTT), що обмежує швидкість TCP при фіксованому розмірі вікна.
- **BRAM** (*Block RAM*) — вбудована блочна пам’ять усередині FPGA.
- **DAQ** (*Data Acquisition*) — високошвидкісний збір і оцифрування даних із сенсорів/АЦП.
- **DDR3** — зовнішня динамічна оперативна пам’ять (на PYNQ-Z1 — 512 МБ, 32-бітна шина).
- **DevCfg** (*Device Configuration Interface*) — апаратний контролер Zynq-7000 для прошивки бітстріму в FPGA PL через PCAP.
- **DMA** (*Direct Memory Access*) — контролер прямого доступу до пам’яті в обхід CPU.
- **DSP** (*Digital Signal Processing*) — апаратні блоки множення-накопичення у FPGA.
- **DTS** (*Device Tree Source*) — текстовий опис апаратної конфігурації плати.
- **ELF** (*Executable and Linkable Format*) — формат виконуваного файлу з метаданими секцій і точкою входу.
- **EMIO** (*Extended Multiplexed I/O*) — розширені лінії вводу-виводу PS, маршрутизовані через піни FPGA PL.
- **FPGA** (*Field-Programmable Gate Array*) — програмована логічна інтегральна схема.
- **FSBL** (*First Stage Boot Loader*) — початковий завантажувач Zynq, що ініціалізує DDR, PLL і MIO.
- **GEM** (*Gigabit Ethernet MAC*) — контролер гігабітного Ethernet у Zynq-7000.
- **GIC** (*Generic Interrupt Controller*) — контролер переривань ARM (у Zynq-7000 — GICv1).
- **GPIO** (*General-Purpose Input/Output*) — цифрові лінії вводу-виводу загального призначення.
- **GTC** (*Global Timer Counter*) — глобальний 64-бітний таймер Cortex-A9 MPCore.
- **I2C / IIC** (*Inter-Integrated Circuit*) — дводротова послідовна шина для підключення сенсорів і периферії.
- **IPC / IPI** — міжпроцесорна комунікація / міжпроцесорне переривання (*Inter-Processor Interrupt*).
- **MDIO** (*Management Data Input/Output*) — послідовна шина керування й діагностики фізичного рівня Ethernet (PHY).
- **MIO** (*Multiplexed I/O*) — мультиплексовані виводи процесорної частини Zynq.
- **MMU** (*Memory Management Unit*) — блок керування віртуальною пам’яттю.
- **OTG** (*On-The-Go*) — режим USB, що дозволяє контролеру працювати як хост або пристрій.
- **PL / PS** — програмована логіка (*Programmable Logic*) та процесорна система (*Processing System*) у Zynq.
- **Pmod** (*Peripheral Module*) — стандартний 6/12-піновий роз’єм Digilent для підключення зовнішніх периферійних модулів.
- **PWM** (*Pulse-Width Modulation*) — широтно-імпульсна модуляція.
- **QSPI** (*Quad Serial Peripheral Interface*) — 4-лінійний швидкісний інтерфейс підключення NOR Flash.
- **RTOS** (*Real-Time Operating System*) — операційна система реального часу.
- **SCU** (*Snoop Control Unit*) — блок апаратної когерентності L1-кешів між ядрами Cortex-A9.
- **SDHCI** (*Secure Digital Host Controller Interface*) — стандартний контролер SD/microSD-карт.
- **SDK** (*Software Development Kit*) — набір компіляторів та утиліт (тут — Zephyr SDK `1.0.1`).
- **SDR** (*Software-Defined Radio*) — програмно-визначуване радіо.
- **SGI** (*Software Generated Interrupt*) — програмне між’ядерне переривання GIC (`INTID 0..15`).
- **SMP** (*Symmetric Multiprocessing*) — симетрична багатопроцесорність, коли одна ОС планує потоки на всіх ядрах.
- **SoC** (*System on Chip*) — система на кристалі.
- **SPI** (*Serial Peripheral Interface*) — синхронна послідовна шина периферії.
- **TFTP** (*Trivial File Transfer Protocol*) — простий протокол передачі файлів по UDP, підтримуваний U-Boot.
- **TTC** (*Triple Timer Counter*) — апаратний триканальний таймер-лічильник у Zynq-7000 PS.
- **UART** (*Universal Asynchronous Receiver-Transmitter*) — послідовний інтерфейс консолі.
- **WDT** (*Watchdog Timer*) — апаратний сторожовий таймер, що перезавантажує систему при зависанні.
- **WFE / SEV** (*Wait For Event / Send Event*) — інструкції ARM для низькоенергетичного очікування та пробудження ядра.
- **XADC** (*Xilinx Analog-to-Digital Converter*) — вбудований 12-бітний АЦП у Zynq-7000 для контролю температури кристала, напруг живлення та аналогових входів.

---

## Посилання

- Популярна версія статті на DOU: [dou.ua/forums/topic/63691/](https://dou.ua/forums/topic/63691/)
- Частина 2 (повна технічна версія AMP/OpenAMP, Mainline Linux 7.3.6 `PREEMPT_RT`, Buildroot): [Zephyr-RTOS-on-PYNQ-Z1-AMP-OpenAMP-Part2-UA.md](Zephyr-RTOS-on-PYNQ-Z1-AMP-OpenAMP-Part2-UA.md)
- Репозиторій проєкту (out-of-tree порт `boards/digilent/pynq_z1/`, `setup_and_build.sh`, патчі `patches/`, `ROADMAP.md`): [github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1)
- Детальне технічне мемо по знайденому багу GICv1/v2 SGI у Zephyr 4.5.0: [docs/zephyr-gic-v1-v2-sgi-bug.md](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/blob/main/docs/zephyr-gic-v1-v2-sgi-bug.md)
- Upstream Zephyr issue та фікс на `main`: [#122600](https://github.com/zephyrproject-rtos/zephyr/issues/122600), [PR #123040](https://github.com/zephyrproject-rtos/zephyr/pull/123040)
- Офіційні образи Digilent PYNQ-Z1: [pynq.io](https://www.pynq.io/)
- Zephyr RTOS: [github.com/zephyrproject-rtos/zephyr](https://github.com/zephyrproject-rtos/zephyr)
- Zephyr SDK (`sdk-ng`): [github.com/zephyrproject-rtos/sdk-ng](https://github.com/zephyrproject-rtos/sdk-ng)
