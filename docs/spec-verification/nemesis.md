# Nemesis scenario coverage notes

The Nemesis suite now contains 39 named cases covering 39 official card IDs. Thirty-four card IDs were added in this expansion, with catalog URLs and branch notes in `nemesis.tsv`. Coverage includes Artifact entry triggers, distinct Artifact type thresholds, normal and super evolution, base/enhance and mode choices, duplicate-deck gates, fusion events, target selection, transformations, and token generation.

The suite is not a full audit of every Nemesis card or every random branch. High-value follow-up gaps include:

- **10572310 リヴァイブチューニング:** discard plus recovery of two distinct destroyed followers.
- **10574120 尽小花・イマリ:** spell draw after discard, evolved-spell summon, and Super-Evolve draw of distinct one-cost spells.
- **10574110 スロース:** evolved end-turn banish/crest and all three random start-turn effects.
- **10673310 劣悪の天斧:** hand discount on large-follower entry, auto-evolution selection, and random damage.
- **10771310 ストリートラン:** each mode and the three-distinct-Artifact all-modes override.
- **10973310 ハクガ燐斂:** duplicate-deck single-target branch and unique-deck all-target branch.
- **10673110 愚劣の兵器:** distributed damage on turn end and evolution.
- **10774110 虚刻のアナテマ・スカーレット:** Artifact-type-scaled sweep at zero and positive type counts.
- **10871110 ありふれた幸福・クラトス:** Last Words copy loses its own Last Words.
- **10771120 ドープダンサー** and **10674110 劣悪の純心・カミシラ:** Artifact/large-follower threshold branches.
- **11073110 人滅の紳士:** fusion-trigger random destruction plus healing.

## Central-run triage

The second central run confirmed several fixture errors, now corrected in `nemesis.cpp`: the unchanged HP expectation for a 1-HP follower in the duplicate-deck branch of 10971110; missing pre-existing Ward abilities for the two-follower selector in 10874110; and invalid fusion fixtures for 11073310, 11072110, and 11074110. These fusion hosts require an Amulet material, and one material at hand slot 1 uses fusion mask `1` after excluding the host slot.

The second run exposed a fixture mistake in **10072210 ドールズシアター**: `next_own_turn()` also advances through the opponent's end turn and draws a normal card at the owner's next turn start. The corrected case now checks the second Puppet immediately after the owner's `EndTurn`, then checks the independent countdown after the opponent's `EndTurn` and the owner's draw. No Nemesis engine bug is currently retained from the second run.

Official effect expectations come from `data/cards/svwb_rotation_ja.json` and the stored official capture file `cpp/tests/wb_spec/official_supplement.json`; the per-card official URLs are recorded in the TSV. This subtask did not compile or execute the scenarios.
