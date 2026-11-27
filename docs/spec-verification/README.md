# SVWB カード仕様テスト

対象はコミット `d9df053d3957bb2000295a2895ae81cd2de2bee5` に登録されたローテーション516種と補助エンティティです。テストは `cpp/tests/wb_spec` にあり、通常の対戦ルールを経由して、カード定義、プレイ可否、結果の状態、境界条件を検査します。CTest の `wb_spec` ラベルで、新規グループをまとめて実行できます。

## ビルドと再現

独立したチェックアウトのルートで実行します。以下の `build/spec-check` は、そのチェックアウト内の専用ディレクトリです。Windows で PATH に CMake がなければ、`Agent.md` 記載の Visual Studio 同梱バイナリを指定します。

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
$env:OMP_NUM_THREADS = '1'
$env:MKL_NUM_THREADS = '1'
$env:CUDA_VISIBLE_DEVICES = '-1'
cmake -S . -B build/spec-check
cmake --build build/spec-check --config Release --target test_wb_spec
ctest --test-dir build/spec-check -C Release -j 1 -L wb_spec
& ./build/spec-check/Release/test_wb_spec.exe --list
& ./build/spec-check/Release/test_wb_spec.exe catalog.10031310.effect-1-Play-Draw --exact
```

単一構成ジェネレーターでは実行ファイルは `build/spec-check` の直下にあります。ケース本体には `CHECK` を使い、Release でも評価します。指定したケースが存在しない場合、実行ファイルは終了コード2を返します。再現用に保存した差異があるため、まとめ実行は終了コード1を返す場合があります。

```powershell
& ./build/spec-check/Release/test_wb_spec.exe > build/spec-check/cases.log
python -X utf8 cpp/tests/wb_spec/coverage_report.py --run-log build/spec-check/cases.log --output build/spec-check/coverage.json
python -X utf8 cpp/tests/wb_spec/mutation_run.py --output-dir build/spec-mutations
```

ミューテーション出力先は未作成の専用ディレクトリを指定します。ランナーはそこにソースとビルドを隔離し、各変異について元のケースが通ることを先に確認します。ビルド失敗と、実行したアサーションによる検出を別々に集計します。 `--cmake` と `--generator` でツールを指定できます。

既存516枚のカード別プレイ確認を台帳に取り込む場合は、`test_wb` の `--rotation-unit`、`--elf-royal-unit`、`--dragon-nightmare-unit`、`--witch-nemesis-unit`、`--high-cost-unit` の出力を1つの UTF-8 ログに保存し、集計コマンドに `--legacy-play-log <ログパス>` を追加します。

## 台帳の更新

- カード基本データは公開 Deck Portal の取得済みスナップショットを使用します。追加トークンの `official_supplement.json` は Cygames API の保存応答から、カードID、種別、基本数値、能力文、取得元を抜粋したテスト入力です。テスト実行時のネットワークアクセスは不要です。
- `spec_data.py` は実装ファイルからID、登録対象、既存の効果スロットを列挙し、公式データの期待値と照合します。完全一致する単純な能力文だけを自動ケースに変換します。モード、連鎖、タイミング、対象の絞り込みは、各担当のシナリオと根拠を追記します。
- 新規シナリオを追加したら、同じ名前を担当の TSV の `case_name` に追加します。複数の対象IDを扱う場合はカンマで区切り、`effect_or_branch` に発動と不発のうち実際に検証する条件を記載します。台帳では、能力文、実装上の登録スロット、ケースの実行結果、既存テスト内のID参照を確認できます。既存テスト内のID参照は監査候補です。
- クレスト・特殊形態のIDに対して API が本体を返さない場合は、元カードの公式ページや取得元を明記できる検証資料を使用します。未確認の裁定は台帳と担当別ノートに残します。
- `search_keys.cpp` は実際の探索ソースをテストターゲットに組み込み、内部の比較器とハッシュを検証します。この結果はゲーム中の裁定と区別して扱います。

カードと共通ルールの参照元は公式 Deck Portal と Cygames ヘルプです。 [カード一覧](https://shadowverse-wb.com/ja/deck/cardslist/) [公式ヘルプ](https://shadowverse-wb.com/ja/help?tab=tab0)
