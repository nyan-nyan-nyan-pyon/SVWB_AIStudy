"use strict";

const $ = (id) => document.getElementById(id);
const seatName = {first: "先攻", second: "後攻"};
const kindName = {follower: "フォロワー", amulet: "アミュレット", spell: "スペル"};
const finishReason = {health: "リーダーの体力", empty_deck: "山札切れ", turn_limit: "ターン上限"};
let game = null;
let busy = false;
let statusMessage = "";
let selection = null;
let targetFilter = null;
let mulliganMask = 0;
let analysis = null;
let humanAnalysis = null;
let renderedHumanAnalysis;
let modelLabels = {};
let deckLabels = {};

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function button(label, className, handler) {
  const node = element("button", className, label);
  node.type = "button";
  node.addEventListener("click", handler);
  return node;
}

function announceError(message = "") {
  $("error").textContent = message;
  $("error").hidden = !message;
}

async function api(command, fields) {
  const response = await fetch("/api/command", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({command, debug: $("debug").checked, ...fields}),
  });
  const payload = await response.json();
  if (!response.ok || !payload.ok) {
    throw new Error(payload.error?.message || "サーバーと通信できません。");
  }
  return payload;
}

function setGame(state) {
  if (game?.revision !== state?.revision) {
    selection = null;
    targetFilter = null;
    mulliganMask = 0;
    humanAnalysis = null;
  }
  game = state;
}

function isAiTurn() {
  return game?.phase === "main" && game.active_seat !== game.human_seat;
}

function isHumanTurn() {
  return game?.phase === "main" && game.active_seat === game.human_seat;
}

async function operate(command, fields = {}, autoAI = true) {
  if (busy) return;
  const debugBefore = game?.opponent?.hand !== undefined && game !== null;
  announceError();
  busy = true;
  statusMessage = command === "ai_turn" ? "AIが手順を探索しています…" : "盤面を更新しています…";
  render();
  try {
    const result = await api(command, fields);
    setGame(result.state);
    if (autoAI && command !== "state" && isAiTurn()) {
      statusMessage = "AIが手順を探索しています…";
      render();
      const afterAI = await api("ai_turn", {revision: game.revision});
      setGame(afterAI.state);
    }
  } catch (error) {
    if (command === "state") $("debug").checked = debugBefore;
    announceError(error instanceof TypeError ? "ローカルサーバーに接続できません。" : error.message);
    if (command !== "state") {
      // A reply can be lost after applying an action. Refresh instead of retrying a mutation.
      try {
        const current = await api("state");
        setGame(current.state);
      } catch {
        // The original message is still shown while the server is unavailable.
      }
    }
  } finally {
    busy = false;
    statusMessage = "";
    render();
    refreshHumanAnalysis();
  }
}

async function refreshHumanAnalysis() {
  if (busy || !isHumanTurn() || humanAnalysis) return;
  const position = {revision: game.revision};
  humanAnalysis = position;
  renderHumanAnalysis();
  try {
    const result = await api("analyze_turn", position);
    if (humanAnalysis !== position) return;
    if (result.analysis.revision !== position.revision) {
      throw new Error("局面が更新されました。");
    }
    humanAnalysis = {result: result.analysis};
  } catch (error) {
    if (humanAnalysis !== position) return;
    humanAnalysis = {error: error instanceof TypeError ? "ローカルサーバーに接続できません。" : error.message};
  }
  renderHumanAnalysis();
}

function legal() {
  return game?.legal_actions || [];
}

function sourceActions(zone, slot) {
  return legal().filter((action) => action.source_zone === zone && action.source_slot === slot);
}

function selectedActions() {
  return selection ? sourceActions(selection.zone, selection.slot) : [];
}

function actionsForTarget(kind, slot = null) {
  return selectedActions().filter((action) => action.target_kind === kind && action.target_slot === slot);
}

function selectSource(zone, slot) {
  if (busy) return;
  const toggleOff = selection?.zone === zone && selection.slot === slot;
  selection = toggleOff ? null : {zone, slot};
  targetFilter = null;
  render();
}

function applyAction(action) {
  if (action) operate("act", {action_index: action.action_index, revision: game.revision});
}

function selectTarget(kind, slot = null) {
  const matches = actionsForTarget(kind, slot);
  if (matches.length === 1) {
    applyAction(matches[0]);
  } else if (matches.length > 1) {
    targetFilter = {kind, slot};
    render();
    $("action-options").querySelector("button")?.focus();
  }
}

function renderStats(id, player, isHuman) {
  const zone = $(id);
  zone.replaceChildren();
  const targetable = !isHuman && !busy && actionsForTarget("leader").length > 0;
  const add = (label, value, type = "", handler = null) => {
    const node = handler ? button("", "stat targetable " + type, handler) : element("div", "stat " + type);
    if (handler) {
      node.dataset.focus = "enemy-leader";
      node.setAttribute("aria-label", "相手リーダーを対象に選ぶ");
    }
    node.append(element("span", "", label), element("strong", "", player ? value : "—"));
    zone.append(node);
  };
  add("HP", player && player.hp + "/" + player.max_hp, "hp", targetable ? () => selectTarget("leader") : null);
  add("PP", player && player.pp + "/" + player.max_pp + (player.bonus_pp ? "（追加" + player.bonus_pp + "）" : ""), "pp");
  add("EP", player?.ep);
  add("SEP", player?.sep);
  add("墓場", player?.shadows);
  add("山札", player?.deck_count);
  if (!isHuman) add("手札", player?.hand_count);
  if (player?.seat === "second") {
    const availability = player.extra_pp_available ? "使用可" : "使用不可";
    add("エクストラPP", availability + "（" + player.extra_pp_uses + "回使用済み）");
  }
  if (player?.leader_barrier) add("能力", "バリア");
}

function renderCrests(id, crests) {
  const zone = $(id);
  zone.replaceChildren();
  for (const crest of crests || Array(5).fill(null)) {
    if (!crest) {
      zone.append(element("div", "slot crest empty"));
      continue;
    }
    const item = element("div", "slot crest filled");
    item.append(element("span", "", crest.name), element("b", "", crest.faith ? "信仰" : "CD " + crest.countdown));
    zone.append(item);
  }
}

function renderBoard(id, board, isHuman) {
  const zone = $(id);
  zone.replaceChildren();
  for (const card of board || Array(5).fill(null)) {
    if (!card) {
      zone.append(element("div", "slot empty"));
      continue;
    }
    const actions = isHuman ? sourceActions("board", card.slot) : actionsForTarget("board", card.slot);
    const targetKind = isHuman ? "own_board" : "board";
    const targetable = !busy && actionsForTarget(targetKind, card.slot).length > 0;
    const chosen = isHuman && selection?.zone === "board" && selection.slot === card.slot;
    const classes = ["card", "slot", "board-card", card.kind];
    if (isHuman && actions.length) classes.push("card-legal");
    if (chosen) classes.push("selected");
    if (targetable) classes.push("targetable");
    const item = isHuman || targetable
      ? button("", classes.join(" "), () => targetable ? selectTarget(targetKind, card.slot) : selectSource("board", card.slot))
      : element("div", classes.join(" "));
    item.title = card.text;
    item.dataset.focus = (isHuman ? "board-" : "enemy-board-") + card.slot;
    if (item instanceof HTMLButtonElement) item.disabled = busy;
    if (isHuman) item.setAttribute("aria-pressed", String(chosen));
    item.append(element("strong", "card-name", card.name));
    if (card.kind === "follower") {
      item.append(element("span", "card-combat", card.attack + " / " + card.health));
    }
    const flags = [];
    if (card.countdown !== null) flags.push("CD " + card.countdown);
    if (card.clue_count) flags.push("証拠 " + card.clue_count);
    if (card.earth_count) flags.push("土の印 " + card.earth_count);
    if (card.double_attack) flags.push("2回攻撃");
    if (card.evolution !== "none") flags.push(card.evolution === "super" ? "超進化" : "進化");
    if (card.ward) flags.push("守護");
    if (card.storm) flags.push("疾走");
    if (card.rush) flags.push("突進");
    if (card.silenced) flags.push("沈黙");
    if (card.can_attack) flags.push("攻撃可");
    if (card.can_act) flags.push("アクト可");
    if (flags.length) item.append(element("span", "card-info", flags.join(" · ")));
    if (targetable) item.append(element("span", "card-info target-mark", "対象に選ぶ"));
    zone.append(item);
  }
}

function renderHand() {
  const zone = $("hand");
  const mulligan = game?.phase === "mulligan";
  const hand = game?.human.hand || [];
  const marked = hand.filter((card) => mulliganMask & (1 << card.slot)).length;
  $("hand-title").textContent = mulligan ? "初手の交換" : "手札";
  $("hand-count").textContent = mulligan ? marked + "枚を交換" : game ? hand.length + "枚" : "";
  $("mulligan-help").hidden = !mulligan;
  $("confirm-mulligan").hidden = !mulligan;
  $("confirm-mulligan").disabled = busy;
  $("confirm-mulligan").textContent = mulligan && !marked ? "交換せずに確定" : "マリガン確定";
  zone.replaceChildren();
  for (const card of hand) {
    const usable = !mulligan && sourceActions("hand", card.slot).length > 0;
    const targetable = !busy && actionsForTarget("hand", card.slot).length > 0;
    const selected = mulligan ? Boolean(mulliganMask & (1 << card.slot))
      : selection?.zone === "hand" && selection.slot === card.slot;
    const item = button("", "card hand-card " + card.kind, () => {
      if (mulligan) {
        mulliganMask ^= 1 << card.slot;
        render();
      } else if (targetable) {
        selectTarget("hand", card.slot);
      } else {
        selectSource("hand", card.slot);
      }
    });
    item.dataset.focus = "hand-" + card.slot;
    item.disabled = busy;
    item.title = card.text;
    item.classList.toggle("selected", selected);
    item.classList.toggle("card-legal", usable);
    item.classList.toggle("targetable", targetable);
    item.setAttribute("aria-pressed", String(selected));
    const top = element("span", "card-top");
    top.append(element("span", "card-cost", card.cost), element("span", "card-type", kindName[card.kind]));
    item.append(top, element("strong", "card-name", card.name));
    if (mulligan) {
      item.append(element("span", "card-info" + (selected ? " swap-mark" : ""), selected ? "交換する" : "保持する"));
    } else if (targetable) {
      item.append(element("span", "card-info target-mark", "対象に選ぶ"));
    } else if (card.spellboost) {
      item.append(element("span", "card-info", "スペルブースト " + card.spellboost));
    } else if (usable) {
      item.append(element("span", "card-info source-mark", "使用できる"));
    } else if (card.kind === "follower") {
      item.append(element("span", "card-info", card.attack + " / " + card.health));
    }
    zone.append(item);
  }
}

function renderActionPanel() {
  const list = $("action-options");
  list.replaceChildren();
  const card = selection && game?.human[selection.zone]?.[selection.slot];
  $("action-panel").hidden = !card;
  if (!card) return;
  $("action-title").textContent = card.name;
  const actions = targetFilter ? actionsForTarget(targetFilter.kind, targetFilter.slot) : selectedActions();
  const note = card.silenced ? "能力は失効しています。" : card.text;
  $("action-description").textContent = note + (actions.length ? "" : " 現在このカードで実行できる行動はありません。");
  for (const action of actions) {
    const option = button(action.label, "", () => applyAction(action));
    option.dataset.focus = "action-" + action.action_index;
    option.disabled = busy;
    list.append(option);
  }
}

function evaluation(q) {
  return q === null ? "未訪問" : q.toFixed(3);
}

function actionSteps(actions) {
  const list = element("ol", "");
  for (const action of actions) list.append(element("li", "", action.label));
  return list;
}

function renderCandidates(panel, candidates, selectedLabel) {
  for (const candidate of candidates) {
    const item = element("details", "candidate");
    const share = (candidate.visit_share * 100).toFixed(1) + "%";
    item.append(element("summary", "", candidate.rank + "位" + (candidate.selected ? " " + selectedLabel : "") +
      " · 評価 " + evaluation(candidate.value) + " · 訪問 " + share));
    const bar = element("progress", "");
    bar.max = 1;
    bar.value = candidate.visit_share;
    bar.setAttribute("aria-label", "訪問割合 " + share);
    item.append(bar, actionSteps(candidate.actions));
    panel.append(item);
  }
}

function renderHumanAnalysis() {
  const view = isHumanTurn() ? humanAnalysis : false;
  if (renderedHumanAnalysis === view) return;
  renderedHumanAnalysis = view;
  const panel = $("human-analysis");
  const status = $("human-analysis-status");
  panel.replaceChildren();
  $("human-analysis-turn").textContent = isHumanTurn() ? game.turn + "T · あなた視点" : "";
  $("retry-analysis").hidden = !view?.error;
  status.hidden = Boolean(view?.result);
  if (view?.result) {
    const result = view.result;
    const first = result.actions[0];
    panel.append(element("div", "ai-eval", "探索 " + result.total_visits + "回"));
    if (first) panel.append(element("p", "recommended-action", "推奨の初手　" + first.label));
    renderCandidates(panel, result.candidates, "推奨");
  } else {
    status.textContent = view?.error || (isHumanTurn()
      ? "現在の局面を探索しています…" : "あなたの手番に候補手順が表示されます。");
  }
}

function renderResearch() {
  const history = $("history");
  history.replaceChildren();
  const entries = game?.recent_actions || [];
  for (const action of [...entries].reverse()) {
    const li = element("li", "");
    const who = action.actor === game.human_seat ? "あなた" : "AI";
    const when = action.type === "mulligan" ? "開始時" : action.turn + "T";
    li.append(element("span", "history-label", when + " " + who), document.createTextNode(action.label));
    history.append(li);
  }
  if (!entries.length) history.append(element("li", "hint", "行動履歴はまだありません。"));

  const latest = game?.last_ai_turn || null;
  // Preserve expandable candidate details across changes within the same turn.
  const analysisKey = latest ? latest.revision ?? [latest.turn, latest.actor, game.human_seat,
    latest.actions.map((action) => action.label).join("\n")].join("/") : null;
  if (analysis === analysisKey) return;
  analysis = analysisKey;
  const panel = $("analysis");
  panel.replaceChildren();
  $("analysis-turn").textContent = latest ? latest.turn + "T · " + seatName[latest.actor] : "";
  if (!latest) {
    panel.append(element("p", "hint", "AIの手番が終わると、選択した手順と探索結果が表示されます。"));
    return;
  }
  panel.append(element("div", "ai-eval", "評価値 " + evaluation(latest.value) + " · 探索 " + latest.total_visits + "回"));
  panel.append(actionSteps(latest.actions), element("h3", "candidate-title", "MCTSの上位候補"));
  renderCandidates(panel, latest.candidates, "採用");
}

function render() {
  const focus = document.activeElement?.dataset.focus;
  const human = game?.human;
  const opponent = game?.opponent;
  const mulligan = game?.phase === "mulligan";
  const aiTurn = isAiTurn();
  $("setup").querySelectorAll("button, select").forEach((element) => { element.disabled = busy; });
  $("debug").disabled = busy;
  $("human-seat").textContent = human ? seatName[human.seat] : "";
  $("opponent-seat").textContent = opponent ? seatName[opponent.seat] : "";
  const model = modelLabels[game?.model_id ?? "default"];
  if (model) $("model-name").textContent = model;
  const home = deckLabels[game?.deck_a ?? "default"];
  const away = deckLabels[game?.deck_b ?? "default"];
  if (home && away) $("match-decks").textContent = home + " 対 " + away;
  $("opponent-hand").hidden = !opponent?.hand || !$("debug").checked;
  $("opponent-hand").textContent = opponent?.hand ? "相手手札 ｜ " +
    (opponent.hand.map((card) => card.cost + " " + card.name).join(" / ") || "0枚") : "";
  renderStats("opponent-stats", opponent, false);
  renderStats("human-stats", human, true);
  renderCrests("opponent-crests", opponent?.crests);
  renderCrests("human-crests", human?.crests);
  renderBoard("opponent-board", opponent?.board, false);
  renderBoard("human-board", human?.board, true);
  renderHand();
  renderActionPanel();
  renderHumanAnalysis();
  renderResearch();

  $("turn-number").textContent = !game ? "対局前" : mulligan ? "マリガン" : game.turn + "ターン目";
  let status = "新規対局を開始してください。";
  if (mulligan) {
    status = "あなたは" + seatName[game.human_seat] + "です。";
  } else if (game?.finished) {
    const result = game.both_lose ? "両者敗北" : game.winner === game.human_seat ? "あなたの勝利" : "対戦AIの勝利";
    status = result + " · " + finishReason[game.end_reason];
  } else if (aiTurn) {
    status = "対戦AIの手番";
  } else if (game) {
    status = selectedActions().some((action) => action.target_kind !== "none")
      ? "対象を選ぶか、手順リストから行動を実行してください。" : "あなたの手番";
  }
  $("status").textContent = statusMessage || status;
  $("retry-ai").hidden = busy || !aiTurn;
  $("extra-pp").disabled = busy || !legal().some((action) => action.type === "extra_pp");
  $("end-turn").disabled = busy || !legal().some((action) => action.type === "end_turn");
  if (focus) document.querySelector('[data-focus="' + focus + '"]:not(:disabled)')?.focus({preventScroll: true});
}

$("setup").addEventListener("submit", (event) => {
  event.preventDefault();
  operate("new_game", {seat: $("seat").value, simulations: Number($("simulations").value),
    deck_a: $("deck-a").value, deck_b: $("deck-b").value, model_id: $("model").value});
});
$("confirm-mulligan").addEventListener("click", () => operate("mulligan", {mask: mulliganMask, revision: game.revision}));
$("extra-pp").addEventListener("click", () => applyAction(legal().find((action) => action.type === "extra_pp")));
$("end-turn").addEventListener("click", () => applyAction(legal().find((action) => action.type === "end_turn")));
$("retry-ai").addEventListener("click", () => operate("ai_turn", {revision: game.revision}));
$("retry-analysis").addEventListener("click", () => {
  humanAnalysis = null;
  operate("state", {}, false);
});
$("debug").addEventListener("change", () => operate("state", {}, false));
$("clear-selection").addEventListener("click", () => {
  const previous = selection;
  selection = null;
  targetFilter = null;
  render();
  if (previous) document.querySelector('[data-focus="' + previous.zone + '-' + previous.slot + '"]')?.focus();
});

async function initialize() {
  render();
  const current = operate("state", {}, false);
  try {
    const response = await fetch("/api/config");
    if (!response.ok) return;
    const config = await response.json();
    const showChoices = (id, options, value) => {
      if (!options?.length) return {};
      $(id).replaceChildren(...options.map((option) => new Option(option.label, option.id)));
      $(id).value = value ?? "default";
      return Object.fromEntries(options.map((option) => [option.id, option.label]));
    };
    await current;
    deckLabels = showChoices("deck-a", config.decks, game?.deck_a);
    showChoices("deck-b", config.decks, game?.deck_b);
    modelLabels = showChoices("model", config.models, game?.model_id);
    $("model-name").textContent = config.model;
    const control = $("simulations");
    if (!Array.from(control.options).some((option) => Number(option.value) === config.simulations)) {
      control.add(new Option(String(config.simulations), String(config.simulations)));
    }
    control.value = String(config.simulations);
    render();
  } catch {
    // The game-state request reports connection errors; display defaults are sufficient here.
  } finally {
    await current;
  }
}

initialize();
