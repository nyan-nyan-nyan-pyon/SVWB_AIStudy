#include "wb_cards.hpp"
#include "play_json.hpp"

#include <algorithm>
#include <fstream>

namespace svwb::wb {
namespace {

struct OfficialCard {
    std::uint32_t official_id;
    CardId id;
};

#include "wb_card_data.inc"

} // namespace

const CardDef* find_card(CardId id) {
    const auto index = static_cast<int>(id) - kFirstCard;
    return index >= 0 && index < static_cast<int>(std::size(cards)) ? &cards[index] : nullptr;
}

const CardDef* find_official_card(std::uint32_t id) {
    const auto found = std::lower_bound(std::begin(official_cards), std::end(official_cards), id,
        [](const auto& left, auto right) { return left.official_id < right; });
    return found != std::end(official_cards) && found->official_id == id ? find_card(found->id) : nullptr;
}

const CrestDef* find_crest(CrestId id) {
    const auto found = std::find_if(std::begin(crests), std::end(crests), [id](const auto& crest) { return crest.id == id; });
    return found == std::end(crests) ? nullptr : found;
}

CardId printed_card(CardId id) {
    const auto* card = find_card(id);
    return card && card->base_form ? card->base_form : id;
}

std::span<const CardId> neutral_bishop_rotation() {
    return rotation_batch;
}

std::span<const CardId> elf_rotation() {
    return rotation_elf;
}

std::span<const CardId> royal_rotation() {
    return rotation_royal;
}

std::span<const CardId> dragon_rotation() {
    return rotation_dragon;
}

std::span<const CardId> nightmare_rotation() {
    return rotation_nightmare;
}

std::span<const CardId> witch_rotation() {
    return rotation_witch;
}

std::span<const CardId> nemesis_rotation() {
    return rotation_nemesis;
}

std::span<const CardId> all_rotation() {
    return rotation_all;
}

bool is_deck_card(CardId id) {
    const auto* card = find_card(id);
    return card && card->implemented && card->deck_eligible;
}

int card_cost(const CardDef& card, CardModifiers modifiers) {
    return std::max(0, card.cost + modifiers.cost + modifiers.temporary_cost_delta -
        card.spellboost_cost_reduction * modifiers.spellboost);
}

int enhance_payment(const CardDef& card, int pp) {
    if (card.next_enhance_cost > 0 && pp >= card.next_enhance_cost) return card.next_enhance_cost;
    return pp >= card.enhance_cost ? card.enhance_cost : 0;
}

Deck read_deck(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::invalid_argument("Could not open deck JSON");
    }
    std::string contents(16385, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(file.gcount()));
    if (contents.size() > 16384 || file.bad()) {
        throw std::invalid_argument("Could not read deck JSON (limit 16 KiB)");
    }
    std::string_view json(contents);
    if (json.starts_with("\xef\xbb\xbf")) json.remove_prefix(3);
    const auto parsed = play::RequestParser(json).parse();
    const auto rows = parsed.find("cards");
    const auto* list = rows == parsed.end() ? nullptr : std::get_if<play::Json::Array>(&rows->second.value);
    if (!list) {
        throw std::invalid_argument("Deck JSON requires a cards array");
    }
    Deck deck{};
    int used = 0;
    for (const auto& row : *list) {
        const auto* card = std::get_if<play::Json::Object>(&row.value);
        if (!card || !card->contains("card_id") || !card->contains("copies")) {
            throw std::invalid_argument("Each deck row requires card_id and copies");
        }
        const auto* id = std::get_if<std::uint64_t>(&card->at("card_id").value);
        const auto* copies = std::get_if<std::uint64_t>(&card->at("copies").value);
        if (!id || *id > UINT32_MAX || !copies || *copies < 1 || *copies > 3 || *copies > kDeckSize - used) {
            throw std::invalid_argument("Invalid deck card ID or copy count");
        }
        const auto* def = find_official_card(static_cast<std::uint32_t>(*id));
        if (!def || !is_deck_card(def->id)) {
            throw std::invalid_argument("Card lacks playable engine rules: " + std::to_string(*id));
        }
        if (std::find(deck.begin(), deck.begin() + used, def->id) != deck.begin() + used) {
            throw std::invalid_argument("Duplicate deck row: " + std::to_string(*id));
        }
        std::fill_n(deck.begin() + used, *copies, def->id);
        used += static_cast<int>(*copies);
    }
    if (used != kDeckSize) {
        throw std::invalid_argument("A deck must contain exactly 40 cards");
    }
    return deck;
}

} // namespace svwb::wb
