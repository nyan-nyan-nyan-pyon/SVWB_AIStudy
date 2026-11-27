# Witch scenario first-run audit

The audit compared each failing scenario with `data/cards/svwb_rotation_ja.json` and `docs/仕様調査.md` §6.3. Tests were not rebuilt during this audit; the parent owns the single-CPU build.

| Official ID | First-run failure class | Audit and action |
| --- | --- | --- |
| 10732310 | Invalid shadow-count expectation | The card is a spell, so its play adds one shadow; destroying the zeroed Earth Sigil adds another. Fixture asserts the Pot has exactly 2 Earth and now expects two shadows after mode 2. The area damage leaves the 4-HP follower at 2 and destroys the 2-HP follower. |
| 10032310 | Invalid shadow-count expectation | The spell and the destroyed Earth Sigil each add one shadow. The Pot is checked at one Earth before Eruption, and the final checks separately verify Sigil removal, surviving follower state, and the correct two-shadow total. Prior follower-damage, leader-health, and hand-count assertions passed. |
| 10031310 | Invalid fixture membership | The second remaining hand card was Mana Aria Scripter Tiko, which is not Spellboostable. Replaced it with Steel Slumber (10833310), whose catalog text explicitly has a Spellboost counter. |
| 10731120 | Target setup and damage expectation | Little Beast's Fanfare and Evolve each choose an enemy follower. Added target 0 to both actions and corrected the Fanfare result from 2 HP to 1 HP because the printed damage is 1. |
| 10632310 | Incorrect own-board target encoding | Own-board selections use `kOwnBoardTarget + slot`, not the raw slot used for an enemy target. Updated the Crystal Hand target to `kOwnBoardTarget`. Since this is a spell destroying a follower, the expectation is two shadows: one for spell play and one for the destroyed follower. |
| 10831110 | Missing required target | Green Fortune's Fanfare and Evolve repeat the chosen enemy-follower damage. Added the sole enemy as target 0 on play. |
| 10632120 | Incorrect Enhance action choice | Enhance is selected automatically from available PP when no printed modal choice exists. The Play action uses choice 0; at 6 PP it pays the Enhance cost and summons two copies. |
| 10832320 | Invalid lethal expectation | The target had 9 HP, so the printed 8 damage could not destroy it. Lowered target HP/max HP to 8 to keep the intended destruction and shadow checks valid. |

The Earth Rite scenarios retain the official behavior and account separately for the spell-play shadow and destroyed-Sigil shadow. The other first-run failures were fixture/action encoding or target-health corrections, not simulator regressions. I also audited every Witch spell whose case asserts shadows: spell play adds one, and any destroyed follower or Sigil adds its own shadow. Normal Invasion and Ground Lightning therefore expect two owner shadows (spell plus destroyed permanent); the follower-based Remirami case remains at one for the destroyed Sigil alone.

## Additional coverage

Added independent cases for Kitty Trick's random pair, Ecstasy and Sephie fusion branches, both Sweet Entity modes, Lincle, Insomnia Witch, Noin Interrogation, Lara Anthem, Kaldigensla, Lovely Masterpiece, and Sammy & Marie. These scenarios use official card text and token records; they have not been rebuilt in this audit turn.


