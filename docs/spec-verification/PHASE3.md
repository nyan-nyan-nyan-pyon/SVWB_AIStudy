# Phase 3 の保守と検証

学習中は、隔離コピーで修正範囲の単体ケースだけを逐次実行します。Phase 3 配布領域の `build_focused.py` は、Phase 2 の変更のないオブジェクトを読み取りで参照し、変更したルール・カードデータ・テストソースを MSVC Release で順にコンパイルして隔離バイナリにリンクします。`run_focused_cases.py` は対象ケースだけを実行して `focused-run.log` と `registrations.tsv` を作ります。`--case` で再現対象を1件に絞れます。

```powershell
python -X utf8 build_focused.py
python -X utf8 run_focused_cases.py
python -X utf8 source/cpp/tests/wb_spec/coverage_report.py --allow-partial --run-log focused-run.log --registration-log registrations.tsv --phase2-spec-log ../spec-verification-phase2/final.log --phase2-legacy-junit ../spec-verification-phase2/native/phase2-legacy-ctest.xml --output coverage.json
python -X utf8 source/cpp/tests/wb_spec/coverage_backlog.py --baseline ../spec-verification-phase2/coverage.json --current coverage.json --decks source/data/decks/phase5c --output backlog.json
```

部分実行レポートは、実行したケースとビルドに登録済みのケースを区別します。Phase 3 で再実行していないケースの結果は `NO RESULT` です。内容を照合した Phase 2 の成功結果は `historical_cases` に保存します。旧単体テストは `cpp/tests/wb_spec/phase2-reviewed-clauses.tsv` に能力文、ケース名、CTestグループ、確認したアサーションを登録し、保存済みJUnitの個別のPASS行と照合します。自動生成されたカウントダウン進行テストも、過去ログのPASSを確認してから該当する節に紐づけます。

`coverage_backlog.py` は当初の Phase 2 台帳が未対応とした節・枝を再分類します。`VERIFIED_IN_PHASE2` は根拠を確認済みの履歴、`NO_LINKED_TEST` は紐づけ未確認、`NOT_APPLICABLE` は集計基準を見直して対象外になった枝です。優先順位2は固定IDと明示された生成元から求めた追跡候補で、カスタム効果の動的な経路は別途監査します。手作業で枝に紐づける場合は `active`、`inactive`、`partial`、`no_target`、`board_cap`、`hand_cap`、`heal_cap` が使えます。

## 学習終了後の実行待ち

1. 専用の隔離ビルドで CMake の `test_wb_spec` と既存の `test_wb` 等をビルドし、CTest を実行する。Phase 3 で登録のみ確認した本番デッキ16ケースと、ノインの既存テストを含める。探索キーの既知の失敗は Phase 2 の台帳と比較する。
2. 本番採用カードの紐づけ未確認の枝を既存テストと照合し、検証が欠けている枝を補完する。固定ID生成経由、キーワード、クレスト・アミュレット、複数モードの順に進む。
3. Phase 2 の残り178候補と、Phase 3 の7候補を隔離したミューテーション実行に掛ける。生存・等価・ビルド失敗・到達しない入力を分類する。
4. 棋譜が利用できる範囲で墓場5からの10753310とEP 2からの10854110を抽出し、旧・新ルールで合法手、遷移、方策候補、終局ラベルの差を測る。必要なログがなければ新規の隔離評価に発動前後の記録を追加する。
5. `PositionHash` の分布と探索時間、`LegacyPosition` が省略するフィールドの到達性を検証する。結果を踏まえて探索の更新と再学習の要否を別途判断する。

[確定裁定](phase3-rulings.md) [本番学習への影響](phase3-training-impact.md)
