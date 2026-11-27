# 探索での状態同一性

## PositionHash の使用箇所

| 箇所 | 実際のキー | 同一性の判定 |
| --- | --- | --- |
| `wb_search.cpp` の MLP ビーム | `GameState` または `LegacyPosition` | `seen` と `finished` のキー比較 |
| 同ファイルの Attention ビーム | 同上 | `seen` と `finished` のキー比較 |
| 同ファイルの観測からのプラン生成 | 同上 | `seen` と `finished_det` のキー比較 |
| 同ファイルのチャンス境界後のビーム補完 | `GameState` | `seen` のキー比較 |

現行の4箇所は `unordered_map` または `unordered_set` に `PositionHash` を渡し、結果の同一性は `GameState::operator==` で判断します。アミュレットをアクトしたかだけが異なる状態を同じマップに登録し、《証拠》を出すスペルのダメージが発動する状態と発動しない状態が別々に解決されることを確認しました。別実装の `cpp/src/search.cpp` にある同名のハッシュも、配列の `PositionKey` をキーにする集合で使われています。

`observation_hash_for` の呼び出しは1箇所で、観測に基づく乱数シードを生成します。観測の等価判定は `same_observation` が状態を比較します。隠しデッキと乱数状態を入れ替えてもシードが一致し、公開クレストの残りカウントを変えると観測が異なることをテストしました。

## LegacyPosition のカードID

`pack` は盤面 `CardId` を下位8ビットに切ります。Phase 1の再現用カードIDペアは同一キーになります。本番用の `data/decks/phase5c` 7デッキの28組を実際の `MatchupFeatures` に渡した結果、旧形式を選ぶのはビショップ同型の1組です。

両席のミラーデッキから、結晶・アクセラレート、カードとクレストの出力ID、対象を進化・取り除く2つのカスタム処理をたどると、取り得る盤面カードIDは101～121の21種に収まります。下位8ビットはこの集合内で一意です。この探索の前提はケース `phase2_audit.legacy.production_pair_and_card_id_closure` でチェックしています。新しいカード生成操作が集合に入ったときは、その生成先の監査が必要です。ここで示した到達範囲はIDの切り詰めに関するもので、旧キーが省略する全状態項目を対象とする証明ではありません。
