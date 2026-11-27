# Phase 2 の保守と再実行

Phase 1 の公式スナップショットと仕様テストを引き継ぎます。旧ケースに加えて `phase2_*` を登録しているため、基本的な全件実行は同じ `wb_spec` ラベルで行えます。個別に確認するときは `test_wb_spec <ケース名> --exact`、定義位置と展開結果を照合するときは `test_wb_spec --inventory` を使います。

## 集計

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
$env:CUDA_VISIBLE_DEVICES = '-1'
cmake -S . -B build/spec-check
cmake --build build/spec-check --config Release --target test_wb_spec
ctest --test-dir build/spec-check -C Release -j 1 -L wb_spec --output-on-failure
& ./build/spec-check/Release/test_wb_spec.exe > build/spec-check/cases.log
& ./build/spec-check/Release/test_wb_spec.exe --inventory > build/spec-check/registrations.tsv
python -X utf8 cpp/tests/wb_spec/coverage_report.py --run-log build/spec-check/cases.log --registration-log build/spec-check/registrations.tsv --output build/spec-check/coverage.json
```

`definition_sites` は `Suite::add` のソース位置数です。同じ位置からパラメータ化して登録されたケースは `execution_instances` に個別に計上します。1つのケース内部でまとめて検査するループの反復は水増ししません。

公式能力文の同一カード内で重複する節は一つにまとめ、台帳の元の節番号を参照できるようにしています。単独で記載された常在キーワードは公式ヘルプとカード本文から対戦時の挙動を検査し、該当する節に紐づけます。節ごとに発動を追跡し、条件付きのキーワードや明示条件には不発、選択文には対象不在を加えます。複数条件や複数対象の全組み合わせは別途レビューします。節に紐づくケースで枝を集計し、失敗した枝も実行済みとして記録します。失敗と成功は別に集計します。 [公式ヘルプ](https://shadowverse-wb.com/ja/help/?tab=tab0)

手作業で対応を追加する場合は、テストの TSV の note に `[clauses: 1=active;2=active,inactive]` のように元の節番号と結果を記載します。使える結果は `active`、`inactive`、`no_target`、`partial` です。複合効果の一部を確認した場合は `partial` を使います。

## ミューテーション

```powershell
python -X utf8 cpp/tests/wb_spec/mutation_catalog.py --coverage build/spec-check/coverage.json --output build/spec-check/mutations-phase2.json --count 180
python -X utf8 cpp/tests/wb_spec/mutation_run.py --manifest build/spec-check/mutations-phase2.json --validate-only --output-dir build/spec-check/mutation-anchors
python -X utf8 cpp/tests/wb_spec/mutation_run.py --manifest build/spec-check/mutations-phase2.json --only phase2_draw_hp_gate --limit 1 --output-dir build/spec-check/mutation-smoke
```

一覧には Phase 1 の代表ケースと Phase 2 で追加した条件検出ケースを含みます。生成器は残りの枠に数値、境界値、発動条件、対象範囲、タイミング、破壊・消滅、解決順を配分します。候補ごとの `probe_scope` は効果を特定したケースとカード単位のケースの区分です。後者の生存は、等価変異、設定したケースに未到達、アサーション不足のどれかを追加分析します。

隔離フォルダー内のコピーを使う `--validate-only` は、元の効果スロットの一致と変異後の値の変化を検査します。ネイティブ実行は `--only` で少数を指定できます。全件をネイティブ実行する際は、学習終了後に明示的な件数制限を与え、候補の基準ケースが成功することを確認します。

状態同一性の根拠は [探索での状態同一性](phase2-search-audit.md)、裁定とゲーム映像は [公式裁定と観測](phase2-rulings.md) にあります。
