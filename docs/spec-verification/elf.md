# Elf scenario triage

Source of expected card text, printed cost, and base stats: `data/cards/svwb_rotation_ja.json` (official catalog entries cited by ID in `elf.tsv`). The cases target recent rotation Elf additions whose simple triggers and thresholds can be isolated deterministically. Each lambda owns its fixture and checks a single scenario boundary or trigger outcome.

## Triage

- **High priority:** 11011110 and 11013110 compare one versus two original-cost-1 cards; 11012110 checks the word “other” at its entry threshold; 11011310 checks the exact Enhance 6 boundary and evolved summons.
- **Trigger coverage:** 11012120 checks the turn-end hand additions; 11014110 checks that Super-Evolve buffs other followers, including fanfare summons.
- Existing `cpp/tests/test_wb.cpp` already exercises the broader Elf rotation catalog, combo/mode behavior, Shrink/Expand/Dustdays/Deer King crests, tokens, and faith mechanics. This fragment adds exact state assertions around the listed branches.

## Gaps

- Randomized target/damage behavior and full-board/hand-overflow outcomes for these cards are not covered here.
- 11014110 is checked for successful fanfare and Super-Evolve but not a crowded-board partial summon boundary.
- Other rotation Elf effects remain covered only by the existing broad suite or catalog smoke path, not a dedicated isolated case in this fragment.

## Expanded trigger coverage

The standalone fragment now contains 20 distinct official Elf card IDs. New cases cover Combo 3/5 boundaries, an end-turn hand discount, board-cost threshold modifiers, Fairy-only entry and Super-Evolve triggers, an ability that fires once per turn across multi-target buffs, and a controlled distinct-follower deck summon. Source lines for each card and supplement token are listed in `elf.tsv`.

## Initial-run failure audit

The six first-run failures were classified as fixture/action errors, not engine bugs, so none is retained as an expected-failure reproduction:

- `elf.11011110...` and `elf.11013110...`: both fanfares add a one-cost Evidence amulet; that amulet counts toward the cards' original-cost-1 thresholds. Fixtures now compare Evidence alone with Evidence plus one qualifying follower.
- `elf.11014110...`: Noishe summons the dog and mouse. A summoned follower does not trigger its own Fanfare, so the dog does not add Evidence in this fixture; expected board count is four.
- `elf.11011310...` and `elf.10814120...`: Enhance is selected automatically by available PP. These cards have no selectable mode, so the legal Play action uses choice zero.
- `elf.10913310...`: Combo counts prior played cards; a card remaining in hand is not included. The boundary now compares two and three prior plays.

These repairs are based on the official card text/token cost in the versioned catalog and `official_supplement.json`. The engine build was managed by the parent; I did not compile or run it.

## Second-run audit and expanded coverage

The sole remaining second-run failure, `elf.11014110...`, was an expected-stat mistake: Super-Evolution grants the source +3/+3, so Noishe is 8/7 from its printed 5/4; the separate +2/+0 applies to the other followers. This was a fixture expectation error, not an engine bug reproduction.

The fragment now adds eleven more dedicated cases for the eleven official Elf IDs 10012120, 10811110, 10814110, 11011120, 10614110, 10711110, 10914120, 11014120, 10714120, 10011130, and 10612120. These focus on high-cost removal, damage caps, deck filters, repeat triggers, and Combo thresholds. Expected text and tokens are referenced in the official catalog and supplement rows cited in `elf.tsv`.

