# 偽 RAMFILE for MZ-1500

# ** WORK IN PROGRESS **

開発中のため未実装・未検証の機能があります。

![nisesenshi on 6001mk2SR](/pictures/board2.jpg)

## これはなに？

MZ-1500 用拡張モジュールです。
本体の拡張スロットまたは、RAMFILE 用スロットに装着します。

以下の機能があります。

- MZ-1R18 RAMFILE
- MZ-1R23 漢字 ROM
- MZ-1R24 辞書 ROM (未検証)
- MZ-1R12 SRAM メモリもどき(バックアップなし)

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
GP29: IORQ
GP30: WR
GP31: RD
GP32: EXWAIT
VBUS: 5V
GND: GND
```

---
## MZ-1R18 RAMFILE

64KB の RAM FILE のエミュレーションをします。
BASIC から INIT "RAM:" などで使用することができます。

---
## MZ-1R23 漢字ROM

漢字ROM のエミュレートをします。
あらかじめ picotool などで漢字 ROM のデータを書き込んでおく必要があります。

```
picotool.exe load -v -x kanji.rom  -t bin -o 0x10020000
```

---
## MZ-1R24 辞書ROM

同じく辞書ROM のエミュレートをします。
あらかじめ picotool などで辞書 ROM のデータを書き込んでおく必要があります。

```
picotool.exe load -v -x dict.rom  -t bin -o 0x10040000
```

---
## MZ-1R12 SRAM メモリ

64KB の SRAM メモリのエミュレーションをします。
電源投入時は Checksum Error で止まるようにしています。

BASIC から $FB に出力すると、ROM 上のデータを SRAM 上にロードできます。
出力する値によってロードするデータを変えることができます。(最大64個)

ROM のデータはあらかじめフラッシュに書き込んでおく必要があります。
アドレス 0x10080000 から 64KiB 単位でおいてください。
最初(0番)の ROM 以下のように書き込みます。

```
picotool.exe load -v -x MZboot.rom  -t bin -o 0x10080000
```

切り替えたのちにリセットボタンを押すと ROM から起動します。

起動できるデータの作成には、
yanataka60さんの[MZ-1500SD 付属のツール 1Z-1R12_Header](https://github.com/yanataka60/MZ-1500_SD)
を使用すると便利です。