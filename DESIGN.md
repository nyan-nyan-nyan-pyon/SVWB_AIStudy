# カードゲームAI解析システム 設計・実装仕様書

## 1. プロジェクト概要
特定の2つのデッキ（デッキA vs デッキB）の対戦に特化した強化学習AIを構築し、最善手同士の対戦勝率・定石・マリガン優先度を算出する。

- **ベース手法**: 1ターン内ビームサーチ（幅64）＋ ターン間MCTS（PUCT）＋ 超軽量MLP（64→32）
- **前提条件**: 山札および相手手札は公開情報として扱う（完全情報ゲーム近似）。
- **想定実行環境**:
  - CPU: Intel Core i5-14400F（10コア / 16スレッド）
  - RAM: 32GB
  - GPU: NVIDIA GeForce RTX 4060（VRAM 8GB）
  - 将来的な並列展開先: Google Colab（L4 GPU / 12コアCPU）

---

## 2. 技術スタックとアーキテクチャ方針

| モジュール | 言語 / ツール | 役割 |
| :--- | :--- | :--- |
| シミュレーター・探索・推論 | **C++20**（CMake + マルチスレッド） | 盤面進行、ビームサーチ、MCTS、MLP順伝播、バイナリログ出力 |
| モデル学習・世代管理 | **Python 3.11+ / PyTorch (CUDA)** | バイナリログ読み込み、重み更新（RTX 4060）、収束判定、実行制御 |
| テスト | **GoogleTest / Catch2**（または標準assert） | カード効果の単体テスト、ランダム対戦耐久テスト、ミラー戦検証 |

### 設計上の重要方針
1. **探索・推論はCPU並列（C++）で完結させる**: 隠れ層64→32の超軽量MLPは、PythonからGPUへ転送するオーバーヘッドの方が大きいため、C++側で直接順伝播を計算する。
2. **GPU（RTX 4060）は学習専用**: 自己対戦で溜まったバイナリログから重みを更新する工程のみPyTorch（CUDA）で行う。
3. **プロセス間連携はバイナリファイル**: `weights.bin` / `val_log.bin` / `pol_log.bin` を介して疎結合にし、ビルドやデバッグの複雑さを避ける。

### 推奨ディレクトリ構成
```text
project_root/
├── CMakeLists.txt
├── DESIGN.md
├── cpp/
│   ├── include/
│   │   ├── state.hpp       # 固定長 GameState, Follower, Action
│   │   ├── card_db.hpp     # CardDef, Effect, 静的カードテーブル
│   │   ├── engine.hpp      # 共通フレーム（効果解決・死亡判定・合法手生成）
│   │   ├── mlp.hpp         # 特徴抽出・軽量MLP推論・重み読み込み
│   │   └── search.hpp      # ターン内ビームサーチ・ターン間MCTS
│   ├── src/
│   │   ├── engine.cpp
│   │   ├── mlp.cpp
│   │   ├── search.cpp
│   │   └── main.cpp        # 自己対戦CLI / 評価対戦CLI
│   └── tests/
│       └── test_engine.cpp # 単体テスト・ランダム1万試合テスト
├── python/
│   ├── train.py            # PyTorch学習・weights.bin出力
│   └── run_loop.py         # 世代ループ制御・収束判定
└── data/                   # 実行時生成（ログ・重み・チェックポイント）
```

---

## 3. コアデータ構造・アルゴリズム仕様

### 3.1 固定長 `GameState`（POD構造体・300バイト）
動的配列を排除し、`memcpy` 1回で複製可能にする。
下記のフィールド構成では、両プレイヤーの山札だけで160バイトを占める。

```cpp
enum FollowerFlags : uint16_t {
    FLAG_NONE         = 0,
    FLAG_WARD         = 1 << 0, // 守護
    FLAG_STORM        = 1 << 1, // 疾走
    FLAG_RUSH         = 1 << 2, // 突進
    FLAG_EVOLVED      = 1 << 3, // 進化済み
    FLAG_CAN_ATTACK   = 1 << 4, // 攻撃可能
    FLAG_CAN_HIT_FACE = 1 << 5, // リーダー攻撃可能
    FLAG_ATTACKED_THIS_TURN = 1 << 6, // 攻撃済み。進化後の再攻撃を防ぐ
};

struct Follower {
    uint16_t card_id; // 0 = 空き
    int8_t   atk;
    int8_t   hp;
    int8_t   max_hp;
    uint8_t  _pad;
    uint16_t flags;
};

struct PlayerState {
    int8_t   leader_hp;         // 初期20
    uint8_t  pp;
    uint8_t  max_pp;            // 上限10
    uint8_t  ep;                // 先攻2 / 後攻3
    uint8_t  shadows;           // 墓場
    uint8_t  hand_count;        // 0..9
    uint8_t  board_count;       // 0..5
    uint8_t  deck_idx;          // 0..40
    uint8_t  evolved_this_turn; // 0 or 1
    uint16_t hand[9];
    Follower board[5];
    uint16_t deck[40];
};

struct GameState {
    PlayerState players[2]; // 0: 先攻, 1: 後攻
    uint8_t turn_player;
    uint8_t turn_number;   // 先攻・後攻が同じターン番号を使用
    int8_t  winner;         // -1: 進行中, 0: 先攻勝, 1: 後攻勝, 2: 引分
};
```

### 3.2 静的カード定義と共通フレーム
カードごとの個別分岐を避け、パラメータ定義（`CardDef`）＋イベントキューで解決する。

- **Trigger**: `ON_PLAY`（ファンファーレ/スペル）、`ON_EVOLVE`（進化時）、`ON_DEATH`（ラストワード）
- **EffectType**: `DAMAGE`, `HEAL`, `BUFF`, `DRAW`, `SUMMON`
- **TargetType**: `NONE`, `CHOSEN_ENEMY_FOLLOWER`, `CHOSEN_ANY_ENEMY`, `ALL_ENEMY_FOLLOWERS`, `ENEMY_LEADER`, `SELF_LEADER`, `SELF_FOLLOWER`
- **解決ループ**:
  1. キューからイベントを1つ取り出して適用
  2. リーダー体力 <= 0 なら即座に勝敗確定
  3. 盤面を走査し、体力 <= 0 のフォロワーを削除・左詰め・墓場+1、`ON_DEATH` 効果をキュー末尾に追加
  4. キューが空になるまで繰り返す

### 3.3 行動表現（`Action`）
```cpp
enum class ActionType : uint8_t { END_TURN = 0, PLAY_CARD, EVOLVE, ATTACK };
struct Action { ActionType type; int8_t src; int8_t dst; };
```

`src` はカードプレイなら手札、進化・攻撃なら自分の盤面の添字。`dst` は相手盤面の添字で、相手リーダーは `-1`、対象なしは `-2` とする。ターン終了時の `src` も `-2`。

### 3.4 軽量MLP（64→32）と特徴量
- **入力特徴量 (`FEAT_DIM = 256`)**: 手番プレイヤー視点で正規化した盤面・体力・PP・進化権・墓場等の数値特徴 ＋ 手札/盤面/山札のカードID疎特徴。
- **アーキテクチャ**: `Linear(256, 64) -> ReLU -> Linear(64, 32) -> ReLU -> [head_val(1), head_pol(1)]`
  - 価値出力: `Sigmoid` で推定勝率 `0.0 ~ 1.0`
  - 方策出力: 同一ターンの候補盤面間で `Softmax` を取り探索確率とする
- **`weights.bin` のバイナリ順（float32 連続配列）**:
  `fc1.weight`, `fc1.bias`, `fc2.weight`, `fc2.bias`, `head_val.weight`, `head_val.bias`, `head_pol.weight`, `head_pol.bias`

### 3.5 2階層探索仕様
1. **ターン内ビームサーチ**:
   - 1アクションごとに合法手を展開し、Zobrist Hash等で同一ターン内の重複盤面を排除。
   - 価値スコア上位 **64局面** を残して次のアクションへ進む（最大20ステップ程度）。
   - `END_TURN` または勝敗確定に達した盤面を「ターン終了候補」として収集。
   - 局所解対策: 全ターンの1%でビーム制限を解除（または拡大）して探索する。
2. **ターン間MCTS（PUCT）**:
   - ターン終了候補から「価値＋方策」の上位 **16個 ＋ ランダム2個**（最大18個）を子ノードとして展開。
   - ルートノードにディリクレノイズを付加。
   - 1ターンあたり 64〜100 シミュレーション程度を実行し、序盤は温度付き確率、後半は最大訪問回数で次局面を選択。

---

## 4. バイナリログと学習・収束仕様

### 4.1 バイナリログ形式（リトルエンディアン）
- **`val_log.bin`**: `ValueSample` の連続配列
  - `float feat[256]` + `float target_value`（その局面の手番視点で 勝=1.0 / 引分=0.5 / 負=0.0）
- **`pol_log.bin`**: ターンごとの可変長レコード
  - ヘッダ: `int32 num_cands`
  - 本体: `num_cands` 個の `PolicySample`（`float feat[256]` + `float target_prob`（MCTS訪問割合））

### 4.2 学習設定（PyTorch）
- **最適化**: Adam（`lr=1e-3`, `weight_decay=1e-4`）
- **損失関数**:
  - 価値ネット: `BCEWithLogitsLoss`
  - 方策ネット: マスク付き `LogSoftmax` による交差エントロピー損失
- **リプレイバッファ**: 直近 5〜10 世代分のログを結合して学習し、振動・忘却を防ぐ。

### 4.3 世代ループと収束条件
- **1世代**: C++マルチスレッド（14〜16スレッド）で約5分間の自己対戦 → PyTorchで数秒学習 → `weights.bin` 更新。
- **収束判定の目安（20〜60世代）**:
  - リプレイ局面上の過去世代との価値出力差 `ΔV <= 0.035` かつ 方策分布差 `Δπ <= 0.042`
  - 5世代ごとの平均勝率が、2回連続で前区間から ±3pt 以内に収まる
- **最終評価**: 収束モデル同士でノイズなし 2,000試合（先攻1,000 / 後攻1,000）を実行して勝率を算出。

---

## 5. Codex 実行ロードマップ（段階的実装・テスト・実験）

以下の Phase 順に実装し、各 Phase の完了条件（テスト）を満たしてから次へ進むこと。

### Phase 1: 最小シミュレーター実装と耐久テスト
- [x] `state.hpp`, `card_db.hpp`, `engine.hpp/cpp` を実装。
- [x] テスト用カード10種とトークン1種、各40枚のデッキA・Bを登録。
- [x] `test_engine.cpp` で以下を検証。
  - 各カード効果・守護の攻撃制限・進化権・ラストワード連鎖の単体テスト。
  - ランダムAI同士で **10,000試合** 実行し、クラッシュ・無限ループ・HP/PP/手札枚数の不整合が0件であること。

Phase 1 では初手を双方4枚とし、各ターン開始時に1枚引く。手札が満杯のときに引いたカードは墓場に置く。進化開始は先攻5ターン目・後攻4ターン目とし、進化で攻撃力と体力を各2増やす。[対戦の基本ルール（Cygames）](https://shadowverse-wb.com/ja/system/cardbattle/battle/)

EPは本設計どおり先攻2・後攻3。空の山札から引こうとした側は敗北する。進化時の対象が盤面にいなければ進化のみ行う。同時に死亡したフォロワーのラストワードは手番側から、それぞれ盤面の左からキューに追加する。

Windowsでは Visual Studio Build Tools 2022 の C++ ツールセットを使い、以下の手順で x64 Release ビルドと CTest を実行する。

```powershell
cmake -S . -B build/windows-release -G "Visual Studio 17 2022" -A x64 -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
cmake --build build/windows-release --config Release --parallel 1 -- /nr:false
ctest --test-dir build/windows-release -C Release -V
```

単体10件と耐久10,000試合がすべてPASS。先後を5,000試合ずつ交替し、固定シード `1511464998` で計953,352行動を検査した。状態不整合は0件で、1スレッドの所要時間は検査込みで約0.28秒（約36,000試合/秒）。

### Phase 2: MLP推論と2階層探索（ビームサーチ＋MCTS）の実装
- [x] `mlp.hpp/cpp`（手番視点の特徴抽出・順伝播・`weights.bin` 読み込み）を実装。
- [x] `search.hpp/cpp`（ハッシュによる重複排除、ビーム幅64、MCTS PUCT）とマルチスレッド自己対戦CLIを実装。
- [x] 検証テスト。
  - ランダム重みモデル同士のミラー戦（200試合・先後交互）が正常終了し、1試合あたりの所要時間を計測。
  - ログ出力（`val_log.bin`, `pol_log.bin`）のバイナリサイズが仕様通りであること。

特徴量は添字0〜119が手番側、120〜239が相手側。各120要素の先頭10要素は体力、現在PP、最大PP、EP、墓場、手札枚数、盤面枚数、山札の残り枚数、同ターン中の進化使用フラグ、進化可否。続く55要素にはカードID・攻撃力・体力・最大体力・7種のフラグを持つ5枠の盤面を並べる。末尾55要素は手札・盤面・残り山札のカードID別頻度と、次の2ドローのカードIDのワンホット。添字240はターン番号を40で割った値、241〜255は予約領域で0とする。

ターン内の重複判定では手札と盤面の順序差を吸収し、ハッシュ衝突時は位置キーを比較する。山札は探索開始時の順序を共有し、各局面のドロー位置をキーに含める。盤面の順番に依存する効果を追加した際は、この同値条件を見直す。

価値ログにはターン開始時の手番側視点と最終勝敗を記録する。方策ログの候補は同じ手番側の視点で、ターン内の行動後、次のプレイヤーのドローとPP更新の前の局面を記録する。`weights.bin` は74,376バイト。重み行列はPyTorchに合わせて、出力ユニットを行、入力を列とする行優先順で格納する。価値ログは1ターン1,028バイト、方策ログはターンごとに4バイトの候補数と候補1件あたり1,028バイトを使う。

自己対戦CLIの使用例。別の重みファイルで比較する場合は `--weights` と `--opponent-weights` を指定する。`--seconds` は新規対戦の開始を止める時間で、進行中の試合は最後まで実行する。

```powershell
$logDir = Join-Path ([IO.Path]::GetTempPath()) "svwb-mirror-200"
./build/windows-release/Release/svwb_ai.exe --games 200 --threads 14 --deck-a A --deck-b A --seed 2026 --opponent-seed 2027 --log-dir $logDir
python cpp/tests/check_logs.py $logDir --games 200
```

Windowsの実行サンドボックスでは、ネイティブ実行ファイルからプロジェクト直下へログを書き込むと処理が終了した。テストとベンチマークはOSの一時ディレクトリへ書き込み、読み戻して検証した。

Core i5-14400F、Windows x64 Releaseで、デッキAミラーを各スレッド構成で200試合ずつ計測した。独立したランダム重み（シード2026・2027）を使い、先攻・後攻は各100試合、1ターンあたり64シミュレーションとした。

| スレッド | 全試合の経過時間 | 個々の試合処理の平均 | 試合/秒 |
| ---: | ---: | ---: | ---: |
| 1 | 52.324秒 | 0.261秒 | 3.82 |
| 14 | 6.849秒 | 0.462秒 | 29.20 |

CTestは4件すべてPASS。両スレッド構成のログをPythonで読み戻し、価値ログ3,918件（4,027,704バイト）、方策ログ3,918ターン・55,607候補（57,179,668バイト）を確認した。候補数の範囲、訪問割合の合計、価値ターゲットの手番反転も検証した。

### Phase 3: PyTorch学習パイプラインと世代ループの接続
- [x] `python/train.py` を実装（リプレイ読み込み、CUDA自動選択、`model.pt` と `weights.bin` の出力）。
- [x] `python/run_loop.py` を実装（自己対戦、学習、評価、収束判定、世代ごとのCSV/JSON）。
- [x] `python/plot_metrics.py` と、探索ノイズ・温度サンプリングを無効にするC++評価モードを実装。
- [x] テスト用デッキ同士で500試合×5世代を実行し、学習済みAIがランダムAIに対して **勝率90%以上** になることを確認。

学習には直近5世代のログを使う。第1世代のログから固定の評価用局面を選び、その局面を学習バッチから除外する。ΔVは勝率出力（Sigmoid）の平均絶対差、Δπは同じ候補集合に対する方策分布の平均全変動距離。5世代おきの収束判定では、直近3区間のデッキA自己対戦勝率を比較する。

評価CLIでは `--evaluate --weights FILE --random-opponent`、または `--evaluate --weights FILE --opponent-weights FILE` を使う。モデルは確定勝ちを優先し、通常は訪問数最大の手を選ぶ。ルートノイズ、ビーム拡大、候補のランダム追加を止める。ランダムAIはターン終了を含む合法手を一様に選ぶ。世代ごとの評価ではモデルの担当デッキを入れ替え、各デッキ割り当て内で先後を交替する。勝率は全試合に占める勝利数の割合。

Python 3.11以上と `python/requirements.txt` の依存パッケージを使う。CUDA版のPyTorchは公式のインストール案内を参照。 [PyTorchのインストール案内](https://pytorch.org/get-started/locally/)

```powershell
python python/run_loop.py --generations 5 --games 500 --threads 14 --eval-games 200 --output-dir data/phase3
```

`--work-dir` はC++の一時ログの出力先で、既定値はOSの一時ディレクトリ。自己対戦完了後に世代フォルダーへログをコピーする。`--seconds` で自己対戦の制限時間を指定できる。各世代は評価まで完了してから公開され、同じ設定の `--resume` で最後に完了した世代から再開できる。損失・モデル差分・勝率のPNGと、集計した `metrics.csv`、`metrics.json` を出力ディレクトリ直下に保存する。

RTX 4060、PyTorch 2.12.1+cu130、自己対戦14スレッド、各ターン64シミュレーション、各世代5エポックで検証した。5世代の総経過時間は96.608秒。第3世代で対ランダム190勝／200試合（95.0%）、最終世代で197勝／200試合（98.5%）。第5世代のΔVは0.01150、Δπは0.01923。評価は各世代で対ランダム・対前世代ともに200試合。成果物は `data/phase3/` に保存した。

### Phase 4 の準備: 対戦の手順と初手カード別勝率

`--analysis-dir DIR` を指定すると、対戦結果からUTF-8の `turn_trace.json` と `opening_win_rates.csv` を出力する。`--trace-game` は手順を記録する試合番号（既定は1）、`--trace-top` はターンごとに表示する候補数（既定は5）。`--seed` で対戦シードを指定する。

JSONには選択した操作を実行順に記録する。操作元と対象の枠番号は、各操作の直前の手札または盤面における1始まりの番号。候補の `value` はそのターンの手番側から見たMCTSの平均バックアップ値で、未訪問候補は `null`。訪問割合の分母は表示外を含むルート全体の訪問数。サブツリーを引き継いだ場合は、それまでの訪問も含む。

CSVは競技者・デッキ・カードIDごとに、ターン開始ドロー前の初手4枚にカードが1枚以上あった試合と、なかった試合を数える。重複カードは1試合として扱う。勝率の分母は引分を含む各群の試合数。差は「あり − なし」のパーセントポイントで、片方の群が0件なら空欄になる。この値は初手カードと勝率の観測上の差を表す。

```powershell
$analysisDir = Join-Path ([IO.Path]::GetTempPath()) "svwb-analysis"
./build/windows-release/Release/svwb_ai.exe --evaluate --weights data/phase3/generation_005/weights.bin --deck-a A --deck-b B --games 400 --threads 14 --seed 74026 --analysis-dir $analysisDir --trace-game 1 --trace-top 5
```

最終モデルを両陣営に使用したデッキA対Bのサンプルを `data/phase4-prep/` に保存した。先後各200試合、手順ログは試合1の全22ターン、CSVは両デッキ計14行。CTestは5件すべてPASS。

### Phase 4A: SVWBコアと実デッキ候補の検証

[仕様調査書](docs/仕様調査.md) に基づくルールを `svwb_rules` ライブラリに実装した。状態と合法手APIは `svwb::wb` にあり、公開ヘッダーは `wb_rules.hpp` と `wb_cards.hpp`。Phase 1〜3の状態・256次元モデル・バイナリログは従来のAPIで利用できる。新ルール側の学習用特徴量と探索の接続は、コアとカードの検証後に扱う。

実デッキ候補14種と関連5種をID 101〜119に登録した。ID 120はミラクルアルミラージの結晶状態を表し、同名上限は元カードと共有する。仮ミラーは先頭12種を各3枚、ラピッドハットとテレサを各2枚。固定シードでマリガンとカード効果の抽選を再現できる。状態は安定した `creation_order` で対象を参照し、墓場とカードID別のフォロワー破壊数を分けて保持する。

今回のカードと共通処理に適用される暫定裁定は `RuleConfig` にまとめた。

| 項目 | 既定値と切り替え |
| --- | --- |
| U01・U02 | 交換カードは交換後に戻してシャッフル。山札の末尾に戻す方式も選べる。PP・攻撃権の更新は開始時効果の前で、ドロー直前にする設定もある。 |
| U03・U04 | クレストは個別の減少タイミングを使用し、該当する開始・終了時効果の後で減少する。同時リーサルは既定で非手番側の勝利。40ターン判定と終了時効果の前後を切り替えられる。 |
| U05〜U08 | 超進化攻撃中の破壊は1体ごとに1点。0ダメージのバリア消費・誘発、攻撃不可の反撃、設置当日のアクト、変身後の同名回数を切り替えられる。 |
| U09・U10 | 待機能力の解決時に発生源と能力の有効性を確認する。通常の同時誘発について優先度を変更できる。 |
| U11・U12・U14 | クレスト満杯・満杯時の証拠合算・クレスト消滅時の墓場・ランダム対象の抽選時点を切り替えられる。 |
| カード固有 | 手札0枚の兎は既定で自動進化しない。能力消去は既定で超進化固有能力も無効にする。 |

変身で個体の攻撃済み状態をリセットする。変身先に突進・疾走がある場合や、進化後・超進化後の姿に変身する場合は、そのターンにも攻撃できる。同名カードのターン内行動上限と攻撃対象の制限は変身先に適用する。後攻のエクストラPPは1〜5ターン目に使用した場合だけ6ターン目開始時に補充される。6ターン目開始時点で未使用なら初回の使用権を持ち越し、その後に使用できるのは1回。

コスト変更の組み合わせ、融合、リアニメイトなど、今回のカードに登場しない仕組みは、この段階の検証範囲外。

```powershell
cmake --build build/windows-release --config Release --parallel 1 -- /nr:false
ctest --test-dir build/windows-release -C Release -V
./build/windows-release/Release/test_wb.exe --stress
```

Windows x64 ReleaseビルドでCTest 7件と、Phase 4Aの単体26グループがすべてPASS。仮ミラー10,000試合を固定シード `1463956490` で実行し、733,610行動の状態検証で異常0件。1スレッドの所要時間は検証込みで0.553秒（約18,088試合/秒）。先攻5,450勝、後攻4,550勝。山札切れと後攻40ターン終了の裁定は単体テストで検証した。

### Phase 4B: 確定ミラーデッキの学習と評価

地害の矯正者は内部ID 121、公式カードID 11063110。ファンファーレの消滅・兎2体の場出しと、超進化時の2ドロー・2回復を登録した。確定した40枚は10種が各3枚、5種が各2枚の `kMirrorDeck`。`svwb_ai --rules wb` は両陣営にこのデッキを使う。

Phase 4B のモデルは新ルール専用の256次元特徴を使う。120要素ずつの手番側・相手側に、PP・EP・SEP等の14要素、場5枠を各12要素、手札のカード別頻度、残り山札の15種の頻度、クレスト3種類のカウントダウン、次の2ドローを配置する。添字240にラウンドを格納する。ネットワークの形状とバイナリ形式は従来のものを使い、重みとリプレイは専用の出力ディレクトリで管理する。

マリガンは16種類の交換マスクを、実際の山札順から独立した8通りのサンプルで評価し、平均価値で選ぶ。本戦の探索は完全情報近似で、山札と局面の乱数状態を固定して候補を展開する。評価時はルートノイズ、ランダム候補追加、温度サンプリングを止める。終局の両者敗北は両陣営の価値0として扱う。

収束判定は第20世代以降の5世代ごとに実施する。最新の ΔV が0.035以下かつ Δπ が0.042以下で、直近3つの5世代区間における対前世代勝率の平均が、隣接する2区間とも3ポイント以内なら停止する。評価用リプレイは第1世代から分けた固定の局面を使う。

```powershell
$workDir = Join-Path ([IO.Path]::GetTempPath()) "svwb-phase4b-work"
python python/run_loop.py --rules wb --generations 5 --games 500 --threads 14 --simulations 64 --eval-games 200 --epochs 5 --seed 44026 --work-dir $workDir --output-dir data/phase4b-mirror
python python/run_loop.py --resume --rules wb --generations 23 --games 500 --threads 14 --simulations 64 --eval-games 200 --epochs 5 --seed 44026 --work-dir $workDir --output-dir data/phase4b-mirror
python python/run_loop.py --resume --rules wb --generations 60 --games 500 --threads 14 --simulations 64 --eval-games 200 --epochs 5 --learning-rate 0.0003 --seed 44026 --work-dir $workDir --output-dir data/phase4b-mirror
python python/analyze_mirror.py --work-dir $workDir --output-dir data/phase4b-mirror
```

最終評価は同一モデル2,000試合と対ランダム200試合。手順JSONは各勝者座席で最初に該当した試合を再現して書き出す。初手CSVは各試合で競技者Aの初手を1件数え、先攻・後攻・合算の各15行を出力する。交換前から保持した現物の有無と、交換後の初手にカードが存在するかの両方を集計する。これらの差は観測比較。

第20世代の ΔV は0.06670、Δπ は0.04472で閾値を上回った。第23世代の確定済みチェックポイントから、Adamの学習率を0.001から0.0003に下げて継続した。自己対戦数、探索回数、固定評価局面と収束判定は全区間で共通。

Windows x64 ReleaseのCTestは8/8、WB単体は27/27グループがPASS。確定デッキのランダムミラー10,000試合は初回0.515秒、最終ビルドでの再計測は0.487秒。1スレッドで685,448行動を検査し、異常0件。RTX 4060で自己対戦500試合ずつ25世代、計12,500試合を学習に使用した。最初の5世代は148.672秒、完了した全世代の処理時間の合計は858.416秒。第25世代で ΔV 0.02651、Δπ 0.02415となり、対前世代の5世代平均49.7% → 51.1% → 50.2%と合わせて収束条件を満たした。

固定モデルの2,000試合では先攻978勝（48.90%）、後攻1,022勝（51.10%）、両者敗北0件。平均手番数は14.6245、平均決着ラウンドは7.5570。対ランダム評価は200勝／200試合。最終評価の所要時間は107.690秒。成果物の入口は `data/phase4b-mirror/report.md`。

初手CSVの合算では15種とも保持・非保持を比較できた。先攻の地害の矯正者、後攻の投影の鳥像は元の手札から保持した実績が0件だったため、該当する保持差は空欄。マリガン後の初手における存在・不在の差は全45行で算出できた。

### 第25世代モデルとのローカル対戦

リポジトリルートから Windows x64 Release をビルドして起動する。Web サーバーは Python 標準ライブラリで動作する。

```powershell
cmake -S . -B build/windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build/windows-release --config Release --target svwb_play_cli
python python/play_ui.py --weights data/phase4b-mirror/generation_025/weights.bin --port 8000
```

[ローカル対戦画面](http://127.0.0.1:8000/) で手番と探索回数を選び、新規対局を開始する。初手で交換するカードを選び、マリガンを確定する。手札や盤面のカードをクリックすると、選べる行動と対象が表示される。AIは自分の手番を自動で実行し、行動順、AI側から見たMCTSの評価値、候補ごとの訪問割合を記録する。未採用候補の非公開カードは伏せて表示する。

別のビルド先は `--exe`、同じ初手で試す場合は `--seed` をサーバーに指定する。1サーバーにつき1対局で、再読み込みすると進行中の局面を取得する。終了はターミナルで Ctrl+C。

`svwb_play_cli --weights FILE [--simulations 64] [--seed INTEGER]` は標準入出力を使い、UTF-8のJSONを1行ずつやり取りする。リクエストは `command` に操作名を指定する。

| 操作 | リクエストのフィールド |
| --- | --- |
| `new_game` | `seat` に `first`、`second`、`random` のいずれか。`seed` と `simulations` は省略可。 |
| `mulligan` | `mask` に0〜15。下位ビットから初手の各枠に対応し、立っているビットのカードを交換する。 |
| `act` | 直前の局面に含まれる `legal_actions` から `action_index` を指定する。 |
| `ai_turn` | AIの手番に呼び出す。選択手順と候補を `ai_turn` で返す。 |
| `analyze_turn` | 人間の手番に呼び出す。局面を進めず、現在の `revision` から探索した候補を `analysis` で返す。候補の `first_action_index` は `legal_actions` の索引。 |
| `state` | 現在の状態を取得する。対局開始前は `null`。 |

成功時は `ok` と `state` を返す。`debug: true` を指定すると、その応答に相手手札の詳細が含まれる。`mulligan`、`act`、`ai_turn` では、直前の状態の `revision` を指定すると古い局面への操作を検出できる。本戦は Phase 4B と同じ完全情報近似の探索を使う。

「あなたの候補手」には、手番開始時と行動後に現在の局面から探索した上位のターン内手順、手番側から見た評価値、訪問割合が表示される。候補内で未公開のカードを引いた後の手札プレイは、カード名を伏せる。

### Phase 5A データ駆動のカード定義と任意対面

公式メタデータ469枚と補足4ファイルを `data/cards/` に格納する。ビルド時に `python/generate_cards.py` が静的データと `engine_cards.json` を検証し、C++テーブルを生成する。公式IDはデッキファイルと対応し、旧ミラーの内部IDは従来のまま使う。補足で定義が未確定のトークンとクレストは、実装済みの効果から参照できる定義を追加するまで実戦に使えない。

カードの効果を追加する場合は `engine_cards.json` に公式ID、内部ID、発動条件、対象、処理手順を登録する。汎用処理は `Operation`、発動条件は `Condition` を使う。複雑な処理は `Custom` と `CardHandler` から呼ぶ。召喚先などのカードを参照する処理では、公式IDを `card_id` で指定できる。次に `cpp/tests/test_wb.cpp` で成功・条件不足・対象不在などの局面を確認する。

デッキJSONは `name` と `cards` を持つ。各カード行に `card_id`（公式ID）と `copies`（1〜3）を指定し、合計40枚にする。用意したビショップ2種はセットの例。裁定は後攻6ターン目まで持ち越した初回エクストラPPや、非手番側が勝つ同時リーサルを含む。

```powershell
cmake -S . -B build/phase5a-release -G "Visual Studio 17 2022" -A x64
cmake --build build/phase5a-release --config Release
ctest --test-dir build/phase5a-release -C Release --output-on-failure
python python/matchup_matrix.py --exe build/phase5a-release/Release/svwb_ai.exe --output-dir data/phase5a-matchups data/decks/bishop-mirror.json data/decks/bishop-monk.json
python python/run_loop.py --exe build/phase5a-release/Release/svwb_ai.exe --deck-a data/decks/bishop-mirror.json --deck-b data/decks/bishop-monk.json --output-dir data/phase5a-training
python python/play_ui.py --exe build/phase5a-release/Release/svwb_play_cli.exe --port 8004
```

任意対面では両デッキ、結晶、実装済み効果から参照するカードを収集し、先後共通の対面ローカルIDを割り当てる。入力は256次元、中間層は64 → 32。学習する対面の `matchup_features.json` はリプレイと重みに付随する。旧ミラーの第25世代モデルは旧ミラーの特徴量を使って再生できる。新しい対面の評価には、そのローカルID体系で学習した重みを指定する。相性表に対面別の重みを用意する場合は `--models` に `matchups` の配列を含むJSONを渡す。配列要素には `deck_a`、`deck_b`、`weights` と任意の `opponent_weights` を記述する。

Webのデッキ選択肢は `--deck-dir` 配下のJSON、モデル選択肢は `--models-dir` 配下の `weights.bin` を読む。モデルとデッキは試合開始時に決まる。`GameState` は固定長PODで、山札領域は80枚、旧カード以外の同名行動・破壊履歴は64種分を確保する。`test_wb` は状態のサイズを出力し、正確なビショップミラーで状態の不変条件を確認する。

Windows x64 Releaseで `GameState` は3,552バイト。CTest 9件、WBの単体33グループが通過した。確定ミラー10,000試合では685,448行動を検証し、状態不整合0件。3回の中央値は0.529秒で、同時に計測した旧版の0.481秒に対して約10%増。第25世代モデルの16試合・1スレッド・64シミュレーションで、Phase 5Aは14.978秒、前後に計測した旧版は15.051秒と14.892秒だった。
