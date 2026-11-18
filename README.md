<!-- PROJECT LOGO -->
<div align="center">
  <img src="doc/images/kosvn_logo.png" alt="KallistiOS-VisualNovel Logo" width="180" />
  <h1 align="center"><strong>KallistiOS-VisualNovel (KallistiOS-VN)</strong></h1>
  <p align="center">
    <strong>KallistiOS ノベルゲーム特化版</strong> — セガ・ドリームキャスト（Dreamcast）向け ノベルゲーム専用 OS &amp; C言語エンジン層 (<code>libkosvn</code>)
    <br />
    <em>Visual Novel Specialized OS &amp; C Engine Runtime for the Sega Dreamcast (Forked from KallistiOS)</em>
  </p>
</div>

---

## 目次

1. [概要と用途（なぜ専用 OS なのか）](#1-概要と用途なぜ専用-os-なのか)
2. [純正 KallistiOS からの変更点](#2-純正-kallistios-からの変更点)
3. [ディレクトリ構成](#3-ディレクトリ構成)
4. [ビルド方法（環境構築〜サンプル生成）](#4-ビルド方法環境構築サンプル生成)
5. [使い方：C言語アプリの作成手順 (`main.c` / `Makefile`)](#5-使い方c言語アプリの作成手順-mainc--makefile)
6. [使い方：シナリオスクリプト (`scenario.txt`) の書き方](#6-使い方シナリオスクリプト-scenariotxt-の書き方)
7. [画像・サウンドアセット仕様](#7-画像サウンドアセット仕様)
8. [実機／エミュレータでのコントローラ操作一覧](#8-実機エミュレータでのコントローラ操作一覧)
9. [LAN デバッグ &amp; UDP テレメトリの使い方](#9-lan-デバッグ--udp-テレメトリの使い方)
10. [主要 C API リファレンス (`kosvn.h`)](#10-主要-c-api-リファレンス-kosvnh)
11. [Upstream KallistiOS Overview &amp; License（本家概要・ライセンス 和文併記）](#upstream-kallistios-overview--license)

---

## 1. 概要と用途（なぜ専用 OS なのか）

**KallistiOS-VN** は、セガ・ドリームキャスト向けオープンソース OS / SDK である **KallistiOS (KOS)** をベースに、**ビジュアルノベル／アドベンチャーゲーム（ADV）の開発・動作に最適化して軽量化・機能特化したカスタムフォーク（ブランチ: `vn-engine`）**です。

汎用ゲーム機 SDK から 3D 行列演算や特殊周辺機器ドライバを切り離しつつ、ノベルゲーム制作に必要な全機能（PowerVR2 2D合成・BIOS フォントによる Shift-JIS 日本語描画・シナリオ VM・VMU セーブ/ロード・AICA サウンド・LAN 経由リアルタイムデバッグ）を C言語アドオンライブラリ **`libkosvn`** として統合しています。

### 主な想定用途
* **『EVE』シリーズ型マルチサイト／コマンド総当たり探偵 ADV の開発**
  複数主人公の視点ザッピング（`SITE_INIT` / `SITE_SWITCH` / `L+X`）、フラグ条件付きコマンド出現（`OPTION_IF`）、調査サブルーチン（`CALL` / `RET` / `@call:`）、証拠品カットイン（`CUTIN`）、台詞送り自動停止ボイス（`VOICE`）を標準サポートしています。
* **ドリームキャスト向けオリジナル／同人ノベルゲームのスクラッチ開発**
  スクリプトファイル（`/rd/scenario.txt`）を書くだけで、テキスト送り・選択肢分岐・立ち絵管理・画面演出・VMU セーブ（全サイト状態＋256フラグ一括保存）まで一式が即動作します。
* **C言語による高速・省メモリな専用ゲーム基盤**
  `kosvn_demo.elf` 構成でも静的フットプリントは約 515 KB（16 MB RAM 中、約 15 MB 以上をリソースやバッファに活用可能）に収まります。さらにタイプライター描画の差分行（Dirty Row）部分 VRAM 転送・PVR VRAM LRU テクスチャキャッシュによる 0ms サイトザッピング・PowerVR2 ポリゴンヘッダ事前コンパイル・FNV-1a ラベルハッシュ検索・SH-4 `-O3` 最適化により、テキスト送りと視点切替を高速化しています。
* **LAN (`dcload-ip` + UDP) を活用した実機快適デバッグ**
  実機やエミュレータと PC を LAN 接続し、ゲームを動かしながら FPS・シナリオ現在行・空き RAM/VRAM を PC 側へリアルタイム送信して監視できます。
* **プロシージャル素材による即座のプロトタイピング**
  画像・BGM ファイルがまだ揃っていない制作初期段階でも、`proc:` 指定の組み込み背景・立ち絵・サウンドを使ってすぐにシナリオと演出テストを始められます。

---

## 2. 純正 KallistiOS からの変更点

ノベルゲームの制作・デバッグに不要なハードウェア負荷やバイナリ肥大を取り除き、通信デバッグと標準周辺機器だけを残しています。

### 残した機能（ノベル制作＋LAN デバッグ必須機能）
* **ネットワーク・デバッグ関連:** `network`（BBA / LAN Adapter / W5500）、`net`（IPv4/UDP/TCP スタック）、`dcload-ip` / `dcload-serial`
* **入力・記録デバイス:** 標準コントローラ (`controller`)、ビジュアルメモリ (`vmu` / `vmufs` / `vmu_pkg`)
* **描画・音声・媒体:** PowerVR2 (`pvr`)、BIOS フォント (`biosfont`)、AICA サウンド (`sound` / `sfxmgr` / `snd_stream`)、CD-ROM (`cdrom`)、ROMFS (`romdisk`)
* **専用初期化フラグ追加:** `kernel/arch/dreamcast/include/arch/init_flags.h` に **`INIT_VN_DEFAULT`**（`= INIT_DEFAULT | INIT_NET`）を定義

### 切り離した機能（不要ドライバ・複雑な 3D 演算）
* **モデム関連:** `kernel/arch/dreamcast/hardware/modem` および `addons/libppp` 内の `ppp_modem.o`
* **特殊 Maple 周辺機器:** キーボード (`keyboard`)、マウス (`mouse`)、ガンコン (`lightgun`)、ぷるぷるパック (`purupuru`)、マイク (`sip`)、ドリームアイ (`dreameye`)
* **3D 座標・行列演算:** `kernel/arch/dreamcast/math/matrix3d.c`（`mat_translate`, `mat_scale`, `mat_rotate_x/y/z`, `mat_rotate`, `mat_perspective`, `mat_lookat` および `exports.txt` の `kbd_*` / `mat_*` エクスポート）
* **サイズ削減効果:** `libkallisti.a` が純正 `4,480,822` bytes → カスタム版 `4,187,802` bytes（**約 293 KB 削減**）

### `libkosvn` エンジンの高速化・安定化機構
* **差分行（Dirty-Row）部分 VRAM 転送 & 増分タイプライター描画 (`kosvn_text.c`):** `1024x512` テキストキャンバス（約 1 MB）の毎フレーム全再描画・全転送を廃止し、新規文字のみを描画して変更スキャンライン（1 行分 約 56 KB）だけを PVR VRAM へ転送（枠溢れクリップ時の再描画ループ抑止ガード付き）。
* **PVR VRAM 16スロット LRU テクスチャキャッシュ (`kosvn_gfx.c`):** 背景・立ち絵・カットインを VRAM 上で LRU キャッシュし、`L+X` マルチサイト切替時の再ロード／プロシージャル再生成を 0ms 化。
* **PowerVR2 ポリゴンヘッダ 16スロット事前コンパイルキャッシュ (`kosvn_gfx.c`):** 単色ポリゴンヘッダ（OP/TR）およびテクスチャポリゴンヘッダ（`hdr_cache[16]`、32 KB アライメント衝突を回避する線形一致走査＋ラウンドロビン更新）により毎フレームの `pvr_poly_compile()` 負荷を削減。
* **FNV-1a 32bit ラベルハッシュ & 高速条件式評価 (`kosvn_script.c`):** スクリプト読込時に全 `LABEL`（最大 128 個）の FNV-1a ハッシュを計算し、`GOTO` / `CALL` / `@call:` ジャンプおよび `IF` / `IF_AND` / `OPTION_IF` の比較演算を高速化。
* **マルチサイト台詞保持・`CHOICE` 復帰保護・VMU `KVN2` フォーマット (`kosvn_script.c` / `kosvn_vmu.c`):** サイト切替時や `CHOICE` 行でのクイックロード時に直前の台詞テキスト・コールスタック・全 256 フラグ・カットイン状態を完全復元。

---

## 3. ディレクトリ構成

本カスタムフォークで追加・変更された主なファイルは以下の通りです。

```text
KallistiOS-VN/
├── kernel/arch/dreamcast/
│   ├── include/arch/init_flags.h          # INIT_VN_DEFAULT 追加・不要ドライバフラグ整理
│   ├── hardware/Makefile                  # modem サブディレクトリ除外（network は維持）
│   ├── hardware/maple/Makefile            # controller.o / vmu.o のみに削減
│   ├── hardware/maple/maple_init_shutdown.c
│   ├── math/Makefile                      # matrix3d.o 除外
│   └── exports.txt                        # kbd_* / matrix3d シンボル削除
├── addons/
│   ├── include/kosvn/kosvn.h              # ノベルエンジン公開 C API ヘッダ
│   └── libkosvn/                          # 専用ノベルエンジン実装
│       ├── Makefile
│       ├── kos/dreamcast.cnf
│       ├── kosvn_internal.h
│       ├── kosvn_core.c                   # メインループ・入力制御・AICA BGM/SE
│       ├── kosvn_gfx.c                    # PowerVR2 2D合成・立ち絵・フェード/シェイク
│       ├── kosvn_text.c                   # BIOSフォント Shift-JIS描画・折り返し・履歴
│       ├── kosvn_script.c                 # 行ベースシナリオパーサ & VM
│       ├── kosvn_vmu.c                    # VMU パッケージセーブ/ロード & LCDロゴ
│       └── kosvn_netdbg.c                 # UDP LANテレメトリ & dbglog 出力
└── examples/dreamcast/kosvn_demo/         # 動作確認用サンプルゲーム
    ├── Makefile
    ├── main.c
    └── romdisk/scenario.txt               # Shift-JIS シナリオデータ
```

---

## 4. ビルド方法（環境構築〜サンプル生成）

Windows の **DreamSDK**（MSYS2 + `sh-elf-gcc` 13.2.0）環境を前提としています。Linux / WSL 環境でも同様にビルド可能です。

### ステップ 1: `environ.sh` の用意
本リポジトリ直下の `environ.sh`（`.gitignore` 対象）で `KOS_BASE` を本リポジトリのパスに合わせて設定します。

```bash
cp doc/environ.sh.sample environ.sh   # または DreamSDK の kos/environ.sh をコピー
# environ.sh 内の KOS_BASE を本リポジトリの POSIX パスに変更
# 例: export KOS_BASE="/h/EveGE/KallistiOS-VN"
```

### ステップ 2: ホストツール・カーネル・`libkosvn.a` のビルド
MSYS2 シェル（DreamSDK Shell）で以下を実行します。

```bash
source /h/EveGE/KallistiOS-VN/environ.sh

# ROMFS 作成・C変換に必要なホストツールをビルド
make -C $KOS_BASE/utils/genromfs
make -C $KOS_BASE/utils/bin2c

# 軽量化されたカーネル (libkallisti.a) とアドオン (libkosvn.a を含む) をビルド
make -C $KOS_BASE/kernel
make -C $KOS_BASE/addons
```

ビルドが成功すると、`lib/dreamcast/libkallisti.a` および `addons/lib/dreamcast/libkosvn.a` が生成されます。

### ステップ 3: サンプルゲーム (`kosvn_demo.elf` / `kosvn_demo.cdi`) のビルド
```bash
# SH-4 ELF バイナリのビルド (dcload-ip / エミュレータ直接起動用)
make -C $KOS_BASE/examples/dreamcast/kosvn_demo clean all

# 実機・エミュレータ用セルフブート CDI ディスクイメージの生成 (mkdcdisc 使用)
make -C $KOS_BASE/examples/dreamcast/kosvn_demo cdi
```

`examples/dreamcast/kosvn_demo/kosvn_demo.elf`（エントリーポイント `0x8c010000` の SH-4 ELF）およびブート可能な `kosvn_demo.cdi` が生成されます。`dcload-ip` やエミュレータ（Flycast 等）、実機 CD-R でそのまま起動できます。

---

## 5. 使い方：C言語アプリの作成手順 (`main.c` / `Makefile`)

### 最小構成の `main.c`

```c
#include <kos.h>
#include <kosvn/kosvn.h>

/* ネットワークデバッグ有効 + 不要ドライバ除外済みの専用初期化フラグ */
KOS_INIT_FLAGS(INIT_VN_DEFAULT);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    vn_config_t cfg = {
        .title           = "My Dreamcast Visual Novel",
        .app_id          = "MY_VN_APP",        /* VMU保存時のアプリケーションID */
        .net_debug_port  = 47890,              /* 0 にすると UDP 送信を無効化 */
        .net_debug_ip    = "255.255.255.255",  /* ブロードキャスト宛先または PC の IP */
        .text_speed_ms   = 24,                 /* 文字送り速度 (ms/文字) */
        .auto_advance_ms = 1800,               /* オートモード待機時間 (ms) */
        /* 以下は省略可 */
        .title_script    = "/rd/title.txt",    /* START メニューの「タイトルへ戻る」先（NULL なら START で終了） */
        .quiet_status    = true,               /* 下端の操作説明を出さない（通知と SKIP/AUTO だけ） */
        .autoplay        = false,              /* テスト用: 文字を飛ばし、選択肢を無作為に選ぶ */
        .autoplay_switch_every = 0,            /* autoplay で n 回選ぶごとに STORY を切り替える */
        .title_return_label = NULL,            /* 「タイトルへ戻る」で飛ぶタイトル台本のラベル（ムービーを飛ばすなど） */
        .se_cursor = NULL, .se_decide = NULL,  /* メニューの効果音（カーソル・決定・取り消し）。NULL なら内蔵音 */
        .se_cancel = NULL
    };

    if(vn_init(&cfg) < 0)
        return -1;

    /* コントローラに挿した VMU 液晶画面へ 48x32 ロゴを表示 */
    vn_vmu_draw_logo("MYVN");

    /* /rd (romdisk) または /cd 上のシナリオを実行 */
    vn_run_script("/rd/scenario.txt");

    vn_shutdown();
    return 0;
}
```

### `Makefile` の記述例

```makefile
TARGET = my_vn_game.elf
OBJS = main.o romdisk.o
KOS_ROMDISK_DIR = romdisk
KOS_CFLAGS += -I$(KOS_BASE)/addons/include -O3 -fomit-frame-pointer -ffast-math -Wall -Wextra

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGET) romdisk.*

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -L$(KOS_BASE)/addons/lib/$(KOS_ARCH) -lkosvn
```

---

## 6. 使い方：シナリオスクリプト (`scenario.txt`) の書き方

`romdisk/scenario.txt` に 1 行 1 命令のテキスト形式でシナリオを記述します。

### ファイル作成上の注意
* **文字コード:** Dreamcast 本体の BIOS フォントが直接描画できるよう、**Shift-JIS (CP932)** で保存してください。
* **改行コード:** LF (`\n`) または CRLF (`\r\n`) のどちらでも読み込めます。
* **テキスト内の明示改行:** `TEXT` コマンド内で `\n` と書くと、その位置で改行されます（文字幅に基づく自動折り返しにも対応しています。行頭に来てはいけない「、。」」）…ー」や小さい「っゃ」などは、2 文字まで行末にはみ出させて前の行に残します＝禁則処理）。
* **コメント:** 行頭が `#` の行および空行は無視されます。

### コマンド一覧

| コマンド | 構文・引数 | 動作説明 |
| --- | --- | --- |
| `BG` | `BG <アセット指定>` | 背景を切り替えます。`none` で背景消去。`proc:...` または RAW16 ファイルパスを指定（512x512、または 153,600 バイトの 320x240 = PS の画面そのまま。後者は 512x256 のテクスチャに入れて 2 倍で描きます）。`BG <パス> <ミリ秒>` で前の背景からクロスフェードします（スキップ中は即切り替え。待つときは続けて `WAIT_FX`）。 |
| `CHAR` | `CHAR <left\|center\|right> <アセット指定>` | 立ち絵を左・中央・右スロットへフェードイン表示します（最大 3 人同時）。 |
| `CLEAR_CHAR` | `CLEAR_CHAR [left\|center\|right\|all]` | 指定スロットまたは全スロット（`all`）の立ち絵を消去します。 |
| `NAME` | `NAME <話者名>` | メッセージウィンドウ上の話者名バッジを設定します。`none` で非表示。 |
| `TEXT` | `TEXT <本文>` | 本文をタイプライター表示し、プレイヤーの決定入力（または Auto/Skip）を待ちます。 |
| `CHOICE` | `CHOICE` | 続く `OPTION` / `OPTION_IF` 行をまとめて選択肢・捜査コマンドカードとして表示し、選択を待ちます（最大 6 つ）。 |
| `OPTION` | `OPTION <表示テキスト> \| <ジャンプ先>` | `CHOICE` の直下に記述します。ジャンプ先には通常の `LABEL` に加え、`@call:<LABEL>`（調査サブルーチン呼び出し）や `@site:<0\|1>`（視点切替）を指定できます。 |
| `OPTION_IF` | `OPTION_IF <フラグ番号> <演算子> <値> <表示テキスト> \| <ジャンプ先>` | 条件を満たしたときだけメニューに表示されるコマンド／選択肢を定義します（演算子は `==`, `!=`, `>=`, `<=`, `>`, `<`）。 |
| `SITE_INIT` | `SITE_INIT <ID(0\|1)> <表示名> <開始LABEL>` | マルチサイト（Multi-Sight）機能を有効化し、各主人公（例: `0 小次郎`, `1 まりな`）の開始ラベルと右上HUD表示名を登録します。 |
| `SITE_SWITCH` | `SITE_SWITCH <ID(0\|1)>` | 現在のサイトの行位置・表示中台詞（`site_dialogues`）・コールスタック・背景・立ち絵・カットイン・BGM を保存し、指定サイトの実行状態へ即座に切り替えます。 |
| `SITE_LOCK` | `SITE_LOCK [ID] [0\|1]` | 指定サイト（省略時は現在サイト）のロック状態を設定します。ロック中（`1`）はプレイヤーによる `L+X` ザッピングおよび `@site:` 選択肢の遷移が抑止され、`[SITE LOCKED]` 通知を表示します。 |
| `LABEL` | `LABEL <ラベル名>` | `GOTO` / `CALL` / `OPTION` / `IF` 等のジャンプ先ラベルを定義します（最大 128 個）。 |
| `GOTO` | `GOTO <ラベル名>` | 指定した `LABEL` へ無条件ジャンプします。 |
| `CALL` | `CALL <ラベル名>` | 現在位置をコールスタック（最大 8 段）に積んでサブルーチンラベルへジャンプします。`CHOICE` 内の `@call:<LABEL>` でも利用可能。 |
| `RET` | `RET` | `CALL` / `@call:` 呼び出し元（捜査完了後にコマンドメニューへ復帰等）へ戻ります。 |
| `SET` | `SET <フラグ番号(0..255)> <整数値>` | ゲーム内フラグ変数（256 個分・VMU セーブ対象）に整数値を代入します。 |
| `ADD` | `ADD <フラグ番号(0..255)> <加算値>` | 指定フラグへ整数値（正負）を加算します（総当たり達成度カウント等に利用）。 |
| `IF` | `IF <番号> <比較演算子> <値> GOTO <ラベル名>` | 単一条件成立時にジャンプします。演算子は `==`, `!=`, `>=`, `<=`, `>`, `<` に対応。 |
| `IF_AND` | `IF_AND <番号1> <演算子1> <値1> <番号2> <演算子2> <値2> GOTO <ラベル名>` | 2つのフラグ条件を同時に満たしたときだけジャンプします（複数サイト合流判定用）。 |
| `BIT` | `BIT <ビット番号(0..4607)> <0\|1>` | 1 ビットのフラグ（4,608 個・VMU セーブ対象）を立てる／消します。整数フラグとは別の領域です。 |
| `BIT_RANGE` | `BIT_RANGE <開始> <終了> <0\|1>` | 開始〜終了（両端を含む）のビットをまとめて設定します。 |
| `IF_BIT` | `IF_BIT <ビット番号> <0\|1> GOTO <ラベル名>` | ビットが指定値と等しいときジャンプします。 |
| `OPTION_BIT` | `OPTION_BIT <ビット番号> <0\|1> <表示テキスト> \| <ジャンプ先>` | ビットが指定値と等しいときだけ表示される選択肢です（`CHOICE` の直下）。 |
| `WAIT` | `WAIT <時間ms>` | 指定時間スクリプトを止めます（`A` またはスキップで即解除可）。 |
| `OVERLAY` | `OVERLAY <枠0..5> <RAW16> <テクスチャ幅> <高さ> <コマ幅> <コマ高さ> <コマ間隔> <コマ数> <x> <y> <幅> <高さ> <blink\|lip\|lip<N>\|still>` | 背景の上に重ねる差分（目パチ・口パク）を登録します。ARGB4444 の縦並びコマ（各コマの上下 1 行は余白）。`blink` は自動まばたき、`lip` は再生中の声の `.LIP` に合わせて口を動かし、それ以外のときは描かずに背景を見せます。`lip<N>` は人物 N の口で、直前の `SPEAKER` が N のときだけ動きます（同じ人物の口が複数あれば最初の 1 つだけ。PS と同じ規則）。`BG` で全部消えます。 |
| `OVERLAY_CLEAR` | `OVERLAY_CLEAR [枠]` | 重ねている差分をすべて（枠を書けばその枠だけ）消します。 |
| `OVERLAY_TRACK` | `OVERLAY_TRACK <枠> <TRK ファイル>` | フレームごとの位置・大きさ・回転・絵を並べたファイルどおりに、絵を動かします（60 Hz、ループ位置つき。形式は `kosvn_overlay.c` の先頭）。最大 4 枚の絵を切り替えられます。`show` モード（`OVERLAY ... show`）は動かない絵を出すだけです。 |
| `LIP_MAP` | `LIP_MAP <コマ数> <形0> <形1> <形2> <形3> <形4>` | 口パクで、その枚数のコマを持つ口に、口の形 0〜4 をどのコマで見せるかを決めます（タイトル台本などで一度だけ）。書かなければ「形の番号 = コマの番号」です。 |
| `SPEAKER` | `SPEAKER <N\|none>` | 次の `VOICE` を話す人物の番号です。`lip<N>` の口のうち、どれを動かすかを決めます。 |
| `SCENE_SE` | `SCENE_SE <WAV> [loop] \| stop` | 場面の効果音です。声と同じストリームで鳴らすので次の `VOICE` で止まりますが、台詞を読み進めても止まりません（PS と同じ）。`loop` を付けると止めるまで繰り返します。長さの上限はありません。 |
| `MOVIE` | `MOVIE <パス>` | ゲーム側が `vn_set_movie_handler()` で登録した再生関数でムービーを再生します（声と BGM は止めます）。登録がなければ飛ばします。 |
| `JUMP` | `JUMP <スクリプトのパス> [ラベル名]` | 別のスクリプトファイルへ移って続けます。フラグとビットは引き継ぎ、ラベル・コールスタック・サイトは読み直します。 |
| `CANCEL` | `CANCEL <ジャンプ先>` | `CHOICE` の直下に書くと、選択肢を出している間に `B` でそこへ飛びます（項目としては出しません）。 |
| `MAP` | `MAP <地図 RAW16> <目印 RAW16>` | `CHOICE` の直下に書くと、その選択肢を地図の上の場所選びとして出します。地図は 512x256 RGB565（左上 320x240 を 2 倍で全画面に表示）、目印は 32x64 ARGB4444（24x48 を 2 倍）。項目は `OPTION @点x,点y,目印x,目印y,順位 <場所名> \| <飛び先>` と書き、点（明滅）と、選んだ場所の目印、下の枠に場所名を出します。左右（上下も可）で移動し、最初は順位のいちばん小さい項目にカーソルが乗ります。 |
| `STORY_LOCAL_BITS` | `STORY_LOCAL_BITS <開始> <終了>` | 話ごとに別々に持つビットの範囲です（最大 128 個）。場面ごとに使い回す作業用のビットを、話を切り替えたときに相手の話へ持ち込まないようにします。`STORY_DEF` の前に置きます。 |
| `STORY_DEF` | `STORY_DEF <番号0..2> <最初のスクリプト> <メニューの表示>` | 並行して進む話（主人公ごとの編など）を登録します。`START` メニューに「<表示>」が出て、その話の続き（未開始なら最初のスクリプト）へ切り替えます。ビットは共通、位置は話ごとに覚えてセーブにも入ります。 |
| `STORY` | `STORY <番号>` | このスクリプトがどの話に属するかを示します（各スクリプトの先頭に置く）。 |
| `STORY_RESET` | `STORY_RESET` | 覚えている話ごとの位置を消します（ニューゲーム用）。 |
| `SYSLOAD` | `SYSLOAD` | セーブスロットの一覧を出して読み込みます（タイトル画面の「ロード」用）。「戻る」なら次の行へ進みます。 |
| `TEXT_AREA` | `TEXT_AREA <x> <y> <幅> \| off` | 背景の絵に文字欄があるとき（PS の会話枠など）、こちらの台詞枠・名前枠を描かずに、その欄の 1 行目に名前、続く 3 行に台詞を書きます（640x480 の座標）。台本の先頭に置きます。 |
| `TEXTBOX` | `TEXTBOX <on\|off>` | 台詞枠と名前枠の描画を出す／消します（文字と選択肢は描きます）。 |
| `CHOICE_CURSOR` | `CHOICE_CURSOR <x> <y> <間隔> \| off` | 選択肢の項目が絵に描いてあるとき、カードを描かずに項目の左にカーソル（●）だけを描きます。座標は 640x480 の画面で、n 番目の項目は y + n×間隔。 |
| `CUTIN` | `CUTIN <アセット指定 \| none>` | 画面中央に証拠品・アイテムのカットイン枠を表示／`none` で消去します（`proc:item_keycard` 等や 128x128 RAW16 に対応）。 |
| `FADE` | `FADE <0xAARRGGBB> <時間ms>` | 画面全体のカラーオーバーレイをフェードさせます（例: 黒フェードアウト `0xFF000000 600`、フェードイン `0x00000000 600`）。 |
| `SHAKE` | `SHAKE <振幅px> <時間ms>` | 画面とメッセージウィンドウを揺らします（例: `SHAKE 8.0 350`）。 |
| `WAIT_FX` | `WAIT_FX` | 実行中の `FADE` / `SHAKE` が完了するまで待機します（`A` またはスキップで即解除可）。 |
| `BGM` | `BGM <アセット指定 \| stop \| fade <ミリ秒>>` | AICA ストリーミング BGM を再生または停止（`stop`）します。`fade` は指定時間で音量を下げてから止めます（台本は待たずに進みます）。 |
| `SE` | `SE <アセット指定>` | AICA 効果音を再生します（FIFO 8枠キャッシュ付き）。 |
| `VOICE` | `VOICE <アセット指定 \| stop>` | 専用ボイスチャンネルで台詞音声を再生します（次の台詞送りやサイト切替時に自動停止）。16bit PCM または 4bit ADPCM（KOS `utils/wav2adpcm` の出力）の `.wav` は RAM に読んでストリーム再生するので長さの制限はありません。同じ名前の `.LIP`（[コマ数 u8, 口の形 u8] の並び）があれば口パクに使います。スキップ中は読み込みません。 |
| `END` | `END` | シナリオを終了し、`vn_run_script()` を抜けます。 |

### シナリオ記述例（EVE シリーズ型 Multi-Sight 捜査 ADV）

```text
# 導入〜マルチサイト初期化
SET 1 0
SET 2 0
SET 3 0
BG proc:night_sky
BGM proc:ambient_calm
FADE 0xFF000000 0
FADE 0x00000000 600
WAIT_FX

SITE_INIT 0 小次郎 site_kojiro
SITE_INIT 1 まりな site_marina
SITE_SWITCH 0

LABEL site_kojiro
BG proc:night_sky
BGM proc:ambient_calm
CHAR left proc:sakura_normal
VOICE proc:voice_kojiro
NAME 小次郎
TEXT 「真夜中の事務所か……事件の手掛かりを探そう。\nどのコマンドで捜査を進める？」

CHOICE
OPTION 【見る】周囲を調べる | @call:kojiro_look
OPTION_IF 1 == 0 【調べる】机のカードキー | @call:kojiro_desk
OPTION_IF 1 == 1 【調べる】金庫のマイクロフィルム | @call:kojiro_film
OPTION 【サイト切替】まりな側へ (L+X) | @site:1
OPTION_IF 2 == 1 【突入】ロック解除・合流へ | finale_converge

LABEL kojiro_desk
CUTIN proc:item_keycard
SET 1 1
ADD 3 1
NAME 小次郎
TEXT 「机の引き出しにセキュリティカードキーがある！\nこれで奥のファイルも開けられそうだ。」
CUTIN none
RET

LABEL site_marina
BG proc:cyber_lab
BGM proc:ambient_tense
CHAR right proc:rei_normal
VOICE proc:voice_marina
NAME まりな
TEXT 「地下ラボへ潜入完了。小次郎さん、聞こえる？\n端末ロックを解除すれば合流できるわ！」

CHOICE
OPTION 【調べる】メイン端末を解析 | @call:marina_hack
OPTION 【サイト切替】小次郎側へ (L+X) | @site:0

LABEL marina_hack
CUTIN proc:item_microfilm
SET 2 1
ADD 3 1
SHAKE 6.0 280
SE proc:flash
NAME まりな
TEXT 「ハッキング完了！セキュリティが解除されたわ！\n小次郎さんのルートへ戻って突入しましょう！」
CUTIN none
RET

LABEL finale_converge
CLEAR_CHAR all
CUTIN none
BG proc:shrine_sunset
BGM proc:ambient_calm
CHAR left proc:sakura_smile
CHAR right proc:rei_smile
IF_AND 1 == 1 2 == 1 GOTO finale_true
NAME まりな
TEXT 「合流成功ね！残りの証拠品も揃えて完璧を目指しましょう！」
GOTO ending

LABEL finale_true
NAME 小次郎＆まりな
TEXT 「全サイト連動＆証拠品解析まで完全クリアだ！\nこれぞDreamcast専用EVEエンジン！」

LABEL ending
BGM stop
FADE 0xFF000000 700
WAIT_FX
END
```

---

## 7. 画像・サウンドアセット仕様

### A. 外部ファイルを使う場合（`/rd/...` または `/cd/...`）
Dreamcast の PowerVR2 / AICA ハードウェアへ高速転送するため、以下の非圧縮フォーマットを直接読み込めます（2の累乗サイズ必須）。

| 種別 | 解像度・形式 | 備考 |
| --- | --- | --- |
| **背景 (`BG`)** | `512 x 512` / 16bit **RGB565** (Non-Twiddled Raw) | `640x480` 画面全体にバイリニア補間描画（1枚 `524,288` bytes） |
| **立ち絵 (`CHAR`)** | `128 x 256` / 16bit **ARGB4444** (Non-Twiddled Raw) | 半透明抜き対応・自動でフェードイン描画（1枚 `65,536` bytes） |
| **カットイン (`CUTIN`)** | `128 x 128` / 16bit **ARGB4444** (Non-Twiddled Raw) | 証拠品・アイテム小窓枠付きで画面中央に表示（1枚 `32,768` bytes） |
| **BGM (`BGM`)** | `22050 Hz` / 16bit PCM / モノラル **WAV** | 先頭 44 byte ヘッダを読み飛ばしてストリーミングループ再生 |
| **効果音 / ボイス (`SE` / `VOICE`)** | KOS `snd_sfx_load()` 対応 **WAV** | AICA サウンドメモリへロードして発音（`VOICE` は台詞送りで自動停止） |

> ※ 独自解像度やカスタムフォーマットを読み込みたい場合は、C API の `vn_tex_load_raw16(tex, path, w, h, fmt)` を直接呼び出せます。

### B. 組み込みプロシージャル素材（`proc:` プレフィックス）
外部アセットなしで即テストできるよう、以下のキーワードを内蔵しています。

* **背景:** `proc:night_sky`（夜空と月・ビル群）、`proc:shrine_sunset`（夕焼けと鳥居）、`proc:cyber_lab`（サイバーグリッド）
* **立ち絵:** `proc:sakura_normal` / `proc:sakura_smile` / `proc:sakura_surprised`、`proc:rei_normal` / `proc:rei_smile` / `proc:rei_surprised`
* **カットイン:** `proc:item_keycard`（セキュリティカードキー）、`proc:item_microfilm`（光ディスク／マイクロフィルム）、`proc:item_file`（極秘資料）
* **BGM:** `proc:ambient_calm`、`proc:ambient_tense`
* **SE / ボイス:** `proc:chime`、`proc:select`、`proc:flash`、`proc:voice_kojiro`、`proc:voice_marina`

---

## 8. 実機／エミュレータでのコントローラ操作一覧

`libkosvn` のメインループ（`vn_step_frame()`）には、標準コントローラ向けのノベル操作が組み込まれています。

| 操作 | 機能 |
| --- | --- |
| **`A` ボタン** | 文字送り途中で全表示 ／ 表示完了後に次の行へ ／ 選択肢決定 |
| **`B` ボタン** | メッセージウィンドウ表示／非表示切替（立ち絵・背景鑑賞用） |
| **`X` ボタン** | **オートモード (`AUTO`)** の ON / OFF 切替 |
| **`Y` ボタン** | **バックログ (`BACKLOG`)** 履歴画面の開閉 |
| **十字キー 上下** | 選択肢のカーソル移動 ／ バックログ表示中のスクロール |
| **`R` トリガー（長押し）** | **スキップ (`SKIP`)** 早送り |
| **`L` トリガー + `A`** | **VMU スロット 0 (`/vmu/a1/KOSVN00`) へクイックセーブ**（`KVN2` 形式：全サイト状態・コールスタック・256 フラグ・BGM/立ち絵/カットインを一括保存） |
| **`L` トリガー + `B`** | **VMU スロット 0 からクイックロード**（`CHOICE` 待機中の行へのロード時は直前台詞も自動復元） |
| **`L` トリガー + `X`** | **Multi-Sight サイトザッピング切替**（有効時に小次郎⇔まりな等の視点を即時切替、`SITE_LOCK` 中は `[SITE LOCKED]` 通知を表示してブロック） |
| **`START` ボタン** | `vn_config_t.title_script` があればシステムメニュー（セーブ／ロード／話の切り替え／タイトルへ戻る／閉じる）。セーブとロードは 4 スロット（`VN_SAVE_SLOTS`、VMU のファイル KOSVN00〜03、1 つ約 9 ブロック）で、一覧には台本名と台詞の頭が出ます。`L+A` / `L+B` は最後に使ったスロットへのクイックセーブ／ロード。タイトル画面では `A` と同じ。`title_script` が無ければゲームループ終了 |

---

## 9. LAN デバッグ &amp; UDP テレメトリの使い方

`INIT_VN_DEFAULT` により、純正 KOS と同等の LAN 機能（BBA / LAN Adapter / `dcload-ip` / UDP ソケット）がそのまま有効になっています。

`vn_config_t.net_debug_port`（例: `47890`）を設定すると、**60フレーム毎またはシナリオ行が進むたび**に、以下の JSON パケットをノンブロッキング UDP（`MSG_DONTWAIT`）で送信します。LAN ケーブル未接続や受信側不在でもフレーム落ちしません。

### 送信パケット例
```json
{"frame":120,"fps":59.9,"line":14,"mode":"PLAY","ram_used":264192,"ram_free":15124480,"vram_free":7208960}
```

### PC 側での受信確認（Python ワンライナー）
```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 47890))
while True:
    print(s.recvfrom(1024)[0].decode().strip())
```

---

## 10. 主要 C API リファレンス (`kosvn.h`)

シナリオ駆動だけでなく、独自のミニゲームや演出を C言語から直接制御したい場合は以下の関数群を利用できます。

* **ライフサイクル:** `vn_init()`, `vn_shutdown()`, `vn_run_script()`, `vn_step_frame()`
* **グラフィックス / 演出:** `vn_bg_set()`, `vn_char_set()`, `vn_char_clear()`, `vn_char_clear_all()`, `vn_cutin_set()`, `vn_cutin_clear()`, `vn_fx_fade()`, `vn_fx_shake()`, `vn_fx_is_busy()`, `vn_tex_load_raw16()`, `vn_tex_free()`
* **テキスト / 選択肢 / 履歴:** `vn_text_set_speaker()`, `vn_text_set_dialogue()`, `vn_text_add_choice()`, `vn_backlog_push()`, `vn_backlog_set_visible()`
* **スクリプト VM & Multi-Sight:** `vn_script_load()`, `vn_script_jump_label()`, `vn_script_call_label()`, `vn_script_return()`, `vn_script_advance()`, `vn_script_set_flag()`, `vn_script_get_flag()`, `vn_script_add_flag()`, `vn_site_init()`, `vn_site_switch()`, `vn_site_get_active()`, `vn_site_is_enabled()`, `vn_site_set_locked()`, `vn_site_is_locked()`
* **VMU:** `vn_vmu_save()`, `vn_vmu_load()`, `vn_vmu_draw_logo()`, `vn_vmu_set_icon()`（セーブファイルのアイコン 32x32・16 色）, `vn_vmu_set_desc()`（ファイル管理画面に出る名前）
* **サウンド:** `vn_audio_play_bgm()`, `vn_audio_stop_bgm()`, `vn_audio_play_se()`, `vn_audio_play_voice()`, `vn_audio_stop_voice()`
* **LAN デバッグ:** `vn_netdbg_init()`, `vn_netdbg_log()`, `vn_netdbg_send_telemetry()`

---

<a id="upstream-kallistios-overview--license"></a>
## Upstream KallistiOS Overview &amp; License

以下はフォーク元である **KallistiOS** の原文 README およびライセンス表記です。KallistiOS のライセンス要件に基づき、著作権表示とライセンス文書（[LICENSE.md](doc/LICENSE.md) / [LICENSE.KOS](doc/license/LICENSE.KOS)）をそのまま保持しています。

---

<!-- UPSTREAM OVERVIEW -->
<div align="center">
  <h2 align="center"><strong>KallistiOS-VisualNovel (Upstream: KallistiOS)</strong></h2>
  <p align="center">
    Independent SDK for the Sega Dreamcast（セガ・ドリームキャスト向け独立系オープンソース SDK）
    <br />
    <a href="https://kos-docs.dreamcast.wiki"><strong>Explore the docs / 公式ドキュメント »</strong></a>
  </p>
</div>

### Overview / 概要（和文併記）

KallistiOS is a development library and operating system for the Sega Dreamcast game console, developed independently from Sega entirely by free software developers. Its flexible permissive license allows both homebrew and commercial use with little restrictions besides proper attribution. As a result, it powers most homebrew and commercial indie releases for the platform. Interfaces and drivers are included for a significant portion of the Dreamcast's hardware capabilities and accessories, including modifications created by hobbyists.

> **和訳（概要）:** KallistiOS は、セガ公式とは独立してフリーソフトウェア開発者により開発されている、セガ・ドリームキャスト向けの開発ライブラリ兼オペレーティングシステムです。適切な著作権表示（クレジット表記）を行えば自作（Homebrew）・商用を問わず利用できる柔軟なライセンスを採用しており、同プラットフォーム向けの多くの自作・インディー商用タイトルの基盤となっています。本体ハードウェア機能や純正周辺機器、有志による改造パーツ向けの各種インタフェースやドライバを備えています。

KallistiOS offers a modern, programmer friendly environment for the vintage Dreamcast system. The distribution includes [scripts for building and installing a cross-compiling toolchain](utils/kos-chain/README.md) using the latest GCC, Binutils, and Newlib. This allows for full support for C17 and C++20 standards and libraries, as well as various POSIX APIs. Preliminary support exists for C23, C++23, and Objective-C.

> **和訳（開発環境）:** 最新の GCC・Binutils・Newlib を使った[クロスコンパイルツールチェイン構築スクリプト](utils/kos-chain/README.md)が含まれており、実機向けながらモダンな開発環境を提供します。C17 / C++20 標準ライブラリや各種 POSIX API をフルサポートし、C23 / C++23 / Objective-C の試験的サポートも備えています。

KallistiOS also features a package manager called [**_kos-ports_**](https://github.com/KallistiOS/kos-ports) which gives developers the power to build and include a rich set of add-on libraries for various common audiovisual formats (jpg, png, mp3, ogg, mpeg), compression formats (libbz2, zlib), scripting languages (Lua, Tcl, MicroPython), gaming APIs (OpenGL, OpenAL, SDL), and many more.

> **和訳（追加ライブラリ）:** [**_kos-ports_**](https://github.com/KallistiOS/kos-ports) と呼ばれるパッケージ機構により、画像・音声（jpg, png, mp3, ogg, mpeg）、圧縮（libbz2, zlib）、スクリプト言語（Lua, Tcl, MicroPython）、各種 API（OpenGL, OpenAL, SDL など）を追加組み込みできます。

### Core Functionality / コア機能（和文併記）
* Concurrency with Kernel Threads, C11 Threads, C++11 `std::thread`, POSIX threads  
  （カーネルスレッド / C11スレッド / C++11 `std::thread` / POSIX スレッドによる並行処理）
* Virtual Filesystem Abstraction  
  （仮想ファイルシステム抽象化 VFS）
* IPv4/IPv6 Network Stack  
  （IPv4/IPv6 ネットワークスタック）
* Dynamically Loaded Libraries and Modules  
  （動的ロード可能なライブラリ / モジュール）
* GDB Debugger Support  
  （GDB デバッガサポート）

### Dreamcast Hardware Support (Upstream Reference) / ドリームキャスト対応ハードウェア（本家リファレンス・和文併記）
* Optical Disc Drive（GD-ROM / 光学ディスクドライブ）
* Low-level 3D PowerVR Graphics（低レベル PowerVR2 グラフィックス）
* SH4 ASM-Optimized Math Routines（SH-4 アセンブリ最適化済み数値演算ルーチン ※`KallistiOS-VN` では 3D 行列演算を除外）
* SH4 SCI and SCIF Serial I/O（SH-4 SCI / SCIF シリアル入出力）
* DMA Controller（DMA コントローラ）
* FlashROM Filesystem（FlashROM ファイルシステム）
* AICA SPU Sound Processor Driver（AICA サウンドプロセッサドライバ）
* Cache and Store Queue Management（キャッシュおよびストアキュー管理）
* Timer Peripherals, Real-Time Clock, Watchdog Timer（タイマー・RTC・ウォッチドッグタイマー）
* Performance Counters（パフォーマンスカウンタ）
* MMU Management（MMU メモリ管理）

### Getting Started (Upstream) / はじめに（本家ドキュメント案内・和文併記）
A beginner's guide to development for the Sega Dreamcast along with detailed instructions for installing KOS and the required toolchains can be found on [dreamcast.wiki](https://dreamcast.wiki/Getting_Started_with_Dreamcast_development). Additional documentation can be found in the docs folder.

> **和訳:** ドリームキャスト開発の初心者向けガイドおよび KOS／ツールチェイン導入手順は [dreamcast.wiki](https://dreamcast.wiki/Getting_Started_with_Dreamcast_development) に掲載されています。リポジトリ内の `doc` フォルダにも各種ドキュメントがあります。

### Licensing / ライセンス（和文併記）
KallistiOS itself is licensed under the BSD-like **KOS License**. **Attribution is not optional**. Additionally, this distribution contains code licensed under various free software licenses.
See [LICENSE.md](doc/LICENSE.md) for more information on licensing, as well as [LICENSE.KOS](doc/license/LICENSE.KOS) for the actual **KOS License** text.

> **和訳:** KallistiOS 本体は BSD ライセンスに類似した **KOS License** の下で公開されています。**著作権表示（Attribution）は必須**です。また、本ディストリビューションには各種フリーソフトウェアライセンスのコードが含まれています。詳細は [LICENSE.md](doc/LICENSE.md) および **KOS License** 本文の [LICENSE.KOS](doc/license/LICENSE.KOS) を参照してください。

### Resources / 関連リソース（和文併記）
* [dreamcast.wiki](http://dreamcast.wiki): Large collection of tutorials and articles for beginners（初心者向けチュートリアル・技術記事集）
* [Simulant Discord Chat](https://discord.gg/bpDZHT78PA): Home to the official Discord channel of KOS（KOS 公式 Discord チャンネル）
* [DCEmulation Forums](http://dcemulation.org/phpBB/viewforum.php?f=29): Goldmine of Dreamcast development information and history（ドリームキャスト開発情報・歴史フォーラム）
* IRC Channel: irc.libera.chat `#dreamcastdev`（IRC 開発チャンネル）
