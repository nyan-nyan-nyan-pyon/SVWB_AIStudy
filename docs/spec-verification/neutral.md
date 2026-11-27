# Neutral spec test triage

Audited against `data/cards/svwb_rotation_ja.json`, the official card detail URLs in `neutral.tsv`, `data/cards/svwb_missing_tokens.json`, `docs/仕様調査.md`, and the generated card table used by the first central run.

## Fixture corrections

- `10701110` and `10002210`: Filtered draws are checked by exact drawn card ID and by the count/identity of the remaining cards. The fixtures no longer assume how the implementation represents removal through `deck_idx`.
- `10803110` and `10901310`: These modern cards are outside the legacy `destroyed_followers` array's card-ID range. Their history belongs in `PlayerState::other_counters`; writing `destroyed_followers[id - kFirstCard]` was an invalid fixture.
- `10002210`: The follower search removes a card at slot 1 while retaining the non-follower at slot 0, so `deck_idx` advances once, not twice.
- `11002110`: Its Evolve trigger requires the chosen enemy follower in the action's `dst` field; the fixture now selects slot 0.
- `10001110`: Enhance is inferred from available PP and `enhance_cost`; the action choice remains 0 because this card has no selectable modes.
- `10801110`: Evolving grants the standard +2/+2 in addition to the card's Evolve +5/+5, so the printed-stat delta is +7/+7.

## EP restoration

Phase 3 user confirmation establishes EP and SEP caps of 2. The previous EP-3 observation was reproduced on the old implementation. Phase 3 caps both paths and adds 0, 1, and 2 EP cases. See [the Phase 3 rulings](phase3-rulings.md).

## Preserved engine bug repro

- `10503310`: The official catalog text says draw 2, followed by destroying a strongest enemy follower. The first central run failed the two-card hand expectation; the generated card table encodes a single `Draw` amount of 1. The test retains the official two-card expectation and isolates a unique strongest target.

The other Neutral failures from `wb-spec-first.log` were fixture or expected-value errors and have been corrected. No build or test run was performed during this audit.

## Additional official effect branches

Added focused cases for `10901110`'s chosen-plus-unselected damage, `11003110`'s wide damage and two Evidence tokens, `10604110`'s Super Evolve Fanfare, `10704110`'s face-attack exclusion, and `11003210`'s Act/draw/Evidence-spend sequence. Existing `test_wb.cpp` scenarios already substantively cover `10502110`, `10602210`, `10703210`, `10802310`, and `10902110`; their card IDs were not duplicated in this addition.
