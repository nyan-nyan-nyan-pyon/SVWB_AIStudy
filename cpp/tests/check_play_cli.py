import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile


def check_state(state, debug=False):
    assert state["human"]["seat"] == state["human_seat"] != state["opponent"]["seat"]
    assert ("hand" in state["opponent"]) == debug
    assert len(state["human"]["hand"]) == state["human"]["hand_count"]
    if debug:
        assert len(state["opponent"]["hand"]) == state["opponent"]["hand_count"]
    for player in (state["human"], state["opponent"]):
        assert len(player["board"]) == len(player["crests"]) == 5
        assert player["max_hp"] == 20 and 0 <= player["deck_count"] <= 40
        assert {"pp", "max_pp", "extra_pp_available", "extra_pp_uses", "ep", "sep", "shadows"} <= player.keys()
    actions = state["legal_actions"]
    assert [action["action_index"] for action in actions] == list(range(len(actions)))
    for action in actions:
        assert action["label"] and {"type", "source_zone", "source_slot", "target_kind", "target_slot", "mode", "form"} <= action.keys()


def exercise(process):
    def line(raw, ok=True):
        process.stdin.write(raw + "\n")
        process.stdin.flush()
        reply = json.loads(process.stdout.readline())
        assert reply["ok"] == ok, reply
        return reply

    def send(command, ok=True, **fields):
        return line(json.dumps({"command": command, **fields}), ok)

    def act(state, action):
        changed = send("act", revision=state["revision"], action_index=action["action_index"])["state"]
        assert changed["revision"] == state["revision"] + 1
        assert changed["recent_actions"][-1]["label"] == action["label"]
        return changed

    assert send("state")["state"] is None
    line('{"command":}', ok=False)
    line('{"command":"state","command":"act"}', ok=False)
    send("act", ok=False, action_index=0)

    # Identical seeds must give identical opening hands. Find a first-cost card to test real plays as well.
    for seed in range(1, 20):
        start = send("new_game", seat="first", seed=seed)["state"]
        if any(card["cost"] == 1 or card["crystal_cost"] == 1 for card in start["human"]["hand"]):
            break
    else:
        raise AssertionError("No one-PP card in the seeded opening hands")
    check_state(start)
    assert start["phase"] == "mulligan" and not start["legal_actions"]
    assert len(start["human"]["hand"]) == 4 and start["human"]["deck_count"] == 36
    send("analyze_turn", ok=False)
    assert send("new_game", seat="first", seed=seed)["state"]["human"]["hand"] == start["human"]["hand"]
    send("mulligan", ok=False, mask=16)
    state = send("mulligan", mask=0)["state"]
    check_state(state)
    assert state["phase"] == "main" and state["turn"] == 1 and state["active_seat"] == "first"
    assert len(state["human"]["hand"]) == 5 and state["human"]["pp"] == 1
    assert [item["actor"] for item in state["recent_actions"]] == ["first", "second"]
    send("ai_turn", ok=False)
    send("act", ok=False, action_index=len(state["legal_actions"]))
    send("act", ok=False, action_index=0, revision=state["revision"] - 1)
    send("analyze_turn", ok=False, revision=state["revision"] - 1)
    hints = send("analyze_turn", revision=state["revision"])
    assert hints["state"] == state
    advice = hints["analysis"]
    assert advice["actor"] == state["human_seat"] and advice["revision"] == state["revision"]
    assert advice["total_visits"] == 8
    assert next(item for item in advice["candidates"] if item["selected"])["actions"] == advice["actions"]
    for candidate in advice["candidates"]:
        first = state["legal_actions"][candidate["first_action_index"]]
        assert candidate["actions"][0]["label"] == first["label"]
        assert math.isclose(candidate["visit_share"], candidate["visits"] / advice["total_visits"])
        assert candidate["value"] is None or 0 <= candidate["value"] <= 1
    assert any(candidate["actions"][0]["type"] == "play" for candidate in advice["candidates"])
    assert send("state")["state"] == state
    play = next(action for action in state["legal_actions"] if action["type"] == "play")
    played = act(state, play)
    assert len(played["human"]["hand"]) == 4 and played["human"]["pp"] == 0
    permanent = played["human"]["board"][0]
    assert permanent["kind"] == "amulet" and permanent["countdown"] in (3, 4)
    updated = send("analyze_turn", revision=played["revision"])
    assert updated["state"] == played and updated["analysis"]["revision"] == played["revision"]
    assert all(candidate["actions"][0]["type"] == "end_turn" for candidate in updated["analysis"]["candidates"])
    ended = act(played, next(action for action in played["legal_actions"] if action["type"] == "end_turn"))
    assert ended["active_seat"] == "second" and not ended["legal_actions"]
    send("analyze_turn", ok=False, revision=ended["revision"])

    response = send("ai_turn", revision=ended["revision"])
    state = response["state"]
    check_state(state)
    assert state["active_seat"] == "first" and state["turn"] == 2
    trace = response["ai_turn"]
    assert trace == state["last_ai_turn"] and trace["actions"] and trace["total_visits"] == 8
    selected = next(candidate for candidate in trace["candidates"] if candidate["selected"])
    assert selected["rank"] == trace["selected_rank"] and selected["actions"] == trace["actions"]
    for candidate in trace["candidates"]:
        assert math.isclose(candidate["visit_share"], candidate["visits"] / trace["total_visits"])
        assert candidate["value"] is None or 0 <= candidate["value"] <= 1
        for action in candidate["actions"]:
            if not candidate["selected"] and action["type"] == "play":
                assert action["card_id"] is None and action["source_slot"] is None
    check_state(send("state", debug=True)["state"], debug=True)
    check_state(send("state")["state"])

    start = send("new_game", seat="second")["state"]
    assert start["human_seat"] == "second" and not start["last_ai_turn"]
    state = send("mulligan", mask=15)["state"]
    assert state["active_seat"] == "first" and len(state["human"]["hand"]) == 4
    assert state["recent_actions"][1]["label"].endswith("4枚交換")
    response = send("ai_turn")
    assert response["state"]["active_seat"] == "second" and response["ai_turn"]["actor"] == "first"
    extra = next(action for action in response["state"]["legal_actions"] if action["type"] == "extra_pp")
    state = act(response["state"], extra)
    assert state["human"]["extra_pp_uses"] == 1 and state["human"]["bonus_pp"] == 1
    check_state(send("new_game", seat="random")["state"])

    monk = Path(__file__).resolve().parents[2] / "data/decks/bishop-monk.json"
    for seed in range(1, 25):
        custom = send("new_game", seat="second", seed=seed, deck_a=str(monk), debug=True)["state"]
        assert all(card["card_id"] != 131 for card in custom["opponent"]["hand"])
        if any(card["card_id"] == 131 for card in custom["human"]["hand"]):
            break
    else:
        raise AssertionError("Custom player deck did not reach the opening hand")
    response = send("mulligan", mask=0)
    assert response["state"]["active_seat"] == "first"
    assert send("ai_turn")["state"]["active_seat"] == "second"


def main():
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="svwb-play-") as scratch:
        weights = Path(scratch) / "weights.bin"
        weights.write_bytes(bytes(74376))
        with subprocess.Popen([exe, "--weights", weights, "--simulations", "8", "--seed", "27"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8") as process:
            try:
                exercise(process)
            finally:
                process.stdin.close()
            assert process.wait(timeout=5) == 0
    print("PASS JSON Lines game, mulligan, play, read-only human analysis, AI trace, hidden hand and extra PP")


if __name__ == "__main__":
    main()
