# 偽 RAMFILE for MZ-1500

## これはなに？

MZ-1500 用拡張モジュールです。
本体の拡張スロットまたは、RAMFILE 用スロットに装着します。

以下の機能があります。

- MZ-1R18 RAMFILE
- MZ-1R23 漢字 ROM
- MZ-1R24 辞書 ROM
- MZ-1R12 SRAM メモリ
- PIO-3034 EMM
- PCG-700

---
## 回路

![board](/pictures/board.jpg)

Z80 Bus に直結するために、
ピンの多い RP2350B のボード(WeAct RP2350B)を使用します。
カートリッジスロットの信号を、単純に Raspberry Pi Pico2 に接続しただけです。
Pico2 が 5V 耐性なのを良いことに直結しています。

```
GP0-7:  D0-7
GP8-23: A0-15
GP25: RESET
GP28: MERQ
GP29: IORQ
GP30: WR
GP31: RD
GP32: EXWAIT
VBUS: 5V
GND: GND
```

PCG-700 のエミュレーションのためには VGA 出力が必要です。
GP33-37 を接続してください。

![Schematics](/pictures/schematics.png)

---
## MZ-1R18 RAMFILE

![RAMFILE test](/pictures/screenshot01.jpg)

64KB の RAM FILE のエミュレーションをします。
BASIC から INIT "RAM:" などで使用することができます。

---
## MZ-1R23 漢字ROM

![KANJI test](/pictures/screenshot02.jpg)

漢字ROM のエミュレートをします。
あらかじめ picotool などで漢字 ROM のデータを書き込んでおく必要があります。

```
picotool.exe load -v -x kanji.rom  -t bin -o 0x10020000
```

---
## MZ-1R24 辞書ROM

同じく辞書ROM のエミュレートをします。
あらかじめ picotool などで辞書 ROM のデータを書き込んでおく必要があります。

![JISHO test](/pictures/screenshot05.jpg)

```
picotool.exe load -v -x dict.rom  -t bin -o 0x10040000
```

---
## MZ-1R12 SRAM メモリ

32KB の SRAM メモリのエミュレーションをします。
電源投入時は Checksum Error で止まるようにしています。

フラッシュとのやり取りは、I/O ポートの $BA,$BB を通して行えます。
内部的には 0-63 のバックアップスロットがあって、リード・ライトを制御できます。

$BA に出力するとその番号のスロットから SRAM に読み込みます。

```
OUT@ $BA,1
```

このあとリセットすると、SRAM から起動できます。

$BB に出力すると、SRAM の内容をその番号のスロットに書き込みます。

```
OUT@ $BB,2
```

なお、書き込み中は VGA 出力が停止します。

スロットに書き込みを行っていないデータは電源を切ると消滅します。

基本的な使い方は、モニタの ES コマンドでテープのソフトを SRAM に保存したのち、BASIC 上で上記コマンドで書き込みを行うことになるでしょう。

なお、エミュレータなどで作成したデータを記録することもできます。
アドレス 0x10080000 から 32KiB 単位でおいてください。
最初(0番)のデータは以下のように書き込みます。

```
picotool.exe load -v -x MZboot.rom  -t bin -o 0x10080000
```

起動できるデータの作成を PC 上でするには、
yanataka60さんの[MZ-1500SD 付属のツール 1Z-1R12_Header](https://github.com/yanataka60/MZ-1500_SD)
を使用すると便利です。

---
## PIO-3034 EMM

![EMM test](/pictures/screenshot03.jpg)

320KB の EMM をエミュレートします。
Hu-BASIC などで使用することができます。

こちらも I/O ポートの $BC,$BD で読み書きの制御が可能です。
内部的には 0-31 の EMM スロットがあって、そのうちの一つを EMM にロードする形になります。

$BC に出力するとその番号のスロットから EMM に読み込みます。

```
OUT@ $BC,1
```

$BD に出力すると、EMM の内容をその番号のスロットに書き込みます。

```
OUT@ $BD,2
```

なお、書き込み中は VGA 出力が停止します。

スロットに書き込みを行っていないデータは電源を切ると消滅します。

---
## VGA OUT & PCG-700

![PCG700 test](/pictures/screenshot04.jpg)

VGA 出力と PCG-700 のエミュレーションをします。
本体出力との同期をとっていませんので、ソフトウェアによっては正しく描画されないことがあります。

本来 PCG-700 は、バンク切り替えに関係なく反応しますが、
MZ-1500 用ソフトの動作に支障があるために、VRAM 選択時にのみ動作します。

なお、あらかじめ本体の CG-ROM の内容をフラッシュに書き込んでおく必要があります。

```
picotool.exe load -v -x font.rom  -t bin -o 0x1001f000
```

---
## 使用ライブラリなど

- [VGA ライブラリ(一部改変)](https://github.com/vha3/Hunter-Adams-RP2040-Demos/tree/master/VGA_Graphics)