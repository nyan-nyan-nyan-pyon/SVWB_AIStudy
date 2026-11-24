# Agent Guide (`SVWB_AIStudy`)

このドキュメントは、`D:/dev/project/SVWB_AIStudy` で作業するすべての AI エージェント（Claude Code / Codex / Antigravity / Cursor 等）が、**セキュリティソフトやサンドボックスによるブロックを回避し、安全かつ再現性高くビルド・テスト・学習を実行するための共通手順書**です。

---

## 1. セキュリティソフト・実行環境の重要制約と対策（必読）

本環境ではセキュリティソフトおよびアプリケーション制御が有効になっており、以下のルールを守らないと `Access is denied`（実行拒否）やプロセスのハングが発生します。

### 1.1 既存の正規ビルドディレクトリ（`build/phase5c-p0-release`）を使用する
- **問題**: 新規のビルドディレクトリ（例：`build/new-dir/`）を作成して新しいパスに `.exe` を生成すると、セキュリティソフトの未知バイナリ検査・実行制御により `Program 'svwb_ai.exe' failed to run: Access is denied` またはスキャン待ちハングが発生します。
- **対策**: CMake ビルドおよびバイナリ実行は、既存の正規ビルドディレクトリ **`D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release`** を使用してください。
- **実行バイナリの正規パス**:
  - `D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/svwb_ai.exe`
  - `D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/svwb_play_cli.exe`
  - `D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/test_engine.exe`
  - `D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/test_search.exe`
  - `D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/test_wb.exe`
- **呼び出し時の注意**: PowerShell 等から連続実行する際は、相対パスではなく上記の**正規化された絶対パス**で呼び出してください。また、リビルド直後やサブプロセス大量実行直後にセキュリティソフトのファイル走査が一時的にファイルをロックする場合があるため、万一 `Access is denied` が出た場合は数秒待ってから再実行してください。

### 1.2 MSBuild は `--parallel` を付けずに単一プロセスツリーで実行する
- **問題**: `cmake --build ... --parallel` を指定すると MSBuild が複数のワーカーノード子プロセス（`MSBuild.exe /nodemode:1`）を常駐生成し、サンドボックスやセキュリティ監視下でプロセス間通信がハング・失敗します。
- **対策**: ビルド時は `--parallel` を付けずに実行してください（単一プロセスでも数十秒で完了します）。
  ```powershell
  $cm = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  & $cm --build D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release --config Release
  ```

### 1.3 一時ファイル・Matplotlib キャッシュをワークスペース内（`build/`）に閉じ込める
- **問題**: ワークスペース外の `C:\Users\<user>\.matplotlib` や `C:\Users\<user>\AppData\Local\Temp` への書き込みは、`[WinError 5] アクセスが拒否されました` やセキュリティソフトの警告を引き起こします。
- **対策**:
  - **Matplotlib キャッシュ**: `D:/dev/project/SVWB_AIStudy/build/phase3-matplotlib` を使用します（`python/run_loop.py` では自動で `MPLCONFIGDIR` に設定済み）。
  - **一時ディレクトリ**:
    - `python/run_loop.py` を実行する際は `--work-dir D:/dev/project/SVWB_AIStudy/build` を指定してください。
    - Python テスト等で `tempfile.TemporaryDirectory` を使う場合は、`dir=ROOT / "build"` を指定してワークスペース内に作成してください。

### 1.4 PyTorch 依存パッケージの参照先（`build/phase3-deps`）
- システムの Python（`C:/Python314/python.exe`）のグローバル `site-packages` には `torch` がインストールされていません。
- PyTorch（`torch 2.12.1+cu130`）一式はワークスペース内の **`D:/dev/project/SVWB_AIStudy/build/phase3-deps`** に配置されています。
- `python/train.py`、`python/run_loop.py`、`cpp/tests/check_attention.py` は起動時に自動で `build/phase3-deps` を `sys.path` に追加しますが、ワンライナー等で `torch` をインポートする場合は以下を設定してください。
  ```powershell
  $env:PYTHONPATH = "D:/dev/project/SVWB_AIStudy/build/phase3-deps"
  ```

---

## 2. ツールチェーンと標準コマンド

### 2.1 CMake / CTest のフルパス
標準の `PATH` に `cmake` / `ctest` が含まれていないシェルセッションでは、Visual Studio 2022 BuildTools 同梱のバイナリをフルパスで呼び出します。

- **CMake**:
  `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`
- **CTest**:
  `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe`

### 2.2 Release ビルドと CTest 全件実行
```powershell
$cm = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
$ct = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"
cd D:/dev/project/SVWB_AIStudy

# 1. Release ビルド（単一プロセス）
& $cm --build build/phase5c-p0-release --config Release

# 2. CTest 全31件の実行
& $ct --test-dir build/phase5c-p0-release -C Release --output-on-failure
```

---

## 3. モデルアーキテクチャ切り替え（`mlp` / `attention`）

`svwb_ai`、`svwb_play_cli`、`python/train.py`、`python/run_loop.py` はすべて `--model-type mlp|attention`（デフォルト: `mlp`）をサポートしています。

| 項目 | 現行軽量 MLP (`--model-type mlp`) | 小型 Attention (`--model-type attention`) |
| --- | --- | --- |
| 総パラメータ数 | 18,626 | 461,826（約 0.462M） |
| ネットワーク構成 | `256 → 64 → 32` + Value/Policy Head | Card Embedding(`600→32`) + Pre-LN Transformer 2層(`d=128, h=4, ffn=512`) + Value(`128→64→1`) + Afterstate Scorer(`192→128→1`) |
| 重みバイナリ形式 | `weights.bin`（74,376 bytes） | `attn_weights.bin`（1,847,384 bytes、`"SVWBATTN"` ヘッダ＋CRC32 付） |
| 特徴スキーマ | `wb-mirror-v2` / `wb-matchup-v2` | `wb-attention-v1` |

### 3.1 推論速度ベンチマーク実行例（`bishop__bishop`、Determinization有効、16試合・64シミュレーション・4スレッド）
```powershell
$exe = "D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/svwb_ai.exe"
$deck = "D:/dev/project/SVWB_AIStudy/data/decks/phase5c/bishop.json"

# Attention モデル（評価モード / 自己対戦モード）
& $exe --rules wb --model-type attention --deck-a $deck --deck-b $deck --games 16 --simulations 64 --threads 4 --seed 2026 --evaluate
& $exe --rules wb --model-type attention --deck-a $deck --deck-b $deck --games 16 --simulations 64 --threads 4 --seed 2026

# 現行 MLP モデル（評価モード / 自己対戦モード）
& $exe --rules wb --model-type mlp --deck-a $deck --deck-b $deck --games 16 --simulations 64 --threads 4 --seed 2026 --evaluate
& $exe --rules wb --model-type mlp --deck-a $deck --deck-b $deck --games 16 --simulations 64 --threads 4 --seed 2026
```

### 3.2 自己対戦＋PyTorch学習ループ（`run_loop.py`）実行例
```powershell
python D:/dev/project/SVWB_AIStudy/python/run_loop.py `
  --exe D:/dev/project/SVWB_AIStudy/build/phase5c-p0-release/Release/svwb_ai.exe `
  --work-dir D:/dev/project/SVWB_AIStudy/build `
  --model-type attention --rules wb `
  --deck-a D:/dev/project/SVWB_AIStudy/data/decks/phase5c/bishop.json `
  --deck-b D:/dev/project/SVWB_AIStudy/data/decks/phase5c/bishop.json `
  --output-dir D:/dev/project/SVWB_AIStudy/build/attn-smoke `
  --generations 2 --games 8 --eval-games 4 --simulations 16 --threads 4 --epochs 2 --batch-size 16 --seed 2026
```

---

## 4. 保護対象データ・既存資産に関する注意

- 既存の学習済みデータ（`data/phase5c/`、`data/phase5c-det-ab/`、`data/phase4b-mirror/`、`data/phase5a-matchups/`）は上書き・変更しないでください。
- 新規実験やスモークテストは必ず別ディレクトリ（例：`build/attn-smoke/` や新しい `data/` サブディレクトリ）を `--output-dir` に指定してください。
