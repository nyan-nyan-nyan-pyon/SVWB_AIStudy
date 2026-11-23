#include "wb_cards.hpp"

namespace svwb::wb {
namespace {

using enum Kind;
using enum Trigger;
using enum Operation;

constexpr std::array<CardDef, kCardSlots> cards{{
    {.id=kWingedStatue, .name="有翼の石像", .kind=Amulet, .cost=1, .countdown=4, .act_cost=1, .effects={{
        {LastWords, Summon, Target::None, 1, kHolyFalcon},
        {OnAct, ReduceCountdown, Target::Self, 1},
    }}},
    {.id=kLilium, .name="図書室の魔女・リリウム", .kind=Follower, .cost=2, .atk=2, .hp=1, .effects={{
        {LastWords, Draw, Target::None, 1},
        {OnEvolve, Silence, Target::ChosenEnemyFollower},
        {OnEvolve, Damage, Target::ChosenEnemyFollower, 2},
    }}},
    {.id=kExias, .name="愛の天使・エクシアス", .kind=Follower, .cost=2, .atk=2, .hp=1,
     .abilities=FLAG_RUSH, .effects={{{OnPlay, AddHand, Target::None, 1, kEvidence}}}},
    {.id=kCraftsman, .name="地害の加工人", .kind=Follower, .cost=2, .atk=3, .hp=1,
     .effects={{{LastWords, AddHand, Target::None, 2, kEvidence}}}},
    {.id=kColette, .name="セイントエクソシスト・コレット", .kind=Follower, .cost=3, .atk=2, .hp=4,
     .abilities=FLAG_WARD, .effects={{
        {OnPlay, EvolveWithAlly, Target::Self},
        {OnEvolved, Damage, Target::RandomEnemyFollower, 1},
        {OnEvolved, Damage, Target::RandomEnemyFollower, 1},
    }}},
    {.id=kAlmiraj, .name="ミラクルアルミラージ", .kind=Follower, .cost=3, .atk=2, .hp=1,
     .abilities=FLAG_STORM, .crystal_form=kAlmirajCrystal},
    {.id=kDunamis, .name="信心の天使・デュナメス", .kind=Follower, .cost=3, .atk=2, .hp=2, .effects={{
        {OnPlay, Damage, Target::ChosenEnemyFollower, 2},
        {OnPlay, AddHand, Target::None, 1, kEvidence},
        {OnEvolve, Damage, Target::ChosenEnemyFollower, 2},
        {OnEvolve, AddHand, Target::None, 1, kEvidence},
    }}},
    {.id=kTrainer, .name="地害の調教師", .kind=Follower, .cost=3, .atk=3, .hp=3, .effects={{
        {OnPlay, AddHand, Target::None, 1, kRabbit},
        {OnEvolve, RestorePp, Target::None, 1},
    }}},
    {.id=kTestimony, .name="不毛の証言", .kind=Spell, .cost=3, .modes=2, .effects={{
        {.trigger=OnPlay, .operation=Damage, .target=Target::RandomEnemyFollower, .amount=4, .mode=1},
        {.trigger=OnPlay, .operation=GrantCrest, .mode=1, .crest=CrestId::Testimony},
        {.trigger=OnPlay, .operation=Summon, .amount=1, .card=kRabbit, .mode=2},
    }}},
    {.id=kProjectedStatue, .name="投影の鳥像", .kind=Amulet, .cost=4, .countdown=2, .act_cost=2, .effects={{
        {LastWords, Summon, Target::None, 1, kSplendidHawk},
        {OnAct, ReduceCountdown, Target::Self, 2},
    }}},
    {.id=kRituel, .name="希望の光彩・リテュエル", .kind=Follower, .cost=5, .atk=2, .hp=4, .modes=3, .effects={{
        {.trigger=OnPlay, .operation=Damage, .target=Target::AllEnemyFollowers, .amount=3, .mode=1},
        {.trigger=OnPlay, .operation=Damage, .target=Target::EnemyLeader, .amount=3, .mode=2},
        {.trigger=OnPlay, .operation=Heal, .target=Target::OwnLeader, .amount=3, .mode=3},
        {OnPlay, Damage, Target::Self, 3},
        {.trigger=OnEvolve, .operation=GrantCrest, .crest=CrestId::Rituel},
    }}},
    {.id=kPropagation, .name="荒廃の繁殖", .kind=Spell, .cost=5, .enhance_cost=8, .effects={{
        {OnPlay, Summon, Target::None, 2, kRabbit},
        {.trigger=OnPlay, .operation=Summon, .amount=2, .card=kRabbit, .mode=kEnhancedMode},
    }}},
    {.id=kRapid, .name="地害の三悪・ラピッドハット", .kind=Follower, .cost=6, .atk=4, .hp=4, .effects={{
        {OnPlay, Banish, Target::ChosenEnemyFollower},
        {OnPlay, Summon, Target::None, 1, kRabbit},
        {.trigger=OnEvolve, .operation=GrantCrest, .crest=CrestId::Rapid},
        {.trigger=OnSuperEvolve, .operation=ExtendCrest, .amount=1, .crest=CrestId::Rapid},
    }}},
    {.id=kTeresa, .name="エルゴンプリーステス・テレサ", .kind=Follower, .cost=7, .atk=3, .hp=3, .effects={{
        {.trigger=OnPlay, .operation=SummonModified, .amount=2, .card=kHealingSister,
         .health_modifier=-2, .granted_abilities=FLAG_STORM},
    }}},
    {.id=kRabbit, .name="地害の兎", .kind=Follower, .cost=2, .atk=1, .hp=1, .effects={{
        {OnEnter, BanishLowestAndEvolve, Target::Self},
        {LastWords, Damage, Target::EnemyLeader, 2},
    }}},
    {.id=kEvidence, .name="神域の物証", .kind=Amulet, .cost=1, .act_cost=1, .clue=true, .effects={{
        {OnAct, Draw, Target::None, 1},
        {OnAct, SpendClue, Target::Self, 1},
    }}},
    {.id=kHolyFalcon, .name="ホーリーファルコン", .kind=Follower, .cost=3, .atk=2, .hp=2, .abilities=FLAG_STORM},
    {.id=kSplendidHawk, .name="壮麗なる隼", .kind=Follower, .cost=6, .atk=4, .hp=4, .abilities=FLAG_STORM},
    {.id=kHealingSister, .name="癒しのシスター", .kind=Follower, .cost=6, .atk=3, .hp=5, .abilities=FLAG_WARD,
     .effects={{{OnPlay, Heal, Target::OwnLeader, 5}}}},
    {.id=kAlmirajCrystal, .name="ミラクルアルミラージ", .kind=Amulet, .cost=1, .countdown=3, .act_cost=1, .effects={{
        {LastWords, Summon, Target::None, 1, kAlmiraj},
        {OnAct, ReduceCountdown, Target::Self, 1},
    }}},
}};

constexpr std::array<CrestDef, 3> crests{{
    {CrestId::Testimony, 2, CrestClock::Start, {{
        {TurnStart, Heal, Target::OwnLeader, 1},
        {TurnStart, AddHand, Target::None, 1, kEvidence},
    }}},
    {CrestId::Rituel, 1, CrestClock::Start, {{
        {LastWords, SummonEvolved, Target::None, 0, kRituel},
    }}},
    {CrestId::Rapid, 1, CrestClock::Start, {{
        {TurnStart, Summon, Target::None, 1, kRabbit},
    }}},
}};

static_assert([] {
    for (int i = 0; i < kCardSlots; ++i) {
        if (cards[i].id != kFirstCard + i) {
            return false;
        }
    }
    return true;
}());

} // namespace

const CardDef* find_card(CardId id) {
    return id >= kFirstCard && id <= kLastForm ? &cards[id - kFirstCard] : nullptr;
}

const CrestDef* find_crest(CrestId id) {
    const auto index = static_cast<int>(id) - 1;
    return index >= 0 && index < static_cast<int>(crests.size()) ? &crests[index] : nullptr;
}

CardId printed_card(CardId id) {
    return id == kAlmirajCrystal ? kAlmiraj : id;
}

bool is_deck_card(CardId id) {
    return id >= kFirstCard && id < kFirstCard + kDeckCardKinds;
}

} // namespace svwb::wb
