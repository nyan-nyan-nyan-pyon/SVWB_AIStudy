#include "play_json.hpp"
#include "wb_search.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <span>

namespace svwb::play {
namespace {

using namespace wb;
using wb::Action;
using wb::ActionList;
using wb::ActionType;
using wb::GameState;
using Object = Json::Object;
using Array = Json::Array;

constexpr int kDefaultSimulations = 64;
constexpr int kMaxSimulations = 256;
constexpr std::uint64_t kMaxSeed = 9007199254740991ULL; // Exactly representable in browser JSON.
constexpr std::size_t kHistoryLimit = 40;

enum class TraceVisibility { Full, OpponentCandidate, HumanCandidate };

template<class T>
T field(const Object& request, std::string_view key, T fallback) {
    const auto item = request.find(key);
    if (item == request.end()) {
        return fallback;
    }
    if (const auto* value = std::get_if<T>(&item->second.value)) {
        return *value;
    }
    throw std::invalid_argument(std::string(key) + " の型が正しくありません。");
}

std::uint64_t number(const Object& request, std::string_view key, std::uint64_t fallback, std::uint64_t max) {
    const auto value = field(request, key, fallback);
    if (value > max) {
        throw std::invalid_argument(std::string(key) + " が指定範囲外です。");
    }
    return value;
}

const char* seat_name(int seat) { return seat == 0 ? "first" : "second"; }

const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Follower: return "follower";
    case Kind::Amulet: return "amulet";
    case Kind::Spell: return "spell";
    case Kind::Faith: return "faith";
    }
    throw std::logic_error("Unknown card kind");
}

std::string_view card_text(CardId id) {
    constexpr std::string_view texts[]{
        "CD4。ラストワードでホーリーファルコンを召喚。アクト1でCD−1。",
        "ラストワードで1枚ドロー。進化時に相手フォロワー1体の全能力を失わせ、2ダメージ。",
        "突進。ファンファーレで神域の物証1枚を手札に加える。",
        "ラストワードで神域の物証2枚を手札に加える。",
        "守護。進化後フォロワーが自分の場にいればファンファーレで進化。進化したとき、ランダムな相手フォロワー1体への1ダメージを2回。",
        "疾走。結晶1でCD3のアミュレット。アクト1でCD−1、ラストワードでこのフォロワーを召喚。",
        "ファンファーレ・進化時に相手フォロワー1体へ2ダメージ。神域の物証1枚を手札に加える。",
        "ファンファーレで地害の兎1枚を手札に加える。進化時にPPを1回復。",
        "ランダムな相手フォロワーに4ダメージを与えてクレストを付与するか、地害の兎1体を召喚。",
        "CD2。ラストワードで壮麗なる隼を召喚。アクト2でCD−2。",
        "相手フォロワー全体に3ダメージ・相手リーダーに3ダメージ・自リーダーを3回復のいずれかを選び、自身に3ダメージ。進化時にクレストを付与。",
        "地害の兎2体を召喚。エンハンス8で4体。",
        "ファンファーレで相手フォロワー1体を消滅させ、地害の兎1体を召喚。進化時にクレスト付与。超進化時はそのCD＋1。",
        "ファンファーレで癒しのシスター1体を召喚し、＋2/−2して疾走を付与。",
        "場に出たとき、最低コストの手札1枚をランダムに消滅させて進化。ラストワードで相手リーダーに2ダメージ。",
        "証拠。アクト1で1枚ドローし、証拠−1。0で破壊。",
        "疾走。",
        "疾走。",
        "守護。ファンファーレで自リーダーを5回復。",
        "CD3。ラストワードでミラクルアルミラージを召喚。アクト1でCD−1。",
        "ファンファーレで相手フォロワー1体を消滅させ、地害の兎2体を召喚。超進化時に2枚ドローし、自リーダーを2回復。",
    };
    static_assert(std::size(texts) == kCardSlots);
    return id >= kFirstCard && id <= kLastForm ? texts[id - kFirstCard] : find_card(id)->text;
}

std::string_view crest_name(CrestId id) {
    switch (id) {
    case CrestId::Testimony: return "不毛の証言";
    case CrestId::Rituel: return "リテュエル";
    case CrestId::Rapid: return "ラピッドハット";
    case CrestId::None: return "";
    default:
        if (const auto* crest = find_crest(id)) return crest->name;
        return "クレスト";
    }
}

std::string_view mode_name(CardId id, int mode) {
    if (id == kTestimony) {
        return mode == 1 ? "ランダムな相手フォロワーに4ダメージ＋クレスト付与" : "地害の兎を1体召喚";
    }
    if (id == kRituel) {
        constexpr std::string_view modes[]{"相手フォロワー全体に3ダメージ", "相手リーダーに3ダメージ", "自リーダーを3回復"};
        return modes[mode - 1];
    }
    if (mode == kAllModes) return "すべてのモード";
    const auto* card = find_card(id);
    if (card && mode >= 1 && mode <= card->modes) {
        const std::string_view markers[]{"（1）", "（2）", "（3）", "（4）"};
        const auto start = card->text.find(markers[mode - 1]);
        if (start != std::string_view::npos) {
            auto line = card->text.substr(start + markers[mode - 1].size());
            return line.substr(0, line.find('\n'));
        }
    }
    return {};
}

Object describe_action(const GameState& state, Action action, std::optional<EntityId> public_before = {}) {
    constexpr std::string_view types[]{"end_turn", "play", "evolve", "super_evolve", "attack", "act", "extra_pp", "mulligan", "fuse"};
    const int seat = state.turn_player;
    const auto& player = state.players[seat];
    Object result{{"actor", seat_name(seat)}, {"turn", state.turn_number},
                  {"type", types[static_cast<int>(action.type)]}, {"source_zone", "none"}, {"source_slot", Json{}},
                  {"target_kind", "none"}, {"target_slot", Json{}}, {"mode", action.choice}, {"form", Json{}},
                  {"card_id", Json{}}, {"cost", Json{}}};
    std::string label;
    switch (action.type) {
    case ActionType::EndTurn:
        label = "ターン終了";
        break;
    case ActionType::ExtraPp:
        label = "エクストラPPを使う";
        break;
    case ActionType::Mulligan:
        label = "マリガンで" + std::to_string(std::popcount(action.choice)) + "枚交換";
        result["mode"] = 0;
        break;
    case ActionType::Play: {
        result["source_zone"] = "hand";
        if (public_before) {
            result["mode"] = 0;
            label = "非公開カードをプレイ";
            break;
        }
        result["source_slot"] = action.src;
        const auto& card = *find_card(player.hand[action.src]);
        const auto price = card_cost(card, player.hand_modifiers[action.src]);
        const int enhanced_price = card.kind == Kind::Follower && follower_fanfare_suppressed(player)
            ? 0 : enhance_payment(card, player.available_pp());
        const bool enhance = enhanced_price != 0;
        const bool alternate = !enhance && price > player.available_pp();
        const bool crystal = alternate && card.crystal_form;
        const bool accelerate = alternate && card.accelerate_form && !crystal;
        const int cost = enhance ? enhanced_price : crystal ? find_card(card.crystal_form)->cost :
            accelerate ? find_card(card.accelerate_form)->cost : price;
        result["form"] = crystal ? "crystal" : accelerate ? "accelerate" : enhance ? "enhance" : "normal";
        result["card_id"] = card.id;
        result["cost"] = cost;
        const std::string_view verb = crystal ? "結晶" : accelerate ? "アクセラレート" : enhance ? "エンハンス" : "プレイ";
        label = std::string(verb) + " " + std::to_string(cost) + "PP｜" + std::string(card.name);
        if (action.choice & kModeMask) {
            Array modes;
            for (int mode = 1; mode <= card.modes; ++mode) {
                if (!(action.choice & (1 << (mode - 1)))) continue;
                modes.emplace_back(mode);
                label += " · ";
                label += mode_name(card.id, mode);
            }
            result["modes"] = std::move(modes);
        } else if (action.choice && !mode_name(card.id, action.choice).empty()) {
            label += " · ";
            label += mode_name(card.id, action.choice);
        }
        break;
    }
    case ActionType::Fuse: {
        result["source_zone"] = "hand";
        result["mode"] = 0;
        if (public_before) {
            label = "非公開カードを融合";
            break;
        }
        const auto& card = *find_card(player.hand[action.src]);
        result["source_slot"] = action.src;
        result["card_id"] = card.id;
        label = "融合｜" + std::string(card.name) + " ← ";
        Array materials;
        for (int slot = 0; slot < player.hand_count; ++slot) {
            if (slot == action.src || !(action.choice & (1 << (slot - (slot > action.src))))) continue;
            if (!materials.empty()) label += " ＋ ";
            label += find_card(player.hand[slot])->name;
            label += "（手札 " + std::to_string(slot + 1) + "）";
            materials.emplace_back(slot);
        }
        result["material_slots"] = std::move(materials);
        break;
    }
    default: {
        const auto& object = player.board[action.src];
        const auto& card = *find_card(object.card_id);
        result["source_zone"] = "board";
        const bool visible = !public_before || object.creation_order < *public_before;
        const auto name = visible ? card.name : "非公開カード由来の盤面カード";
        if (visible) {
            result["source_slot"] = action.src;
            result["card_id"] = card.id;
        }
        switch (action.type) {
        case ActionType::Evolve: label = "進化｜"; break;
        case ActionType::SuperEvolve: label = "超進化｜"; break;
        case ActionType::Attack: label = "攻撃｜"; break;
        case ActionType::Act:
            label = "アクト｜";
            if (visible) {
                result["cost"] = card.act_cost;
                label = "アクト " + std::to_string(card.act_cost) + "PP｜";
            }
            break;
        default: throw std::logic_error("Unknown board action");
        }
        label += name;
        if (visible && action.choice) {
            const auto mode = mode_name(card.id, action.choice);
            if (!mode.empty()) label += " · " + std::string(mode);
        }
        break;
    }
    }
    if (action.dst == kEnemyLeader) {
        result["target_kind"] = "leader";
        label += " → 相手リーダー";
    } else if (action.dst >= 0) {
        const bool hand = action.dst >= kOwnHandTarget;
        const bool own = action.dst >= kOwnBoardTarget;
        const int slot = action.dst - (hand ? kOwnHandTarget : own ? kOwnBoardTarget : 0);
        result["target_kind"] = hand ? "hand" : own ? "own_board" : "board";
        result["target_slot"] = hand && public_before ? Json{} : Json(slot);
        label += " → ";
        if (hand && public_before) {
            label += "非公開の手札";
        } else {
            const auto id = hand ? player.hand[slot] : state.players[own ? seat : 1 - seat].board[slot].card_id;
            label += find_card(id)->name;
            label += std::string(hand ? "（手札 " : "（場 ") + std::to_string(slot + 1) + "）";
        }
    }
    if (action.selection && !(public_before && action.type == ActionType::Play)) {
        const bool hand = !action.selection_on_enemy && (action.selection_in_hand || action.type != ActionType::Play);
        const auto& board_side = action.selection_on_enemy ? state.players[1 - seat] : player;
        Array slots;
        label += " → ";
        if (hand && public_before) {
            label += "非公開の手札 " + std::to_string(std::popcount(action.selection)) + "枚";
        } else {
            for (int slot = 0; slot < (hand ? player.hand_count : board_side.board_count); ++slot) {
                if (!(action.selection & (1 << slot))) continue;
                if (!slots.empty()) label += " ＋ ";
                const auto id = hand ? player.hand[slot] : board_side.board[slot].card_id;
                label += find_card(id)->name;
                label += std::string(hand ? "（手札 " : "（場 ") + std::to_string(slot + 1) + "）";
                slots.emplace_back(slot);
            }
        }
        result["target_kind"] = hand ? "hand" : action.selection_on_enemy ? "board" : "own_board";
        result["target_slots"] = std::move(slots);
    }
    if (action.secondary_slot >= 0) {
        if (public_before) {
            label += "／手札1枚を捨てる";
        } else {
            result["discard_slot"] = action.secondary_slot;
            label += "／捨てる手札 ";
            label += find_card(player.hand[action.secondary_slot])->name;
            label += "（手札 " + std::to_string(action.secondary_slot + 1) + "）";
        }
    }
    result["label"] = std::move(label);
    return result;
}

Object hand_card(CardId id, int slot, CardModifiers modifiers) {
    const auto& card = *find_card(id);
    return {{"slot", slot}, {"card_id", id}, {"name", card.name}, {"kind", kind_name(card.kind)},
            {"cost", card_cost(card, modifiers)}, {"text", card_text(id)},
            {"attack", card.atk + modifiers.attack}, {"health", card.hp + modifiers.defense},
            {"spellboost", modifiers.spellboost}, {"fused_materials", modifiers.fused_materials},
            {"crystal_cost", card.crystal_form ? Json(find_card(card.crystal_form)->cost) : Json{}},
            {"accelerate_cost", card.accelerate_form ? Json(find_card(card.accelerate_form)->cost) : Json{}},
            {"enhance_cost", card.enhance_cost ? Json(card.enhance_cost) : Json{}}};
}

Object player_json(const GameState& state, int seat, bool show_hand, const ActionList& legal) {
    const auto& player = state.players[seat];
    std::array<bool, kBoardLimit> attacks{}, acts{};
    if (state.phase == Phase::Main && state.turn_player == seat) {
        for (const auto& action : legal) {
            if (action.type == ActionType::Attack) {
                attacks[action.src] = true;
            } else if (action.type == ActionType::Act) {
                acts[action.src] = true;
            }
        }
    }
    Array board, crests;
    for (int slot = 0; slot < kBoardLimit; ++slot) {
        if (slot >= player.board_count) {
            board.emplace_back();
            continue;
        }
        const auto& object = player.board[slot];
        const auto& card = *find_card(object.card_id);
        board.emplace_back(Object{
            {"slot", slot}, {"entity_id", object.creation_order}, {"card_id", card.id}, {"name", card.name},
            {"kind", kind_name(card.kind)}, {"cost", card.cost}, {"text", card_text(card.id)}, {"attack", object.atk},
            {"health", object.hp}, {"max_health", object.max_hp},
            {"evolution", object.evolution == Evolution::Super ? "super" :
                          object.evolution == Evolution::Normal ? "normal" : "none"},
            {"ward", (object.abilities & FLAG_WARD) != 0}, {"storm", (object.abilities & FLAG_STORM) != 0},
            {"rush", (object.abilities & FLAG_RUSH) != 0}, {"silenced", object.silenced},
            {"can_attack", attacks[slot]}, {"can_act", acts[slot]}, {"readied", object.readied},
            {"attacked_this_turn", object.attacked_this_turn}, {"acted_this_turn", object.acted_this_turn},
            {"countdown", card.countdown > 0 ? Json(object.countdown) : Json{}}, {"clue_count", object.clue_count},
            {"earth_count", object.earth_count}, {"double_attack", (object.abilities & kDoubleStrike) != 0},
            {"act_cost", card.act_cost >= 0 ? Json(card.act_cost) : Json{}},
        });
    }
    for (int slot = 0; slot < kCrestLimit; ++slot) {
        if (slot >= player.crest_count + player.faith_count) {
            crests.emplace_back();
        } else if (slot >= player.crest_count) {
            const auto faith = player.faiths[slot - player.crest_count];
            const int index = slot - player.crest_count;
            crests.emplace_back(Object{{"name", find_card(faith)->name}, {"faith", true}, {"countdown", Json{}},
                                       {"faith_value", player.faith_values[index]}, {"faith_upgrades", player.faith_upgrades[index]}});
        } else {
            const auto& crest = player.crests[slot];
            crests.emplace_back(Object{{"name", crest_name(crest.id)},
                                       {"countdown", crest.countdown ? Json(crest.countdown) : Json{}}});
        }
    }
    Object result{
        {"seat", seat_name(seat)}, {"hp", player.leader_hp}, {"max_hp", player.leader_max_hp},
        {"pp", player.available_pp()}, {"regular_pp", player.pp}, {"max_pp", player.max_pp},
        {"bonus_pp", player.bonus_pp}, {"extra_pp_available", player.extra_pp_available},
        {"extra_pp_uses", player.extra_pp_uses}, {"ep", player.ep}, {"sep", player.sep},
        {"evolved_this_turn", player.manual_evolved}, {"leader_barrier", player.leader_barrier},
        {"shadows", player.shadows}, {"deck_count", player.deck_remaining()}, {"hand_count", player.hand_count},
        {"combo", player.plays_this_turn}, {"rally", player.followers_entered}, {"crest_count", player.crest_count},
        {"board", std::move(board)}, {"crests", std::move(crests)},
    };
    if (show_hand) {
        Array hand;
        for (int slot = 0; slot < player.hand_count; ++slot) {
            hand.emplace_back(hand_card(player.hand[slot], slot, player.hand_modifiers[slot]));
        }
        result["hand"] = std::move(hand);
    }
    return result;
}

class Session {
public:
    Session(const Mlp& model, int simulations, std::optional<std::uint64_t> seed)
        : model_(model), default_simulations_(simulations), default_seed_(seed) {}

    Object handle(const Object& request) {
        const auto command = field(request, "command", std::string{});
        const bool debug = field(request, "debug", false);
        Object response{{"ok", true}};
        if (command == "new_game") {
            start(request);
        } else if (command != "state") {
            if (!state_) {
                throw std::invalid_argument("先に新規対局を開始してください。");
            }
            const auto expected = field(request, "revision", revision_);
            if (expected != revision_) {
                throw std::invalid_argument("盤面が更新されました。現在の盤面から操作を選び直してください。");
            }
            if (command == "mulligan") {
                mulligan(request);
            } else if (command == "act") {
                act(request);
            } else if (command == "ai_turn") {
                response["ai_turn"] = ai_turn();
            } else if (command == "analyze_turn") {
                response["analysis"] = analyze_turn();
            } else {
                throw std::invalid_argument("不明なコマンドです。");
            }
        }
        response["state"] = snapshot(debug);
        return response;
    }

private:
    void start(const Object& request) {
        const auto seat = field(request, "seat", std::string("random"));
        if (seat != "first" && seat != "second" && seat != "random") {
            throw std::invalid_argument("先攻・後攻・ランダムのいずれかを指定してください。");
        }
        const auto simulations = number(request, "simulations", default_simulations_, kMaxSimulations);
        if (simulations == 0) {
            throw std::invalid_argument("シミュレーション数は1以上を指定してください。");
        }
        const auto deck_a_path = field(request, "deck_a", std::string{});
        const auto deck_b_path = field(request, "deck_b", std::string{});
        const auto load_deck = [](const std::string& path) {
            return path.empty() ? kMirrorDeck : read_deck(std::filesystem::path(std::u8string(path.begin(), path.end())));
        };
        const auto deck_a = load_deck(deck_a_path);
        const auto deck_b = load_deck(deck_b_path);
        auto matchup = MatchupFeatures(deck_a, deck_b);
        const auto seed = number(request, "seed", default_seed_.value_or(0), kMaxSeed);
        auto game_seed = seed;
        if (!default_seed_ && !request.contains("seed")) {
            std::random_device entropy;
            game_seed = (static_cast<std::uint64_t>(entropy() & 0x1fffff) << 32) | entropy();
        }
        std::seed_seq search_seed{static_cast<std::uint32_t>(game_seed), static_cast<std::uint32_t>(game_seed >> 32)};
        std::mt19937 rng(search_seed);
        const int random_seat = std::uniform_int_distribution<int>(0, 1)(rng);
        const int human = seat == "random" ? random_seat : seat == "first" ? 0 : 1;
        const auto next = human == 0 ? new_game(deck_a, deck_b, game_seed) : new_game(deck_b, deck_a, game_seed);
        rng_ = rng;
        human_ = human;
        simulations_ = static_cast<int>(simulations);
        state_ = next;
        matchup_ = std::move(matchup);
        history_.clear();
        last_ai_turn_ = {};
        ++revision_;
    }

    void append(Json entry) {
        if (history_.size() == kHistoryLimit) {
            history_.erase(history_.begin());
        }
        history_.push_back(std::move(entry));
    }

    void mulligan(const Object& request) {
        if (state_->phase != Phase::Mulligan) {
            throw std::invalid_argument("マリガンはすでに確定しています。");
        }
        const auto mask = number(request, "mask", 16, 15);
        for (int seat = 0; seat < kPlayers; ++seat) {
            const auto action = seat == human_
                ? Action{ActionType::Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(mask)}
                : choose_mulligan(*state_, model_, rng_, &matchup_);
            auto entry = describe_action(*state_, action);
            apply_legal_action(*state_, action);
            append(std::move(entry));
        }
        ++revision_;
    }

    void act(const Object& request) {
        require_turn(human_);
        const auto actions = legal_actions(*state_);
        const auto index = number(request, "action_index", actions.size(), actions.size() - 1);
        const auto action = actions[static_cast<int>(index)];
        auto entry = describe_action(*state_, action);
        apply_legal_action(*state_, action);
        append(std::move(entry));
        ++revision_;
    }

    void require_turn(int seat) const {
        if (state_->phase != Phase::Main || state_->outcome != Outcome::Ongoing || state_->turn_player != seat) {
            throw std::invalid_argument(seat == human_ ? "あなたの手番に実行してください。" : "AIの手番に実行してください。");
        }
    }

    static Array replay(GameState& state, std::span<const Action> actions, TraceVisibility visibility) {
        Array entries;
        const int actor = state.turn_player;
        const auto initial_draw_index = state.players[actor].deck_idx;
        std::optional<EntityId> public_before;
        if (visibility == TraceVisibility::OpponentCandidate) {
            public_before = state.next_creation_order;
        }
        for (const auto& action : actions) {
            const auto legal = legal_actions(state);
            if (std::find(legal.begin(), legal.end(), action) == legal.end()) {
                throw std::logic_error("MCTS returned an illegal step");
            }
            entries.emplace_back(describe_action(state, action, public_before));
            const auto next_creation_order = state.next_creation_order;
            apply_legal_action(state, action);
            if (visibility == TraceVisibility::HumanCandidate && !public_before &&
                state.players[actor].deck_idx != initial_draw_index) {
                // Subsequent plays could use a card that the human has yet to draw.
                public_before = next_creation_order;
            }
        }
        return entries;
    }

    Object describe_turn(const Decision& decision, bool for_human) const {
        const auto selected = std::find_if(decision.ranked.begin(), decision.ranked.end(),
                                           [](const RankedTurn& item) { return item.selected; });
        if (selected == decision.ranked.end()) {
            throw std::logic_error("MCTS returned no selected plan");
        }
        auto played = *state_;
        auto actions = replay(played, selected->actions,
                              for_human ? TraceVisibility::HumanCandidate : TraceVisibility::Full);
        if (played != decision.next) {
            throw std::logic_error("MCTS trace does not reproduce the selected position");
        }
        Array candidates;
        const auto legal = legal_actions(*state_);
        const auto selection_rank = static_cast<int>(selected - decision.ranked.begin()) + 1;
        for (int rank = 1; rank <= static_cast<int>(decision.ranked.size()); ++rank) {
            const auto& item = decision.ranked[rank - 1];
            if (rank > 5 && !item.selected) {
                continue;
            }
            auto alternative = *state_;
            auto plan = item.selected ? actions : replay(alternative, item.actions,
                for_human ? TraceVisibility::HumanCandidate : TraceVisibility::OpponentCandidate);
            Object candidate{{"rank", rank}, {"selected", item.selected}, {"visits", item.visits},
                {"visit_share", decision.visits > 0 ? static_cast<double>(item.visits) / decision.visits : 0.0},
                {"value", item.value ? Json(static_cast<double>(*item.value)) : Json{}}, {"actions", std::move(plan)}};
            if (for_human) {
                if (item.actions.empty()) {
                    throw std::logic_error("MCTS returned an empty plan");
                }
                const auto first = std::find(legal.begin(), legal.end(), item.actions.front());
                candidate["first_action_index"] = static_cast<int>(first - legal.begin());
            }
            candidates.emplace_back(std::move(candidate));
        }
        return {{"actor", seat_name(state_->turn_player)}, {"turn", state_->turn_number}, {"revision", revision_},
                {"total_visits", decision.visits}, {"selected_rank", selection_rank},
                {"value", selected->value ? Json(static_cast<double>(*selected->value)) : Json{}},
                {"actions", std::move(actions)}, {"candidates", std::move(candidates)}};
    }

    Json analyze_turn() const {
        require_turn(human_);
        auto analysis_rng = rng_;
        Search search(model_, model_, analysis_rng, SearchOptions{simulations_, false, true}, &matchup_);
        return describe_turn(search.choose(*state_, false), true);
    }

    Json ai_turn() {
        require_turn(1 - human_);
        Search search(model_, model_, rng_, SearchOptions{simulations_, false, true}, &matchup_);
        const auto decision = search.choose(*state_, false);
        auto trace = describe_turn(decision, false);
        for (const auto& entry : std::get<Array>(trace.at("actions").value)) {
            append(entry);
        }
        *state_ = decision.next;
        last_ai_turn_ = std::move(trace);
        ++revision_;
        return last_ai_turn_;
    }

    Json snapshot(bool debug) const {
        if (!state_) {
            return {};
        }
        const auto legal = legal_actions(*state_);
        Array actions;
        if (state_->phase == Phase::Main && state_->turn_player == human_) {
            for (int i = 0; i < legal.size(); ++i) {
                auto action = describe_action(*state_, legal[i]);
                action["action_index"] = i;
                actions.emplace_back(std::move(action));
            }
        }
        const auto outcome = state_->outcome;
        const char* reason = nullptr;
        switch (state_->end_reason) {
        case EndReason::Health: reason = "health"; break;
        case EndReason::EmptyDeck: reason = "empty_deck"; break;
        case EndReason::TurnLimit: reason = "turn_limit"; break;
        case EndReason::None: break;
        }
        const bool done = outcome != Outcome::Ongoing;
        const auto phase = done ? "finished" : state_->phase == Phase::Mulligan ? "mulligan" : "main";
        const char* winner = outcome == Outcome::FirstWins ? "first" : outcome == Outcome::SecondWins ? "second" : nullptr;
        return Object{{"phase", phase}, {"revision", revision_}, {"simulations", simulations_},
                      {"human_seat", seat_name(human_)}, {"active_seat", seat_name(state_->turn_player)},
                      {"turn", state_->turn_number}, {"finished", done}, {"winner", winner ? Json(winner) : Json{}},
                      {"both_lose", outcome == Outcome::BothLose}, {"end_reason", reason ? Json(reason) : Json{}},
                      {"human", player_json(*state_, human_, true, legal)},
                      {"opponent", player_json(*state_, 1 - human_, debug, legal)},
                      {"legal_actions", std::move(actions)}, {"recent_actions", history_}, {"last_ai_turn", last_ai_turn_}};
    }

    const Mlp& model_;
    int default_simulations_;
    std::optional<std::uint64_t> default_seed_;
    std::optional<GameState> state_;
    MatchupFeatures matchup_;
    std::mt19937 rng_;
    std::uint64_t revision_ = 0;
    int human_ = 0;
    int simulations_ = kDefaultSimulations;
    Array history_;
    Json last_ai_turn_;
};

template<class Char>
int run(int argc, Char** argv) {
    std::filesystem::path weights;
    int simulations = kDefaultSimulations;
    std::optional<std::uint64_t> seed;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path option(argv[i]);
        if (option == "--help") {
            std::cout << "Usage: svwb_play_cli --weights FILE [--simulations 1..256] [--seed INTEGER]\n";
            return 0;
        }
        if (++i == argc) {
            throw std::invalid_argument("Missing CLI option value");
        }
        const std::filesystem::path argument(argv[i]);
        if (option == "--weights") {
            weights = argument;
        } else if (option == "--seed" || option == "--simulations") {
            std::uint64_t n = 0;
            if (!unsigned_integer(argument.string(), n) ||
                (option == "--seed" ? n > kMaxSeed : n == 0 || n > kMaxSimulations)) {
                throw std::invalid_argument("CLI numeric option out of range");
            }
            if (option == "--seed") {
                seed = n;
            } else {
                simulations = static_cast<int>(n);
            }
        } else {
            throw std::invalid_argument("Unknown CLI option");
        }
    }
    if (weights.empty()) {
        throw std::invalid_argument("--weights FILE is required");
    }
    const auto model = Mlp::load(weights);
    Session session(model, simulations, seed);
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (std::string line; std::getline(std::cin, line);) {
        Object reply;
        try {
            if (line.size() > 4096) {
                throw std::invalid_argument("JSONコマンドが長すぎます。");
            }
            reply = session.handle(RequestParser(line).parse());
        } catch (const std::invalid_argument& error) {
            reply = {{"ok", false}, {"error", Object{{"code", "invalid_request"}, {"message", error.what()}}}};
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            reply = {{"ok", false}, {"error", Object{{"code", "engine_error"}, {"message", "対局処理に失敗しました。"}}}};
        }
        write_json(std::cout, std::move(reply));
        std::cout << '\n' << std::flush;
    }
    return std::cin.bad() ? 1 : 0;
}

} // namespace
} // namespace svwb::play

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        return svwb::play::run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
