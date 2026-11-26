<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# titan2-boot

[English version](README.md)

Меню загрузки для Unihertz Titan 2 (MT6878): Android или mainline Linux. Это вторая ступень после
заводского LK: aarch64, без ОС. LK не заменяется и не перепрошивается. Он загружает этот образ из
`boot_a` так же, как ядро: распаковывает по адресу 0x40000000 и передаёт управление на EL1, а в x0
лежит FDT, который он уже собрал. Как заводской LK загружает образ, описано в titan2-docs:
`docs/lk-boot-requirements.md`.

Работает на телефоне (2026-10-24): меню, Android, recovery и зарядка выключенного телефона,
mainline из пачки и чтение UFS с пунктами раздела `linux`.

## Как устроено

Коротко (подробности — в английском README): в пачке именованные файлы и `extlinux.conf` (синтаксис
U-Boot, `src/conf.c`) с пунктами меню; при `UFS on` в меню появляется пункт «Linux partition»:
только когда его выбрали (или он пункт по умолчанию, `DEFAULT ufs`), titan2-boot поднимает UFS
только на чтение (`src/ufs.c`) и показывает пункты `/boot/extlinux/extlinux.conf` раздела `linux`
(GPT, ext4). Ядро Android остаётся в пачке: recovery, зарядка и обычная загрузка Android от
чтения UFS не зависят.
Ядра — raw, gzip или lz4; распаковываются в свободную по карте LK память, MMU включён, всё
зарезервированное LK отображено как Device. Пункты: `LINUX`/`FDT`/`INITRD`/`APPEND` — mainline,
`ANDROID <файл>` или `ANDROID part:boot_b` — Android.

Ниже — описание этапа 1 (пачка), оно по-прежнему верно для пунктов из пачки.

`boot_a` = titan2-boot + «пачка» (`src/pack.h`, `tools/mkpack.py`): ядро Android, ядро mainline,
его DTB, initramfs и cmdline. LK распаковывает всё одним куском, поэтому ядра запускаются на месте и
ничего не копируется. `vendor_boot_a`, `init_boot_a`, `dtbo_a` остаются от Android, значит LK
собирает заводской FDT и грузит прошивки (SCP, модем и др.) так же, как для Android.

- **Android:** ядро получает FDT и рамдиск от LK без изменений.
- **Linux:** берётся DTB mainline из пачки, и в него переносится то, что LK узнал при загрузке
  (`src/fdtfix.c`): `/memory`, все узлы `/reserved-memory/mblock-*` (у фреймбуфера ставится
  `no-map`), записи LK в `/chosen` (`atag,*`, `kaslr-seed`, …), свойства, которые хуки LK пишут
  в узлы модема, и узлы `/mdproperties`, `/md_attr_node`. Узел DTB может запросить phandle LK в
  `/reserved-memory` (`titan2,lk-refs`) и поменять регионы, на которые они указывают
  (`titan2,lk-region-compatible`, `titan2,lk-region-map` снимает `no-map`) — так пул Wi-Fi
  становится restricted DMA pool без нового boot_a. bootargs собираются как у LK:
  свои bootargs DTB, затем earlycon, console и ramoops от LK, затем cmdline из пачки.
- **Fastboot / Recovery / Reboot:** через регистр reboot-mode (`TOPRGU+0x24`) и PSCI-сброс.
- Если LK загрузился не в обычном режиме (recovery, зарядка в выключенном состоянии, будильник,
  meta), меню не показывается и сразу запускается Android: эти режимы обрабатывает он.

Меню рисуется во фреймбуфере LK (шрифт Terminus 16x32). Панель работает в командном режиме DSI, поэтому
после отрисовки запускается один кадр через конвейер LK: mutex 0, OVL0, DSI0. Последовательность
та же, что в драйвере mainline `mtk_cmdfb_kick.c`, который проверен на железе. Перед переходом в
ядро конвейер возвращается в то состояние, в котором его оставил LK. Управление: громкость вверх и вниз
двигает выделение, питание запускает пункт. Громкость — это GPIO54/55 (настраиваются как вход с
подтяжкой, как в Android). Питание читается из PMIC MT6363 по SPMI (программный канал 2 PMIF,
`TOPSTATUS`). Если PMIC не отвечает, пункт запускает громкость вниз. Пока не нажата ни одна
клавиша, идёт обратный отсчёт с полосой до пункта по умолчанию.

## Сборка

```
make TOOLCHAIN=/путь/к/clang-21           # out/titan2-boot.bin (без пачки)
make test LK_FDT=<FDT LK> DTB=<DTB mainline>   # перенос данных LK на ПК (cc, ASan/UBSan)
make qemu                                 # тестовая сборка для qemu virt (out-qemu/)
```

Компилятор — clang 21: AOSP clang r574158 или официальный выпуск LLVM 21.1.8 (`TOOLCHAIN=` или
`CLANG_DIR=`, каталог с `bin/clang`). `ld.lld` из выпуска LLVM требует на ПК `libxml2.so.2`;
`scripts/make-bootmenu.sh` из titan2-tools запускает эту сборку в своём контейнере, где эта
библиотека есть. Смена компилятора или флагов пересобирает всё (`out/build.id`). Сборка идёт с `-mstrict-align` (код работает с
выключенным MMU, пока не построена карта памяти). `boot.img` собирают скрипты titan2-tools:
`scripts/make-bootmenu.sh` (с `tools/mkpack.py` отсюда) и проверка в qemu
`scripts/test-bootmenu-qemu.sh`. Установка Android и Linux рядом — titan2-docs, `docs/DUALBOOT.md`.

Проверено на телефоне (2026-10-24): меню, клавиши, Android (и после работы с UFS), recovery и
зарядка выключенного телефона (без меню, сразу Android), mainline из пачки, MMU, распаковка
ядер gzip/lz4 и чтение UFS (HS-G4, GPT, ext4) с пунктами раздела `linux`. На телефоне чтение
UFS с самого начала пишет свой лог в pstore Android, так что после зависания и сброса его можно
прочитать в `/sys/fs/pstore/console-ramoops-0`.

## Как делается проект и с кем он связан

- Проект разрабатывается с помощью ИИ-ассистентов (Anthropic Claude). В коммитах стоит строка
  `Co-Authored-By: Claude`; каждое изменение проверяет и испытывает на устройстве человек —
  мейнтейнер (serlk), он же за него отвечает.
- Проект **не связан с postmarketOS** и MT6878-Mainline. Мы опираемся на их GPL-работу над ядром
  (спасибо!), но postmarketOS не принимает вклад, сделанный с помощью ИИ, поэтому отсюда ничего
  не отправляется в postmarketOS, и это не порт postmarketOS.

## Источники

Сведения о железе (адреса, значения регистров, состояние, в котором заводской
загрузчик оставляет железо) получены с самого телефона: чтение регистров на
устройстве, заводское дерево устройств и журналы загрузки, собственные модули
ядра телефона, объявленные под GPL, и анализ поведения заводского загрузчика
(его код не воспроизводится); для сравнения — публичные исходники MediaTek под
GPL и upstream Linux. Чтение UFS написано методом чистой комнаты: по
`spec/ufs-mt6878.md`, описанию из одних фактов, разработчиком, не видевшим
анализа загрузчика. Проприетарного кода, прошивок и бинарных файлов здесь нет.

## Связанные репозитории

- [titan2-linux](https://github.com/Serlk/titan2-linux): ядро, ветка `titan2` (порт на mainline, который загружает это меню).
- [titan2-tools](https://github.com/Serlk/titan2-tools): сборка образов (`scripts/make-bootmenu.sh` упаковывает это меню), initramfs, сетевой и дисковый корень, службы.
- [titan2-docs](https://github.com/Serlk/titan2-docs): документация, в том числе инструкция по двойной загрузке `docs/DUALBOOT.md`.
- [titan2-camera](https://github.com/Serlk/titan2-camera), [titan2-fingerprint](https://github.com/Serlk/titan2-fingerprint), [titan2-gnss](https://github.com/Serlk/titan2-gnss): необязательные части для камер, отпечатка и GNSS.
- [titan2-android](https://github.com/Serlk/titan2-android): сборки Android для Titan 2.

## Лицензия

Copyright (c) 2026 serlk. GPL-2.0-or-later, см. [LICENSE](LICENSE). `src/font.h` сгенерирован из
шрифта ядра Linux `lib/fonts/font_ter16x32.c` (GPL-2.0), поэтому собранный бинарник
распространяется под GPL-2.0-only. `libfdt/` взята из ядра Linux без изменений
(GPL-2.0-or-later OR BSD-2-Clause). `src/inflate.c` — изменённая версия puff.c из zlib (Zlib, см.
файл). Кода производителя нет: UFS и дисплей написаны по нашему описанию поведения железа.

Проект не связан с Unihertz и MediaTek.

Лицензии: [LICENSE](LICENSE) (GPL-2.0), подробности по файлам — в [LICENSING.md](LICENSING.md).
