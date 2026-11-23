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
- [ ] `python/train.py` を実装（バイナリ読み込み、RTX 4060での学習、`weights.bin` 書き出し）。
- [ ] `python/run_loop.py` を実装（C++自己対戦と `train.py` の交互実行、リプレイバッファ管理、収束指標のログ出力）。
- [ ] 検証実験:
  - テスト用デッキ同士で 5〜10 世代回し、学習済みAIがランダムAIに対して **勝率90%以上** になることを確認。

### Phase 4: 実デッキ適用と本格解析
- [ ] 解析したい2つの実デッキのカード定義を `card_db.hpp` に追加し、カード単体テストを全件作成。
- [ ] 1対面の収束まで学習を回し、最終勝率（2,000試合）および初手カード別のマリガン勝率差を出力。
