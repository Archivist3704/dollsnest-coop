#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dsh::reflect {

[[nodiscard]] bool is_npc_character(const std::string& actorClass, const std::string& controllerClass);
[[nodiscard]] std::string npc_exclusion_reason(const std::string& actorClass,
                                               const std::string& controllerClass);

struct EnemyRecord {
    std::uintptr_t actor = 0;
    std::string    actorClass;
    std::string    actorName;
    bool           locationRead = false;
    float          x = 0.0f;
    float          y = 0.0f;
    float          z = 0.0f;
    std::string    locationNote;

    std::uintptr_t controller = 0;
    bool           controllerRead = false;
    std::string    controllerClass;
    std::string    controllerNote;
};

struct BossMoveResult {
    std::string name;
    bool  applyOk = false;
    bool  hadBefore = false;
    bool  hadAfter = false;
    float beforeX = 0.0f, beforeY = 0.0f, beforeZ = 0.0f;
    float afterX = 0.0f, afterY = 0.0f, afterZ = 0.0f;
    float distanceToPawnCm = -1.0f;
    std::string note;
};
[[nodiscard]] BossMoveResult move_ai_character_near_player(const std::string& namePart, float distanceCm);

struct AiBootResult {
    std::string name;
    bool  attempted = false;
    bool  made = false;
    std::string note;
};
[[nodiscard]] AiBootResult boot_one_ai_character(const std::string& namePart);

struct AggroMachine;

struct EnemySurvey {
    bool          worldFound = false;
    std::uint32_t levelsWalked = 0;
    std::uint64_t actorsWalked = 0;

    std::uint64_t pawnsWalked = 0;
    std::string   worldNote;

    std::uintptr_t baseClass = 0;
    std::vector<EnemyRecord> enemies;

    std::vector<std::pair<std::string, std::uint32_t>> candidateClasses;

    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] EnemySurvey survey_enemies(std::vector<AggroMachine>* machinesOut = nullptr);

struct AggroMachine {
    std::uintptr_t actor = 0;
    std::string    name;
    std::string    cls;
    std::string    controllerCls;
    bool           hasController = false;
    bool           hasLocation = false;
    float          x = 0.0f;
    float          y = 0.0f;
    float          z = 0.0f;
    std::string    locationNote;
};

struct AggroCandidate {
    std::uintptr_t actor = 0;
    std::string name;
    std::string cls;
    float       score = 0.0f;
    float       targetedTime = 0.0f;
    bool        filtered = false;
    bool        isMachine = false;
    bool        isNpc = false;
};

struct AggroEnemy {
    std::string   enemyName;
    std::string   enemyClass;
    std::string   controllerClass;
    std::uintptr_t targeting = 0;
    std::string   mainTargetName;
    std::string   mainTargetClass;
    std::string   mainTargetNote;
    bool          listRead = false;
    std::string   listNote;
    std::vector<AggroCandidate> candidates;
    std::uint32_t candidatesUnreadable = 0;

    bool          perceptionRead = false;
    std::uint32_t perceptionKnown = 0;
    std::vector<std::string> perceptionNames;
    std::vector<std::string> perceptionActorNames;
    std::string   perceptionNote;
    bool          holdsMachine = false;
    bool          holdsNpc = false;

    bool          hasLocation = false;
    float         x = 0.0f;
    float         y = 0.0f;
    float         z = 0.0f;
    std::string   locationNote;
    struct MachineVerdict {
        std::string name;
        bool        inPerception = false;
        bool        inCandidates = false;
        bool        distanceKnown = false;
        float       distanceCm = 0.0f;
    };
    std::vector<MachineVerdict> machines;
};

struct AggroSurvey {
    bool          worldFound = false;
    std::string   worldNote;
    std::uint32_t enemiesListed = 0;
    std::uint32_t npcSkipped = 0;
    std::vector<std::string> npcNames;
    std::uint32_t enemiesWithMachineCandidate = 0;
    std::uint32_t enemiesWithMachinePerceived = 0;
    std::uint32_t enemiesWithNpcCandidate = 0;
    std::uint32_t candidatesTotal = 0;
    std::uint32_t candidatesFiltered = 0;
    std::uint32_t perceptionUnavailable = 0;
    std::vector<AggroMachine> machines;
    std::vector<AggroEnemy> enemies;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] AggroSurvey survey_aggro();

struct TargetOfferReport {
    bool          worldFound = false;
    std::string   note;
    std::uint32_t enemies = 0;
    std::uint32_t puppets = 0;
    std::uint32_t alreadyHeld = 0;
    std::uint32_t added = 0;
    std::uint32_t refused = 0;
    std::uint32_t npcSkipped = 0;

    std::uint32_t npcMachines = 0;
    std::vector<std::string> lines;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] TargetOfferReport offer_puppets_as_candidates();

struct TargetWatchArm {
    bool          ok = false;
    std::string   note;
    std::uint32_t enemies = 0;
    std::uint32_t machines = 0;
};

[[nodiscard]] TargetWatchArm target_watch_arm();
[[nodiscard]] bool target_watch_armed();

[[nodiscard]] std::vector<std::string> target_watch_sample(std::uint64_t nowMs);

[[nodiscard]] std::vector<std::string> target_watch_disarm();

struct HpAttributeSetRead {
    std::string   cls;
    std::string   name;
    std::uintptr_t address = 0;

    std::string   changeNote;

    static constexpr int kEnemyFloats = 28;
    float narrow[kEnemyFloats] = {};
    bool  narrowRead = false;

    static constexpr int kWideFloats = 96;
    float wide[kWideFloats] = {};
    bool  wideRead = false;
};

struct HpEnemyRead {
    std::string enemyName;
    std::string enemyClass;
    std::uintptr_t asc = 0;
    std::string ascClass;
    std::string note;
    std::vector<HpAttributeSetRead> sets;
};

struct HpSurvey {
    bool          worldFound = false;
    std::string   worldNote;
    std::uint32_t enemiesListed = 0;
    std::uint32_t npcSkipped = 0;
    std::uint32_t withAbilitySystem = 0;

    bool          localFound = false;
    std::string   localNote;
    std::string   localClass;
    std::uintptr_t localAsc = 0;
    std::vector<HpAttributeSetRead> localSets;
    std::vector<HpEnemyRead> enemies;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] HpSurvey survey_hp(bool localPlayerToo);

[[nodiscard]] std::vector<std::string> survey_owned_objects(std::uintptr_t actor, const std::string& label);

[[nodiscard]] std::vector<std::string> survey_weapon_inner(std::uintptr_t weapon, const std::string& label);

[[nodiscard]] std::vector<std::string> survey_weapon_slot(std::uintptr_t actor, std::uintptr_t slotOffset,
                                                         const std::string& label);

struct HpWatchTarget {
    std::uintptr_t actor = 0;
    std::uintptr_t set = 0;
    std::string    name;
    std::string    cls;
    bool           isLocal = false;
    bool           npcClassified = false;
};

struct HpWatchSample {
    HpWatchTarget target;
    float health = 0.0f;
    float shield = 0.0f;
    float maxHealth = 0.0f;
    float maxShield = 0.0f;
    bool  healthRead = false;
    bool  shieldRead = false;
};

struct HpWatchFrame {
    bool        ok = false;
    std::string note;
    std::vector<HpWatchSample> samples;
};

void hp_watch_refresh_targets(bool includeNpcClassified);

[[nodiscard]] HpWatchFrame hp_watch_sample();

[[nodiscard]] const std::vector<HpWatchTarget>& hp_watch_targets();

void hp_watch_forget_targets();

[[nodiscard]] bool hp_write_health(std::uintptr_t set, float value, float* readBackOut, std::string& note);

struct CharacterVitals {
    std::uintptr_t set = 0;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float shield = 0.0f;
    float maxShield = 0.0f;
    bool  read = false;
    std::string classNote;
    std::string note;
};

[[nodiscard]] CharacterVitals character_vitals(std::uintptr_t actor);

[[nodiscard]] bool character_attribute_set(std::uintptr_t actor, std::uintptr_t& setOut, std::string& note);

struct CharacterEnergy {
    std::uintptr_t set = 0;
    float energy = 0.0f;
    float maxEnergy = 0.0f;
    bool  read = false;
    std::string note;
};

[[nodiscard]] CharacterEnergy character_energy(std::uintptr_t actor);

struct HpDamageProbe {
    bool  resolved = false;
    bool  callMade = false;
    bool  effective = false;
    float returned = 0.0f;
    float before = 0.0f;
    float after = 0.0f;
    bool  readBefore = false;
    bool  readAfter = false;
    std::string name;
    std::string actorClass;
    std::string note;
};

[[nodiscard]] HpDamageProbe hp_probe_apply_damage(const std::string& namePart, float amount);

struct HpNotifyProbe {
    bool  resolved = false;
    bool  writeLanded = false;
    bool  notified = false;
    float before = 0.0f;
    float wrote = 0.0f;
    float after = 0.0f;
    bool  readAfter = false;
    std::string name;
    std::string note;
};

[[nodiscard]] HpNotifyProbe hp_probe_set_and_notify(const std::string& namePart, float value);

[[nodiscard]] std::vector<std::string> hp_probe_near_list(std::size_t maxLines);

[[nodiscard]] bool hp_notify_health_changed(std::uintptr_t set, float oldValue, std::string& note);

void hp_apply_lethal_damage_on_game_thread(std::uintptr_t set, std::uintptr_t actor, const std::string& name);

struct AiRuleVerdict {
    std::string ruleClass;
    bool        asked = false;
    bool        filtered = false;
    bool        scoreKnown = false;
    float       score = 0.0f;
    bool        nativeRule = false;
    bool        hasRange = false;
    float       maxRange = 0.0f;
    bool        inverse = false;
    std::string note;
};

struct AiEnemyFacts {
    std::string enemyName;
    std::string enemyClass;
    std::string controllerClass;
    bool        teamKnown = false;
    int         team = -1;
    bool        stateKnown = false;
    int         state = -1;
    std::string mainTargetName;
    bool        attitudeToPuppetKnown = false;
    int         attitudeToPuppet = -1;
    bool        attitudeToOwnKnown = false;
    int         attitudeToOwn = -1;

    std::vector<std::string> senses;
    bool        sightKnown = false;
    bool        sightDetectsEnemies = false;
    bool        sightDetectsNeutrals = false;
    bool        sightDetectsFriendlies = false;
    std::string sensesNote;
    bool        perceptionRead = false;
    std::uint32_t perceptionKnown = 0;
    std::vector<std::string> perceptionNames;
    bool        seesPuppet = false;
    bool        seesOwn = false;
    std::string perceptionNote;
    bool        listRead = false;
    std::uint32_t candidates = 0;
    std::uint32_t candidatesFiltered = 0;
    bool        puppetIsCandidate = false;
    bool        puppetCandidateFiltered = false;
    bool        puppetCandidateScoreKnown = false;
    float       puppetCandidateScore = 0.0f;
    std::string listNote;
    bool        distanceToPuppetKnown = false;
    float       distanceToPuppetCm = 0.0f;
    bool        distanceToOwnKnown = false;
    float       distanceToOwnCm = 0.0f;

    bool        candidateCheckMade = false;
    bool        candidateAccepted = false;
    std::string candidateCheckNote;
    bool        filterCheckMade = false;
    bool        filterSaysFiltered = false;
    bool        filterScoreKnown = false;
    float       filterScore = 0.0f;
    std::string filterCheckNote;
    std::vector<AiRuleVerdict> rules;

    bool        sideEffectObserved = false;
    std::string sideEffectNote;
};

struct AiTargetingSurvey {
    bool          worldFound = false;
    std::string   worldNote;
    std::uint32_t enemiesListed = 0;
    std::uint32_t npcSkipped = 0;
    std::vector<std::string> npcNames;

    bool          hasPuppet = false;
    std::string   puppetName;
    std::string   puppetClass;
    bool          puppetTeamKnown = false;
    int           puppetTeam = -1;
    bool          hasOwn = false;
    std::string   ownName;
    std::string   ownClass;
    bool          ownTeamKnown = false;
    int           ownTeam = -1;
    bool          teamFunctionUsable = false;
    std::string   teamFunctionNote;
    bool          stimuliScanned = false;
    bool          puppetHasStimuliSource = false;
    bool          ownHasStimuliSource = false;
    std::vector<std::string> puppetComponents;
    std::vector<std::string> ownComponents;
    std::vector<AiEnemyFacts> enemies;
    std::uint32_t enemiesNotDetailed = 0;

    std::string   verdict;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] AiTargetingSurvey survey_ai_targeting(std::uint32_t detailCount);

struct AiTeamWriteResult {
    bool        puppetFound = false;
    std::string puppetName;
    bool        callMade = false;
    bool        written = false;
    bool        beforeKnown = false;
    int         before = -1;
    bool        afterKnown = false;
    int         after = -1;
    int         askedFor = -1;
    std::string note;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] AiTeamWriteResult ai_set_puppet_team(int team);

struct AiTargetSnapshot {
    bool          perceptionRead = false;
    std::uint32_t perceived = 0;
    bool          seesPuppet = false;
    bool          listRead = false;
    bool          puppetCandidate = false;
    bool          puppetFiltered = false;
    bool          puppetScoreKnown = false;
    float         puppetScore = 0.0f;
    std::string   mainTarget;
    int           state = -1;
};

struct AiNudgeResult {
    bool        found = false;
    std::string enemyName;
    bool        callMade = false;
    bool        changed = false;
    std::uint8_t affiliationBefore = 0;
    std::uint8_t affiliationAfter = 0;
    bool        affiliationKnown = false;
    std::string note;
    AiTargetSnapshot before;
    AiTargetSnapshot after;
    bool        afterTaken = false;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] AiNudgeResult ai_push_perception(const std::string& enemyPart);

[[nodiscard]] AiNudgeResult ai_set_enemy_friendlies(const std::string& enemyPart, bool enable);

[[nodiscard]] AiNudgeResult ai_report_damage_to_enemy(const std::string& enemyPart, float amount);

struct AiAttitudeRow {
    int team = -1;
    std::string teamName;
    int attitude = -1;
};
struct AiAttitudeTable {
    bool usable = false;
    std::string note;
    int against = 255;
    std::vector<AiAttitudeRow> rows;
    std::uint32_t hostileCount = 0;
    std::uint32_t readCount = 0;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};
[[nodiscard]] AiAttitudeTable ai_team_attitudes_towards_no_team();

[[nodiscard]] AiNudgeResult ai_set_enemy_team(const std::string& enemyPart, int team);

[[nodiscard]] AiNudgeResult ai_register_puppet(const std::string& enemyPart, bool enable);

}
