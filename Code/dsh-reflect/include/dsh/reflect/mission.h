#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsh::reflect {

inline constexpr const char* kMainSaveSlot = "ECBZ40O0JYGEQI0BF7JKC919O";
inline constexpr const char* kTestSaveSlot = "0AK7P69R4BWHT7LQX0A52B6X2";

struct MissionVolume {
    std::uintptr_t actor = 0;
    std::string    actorClass;
    std::string    actorName;
    std::string    missionId;
    std::uint32_t  missionIdIndex = 0;
    std::uint32_t  missionIdNumber = 0;
    std::string    missionIdNote;
    bool           typeMeasured = false;
    std::uint8_t   missionType = 0;
    std::string    typeNote;

    bool           stateMeasured = false;
    std::uint8_t   state = 0;
    std::string    stateNote;
};

struct MissionTable {

    bool          slotNameMeasured = false;
    std::string   slotName;
    std::string   slotNote;
    std::uint32_t emsInstances = 0;
    bool          stateReadAllowed = false;
    std::string   gateNote;

    std::uintptr_t saveObject = 0;
    std::string    saveObjectName;
    std::string    saveClassName;
    std::uint32_t  saveInstances = 0;
    std::string    saveNote;

    bool           saveCustomNameMeasured = false;
    std::string    saveCustomName;
    std::string    saveCustomNameNote;

    bool          worldFound = false;
    std::uint32_t levelsWalked = 0;
    std::uint64_t actorsWalked = 0;
    std::string   worldNote;
    std::vector<MissionVolume> volumes;

    std::vector<std::pair<std::string, std::uint32_t>> volumeishClasses;

    bool          defeatedBossNumMeasured = false;
    std::int32_t  defeatedBossNum = 0;
    std::string   defeatedBossNumNote;

    [[nodiscard]] std::vector<std::string> report_lines() const;

};

[[nodiscard]] std::vector<std::string> survey_mission_actors();

[[nodiscard]] MissionTable read_mission_table();

struct SaveContainer {
    std::string    name;
    std::uint32_t  offset = 0;
    std::string    fieldType;
    std::uint32_t  elementBytes = 0;
    bool           headerRead = false;
    std::uintptr_t elements = 0;
    std::uint32_t  num = 0;
    std::uint32_t  max = 0;
    std::uint32_t  firstFree = 0;
    std::uint32_t  numFree = 0;

    bool           freeListMeasured = false;
    std::string    note;

    std::vector<std::string> samples;
};

struct KeyItemProbe {
    std::string  className;
    std::string  assetName;
    bool         inSave = false;
    std::string  note;
};

struct SaveProbe {
    bool           saveFound = false;
    std::uintptr_t saveObject = 0;
    std::string    note;

    std::string    fields;
    std::vector<SaveContainer> containers;

    std::vector<KeyItemProbe> keyItems;
    std::uint32_t obtainedItemsResolved = 0;

    std::vector<std::string> flagSyncable;
    std::vector<std::string> flagExcluded;
    std::uint32_t flagsRead = 0;

    std::vector<std::string> unmatchedKeyishNames;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] SaveProbe probe_save_containers();

struct MissionStartOutcome {

    std::string  missionId;
    std::uint8_t requestedType = 0;
    std::uint32_t nameIndex = 0;
    std::uint32_t nameNumber = 0;

    std::uintptr_t gameMode = 0;
    std::string    world;
    std::string    gameModeClass;
    std::string    declaringClass;
    std::string    chain;
    bool           functionResolved = false;
    bool           called = false;
    std::string    note;

    bool           beforeStateMeasured = false;
    std::uint8_t   beforeState = 0;
    bool           beforeIsMissionMode = false;
    bool           beforeIsMissionModeMeasured = false;

    bool           afterStateMeasured = false;
    std::uint8_t   afterState = 0;
    bool           afterIsMissionMode = false;
    bool           afterIsMissionModeMeasured = false;
    std::string    afterStateNote;
    std::uint32_t  pendingAfterMs = 0;

    bool                        recentlyMeasured = false;
    std::vector<std::string>    recentlyDefeated;
    std::string                 recentlyNote;

    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] MissionStartOutcome start_mission_mode(const std::string& missionId,
                                                     std::uint8_t requestedType);

[[nodiscard]] std::vector<std::string> survey_boss_ai(const std::string& namePart);

struct BossWakeResult {
    bool         attempted = false;
    bool         bootCalled = false;
    bool         stateCalled = false;
    bool         dispatched = false;
    std::string  character;
    std::string  note;
    std::vector<std::string> lines;
};
[[nodiscard]] BossWakeResult mission_wake_boss(const std::string& namePart);

struct BossStateWriteResult {
    std::string  missionId;
    std::uint8_t requested = 0;
    std::uint32_t nameIndex = 0;
    std::uint32_t nameNumber = 0;

    bool           saveFound = false;
    std::uintptr_t save = 0;
    std::uint32_t  elements = 0;
    std::uint32_t  entriesScanned = 0;

    bool           elementFound = false;
    std::uint32_t  elementSlot = 0;
    std::uintptr_t element = 0;
    std::uintptr_t valueAddress = 0;
    bool           beforeMeasured = false;
    std::uint8_t   before = 0;
    bool           written = false;
    bool           afterMeasured = false;
    std::uint8_t   after = 0;

    bool           defeatedBeforeMeasured = false;
    std::int32_t   defeatedBefore = 0;
    bool           defeatedAfterMeasured = false;
    std::int32_t   defeatedAfter = 0;

    std::string note;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] BossStateWriteResult set_boss_state(const std::string& missionId, std::uint8_t state);

struct BossStateRecord {
    std::string  id;
    std::uint8_t state = 0;
};

[[nodiscard]] std::vector<BossStateRecord> all_boss_states(std::string& note);
[[nodiscard]] bool mission_states_of(const std::vector<std::string>& ids,
                                     std::vector<std::uint8_t>& states,
                                     std::vector<bool>& measured, std::string& note);

struct MissionRelaunchResult {
    std::string  missionId;
    std::uint32_t nameIndex = 0;
    std::uint32_t nameNumber = 0;
    std::uint8_t type = 0;

    std::uintptr_t volume = 0;
    std::string    volumeClass;
    std::string    volumeName;
    std::uintptr_t manager = 0;
    std::string    managerClass;
    std::uint32_t  walls = 0;
    bool           wallsMeasured = false;

    std::vector<std::string> unattemptedLevels;
    std::vector<std::string> attemptedLevels;
    std::vector<std::string> clearedLevels;
    std::string levelsNote;

    bool         stateBeforeMeasured = false;
    std::uint8_t stateBefore = 0;
    bool         stateWritten = false;
    bool         stateAfterMeasured = false;
    std::uint8_t stateAfter = 0;

    bool         initializeCalled = false;
    std::string  initializeNote;
    bool         startedModeCalled = false;
    std::string  startedModeNote;
    bool         startCalled = false;
    std::string  startNote;
    bool         modeAfterMeasured = false;
    bool         modeAfter = false;

    std::string note;
    std::vector<std::string> lines;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};

[[nodiscard]] MissionRelaunchResult relaunch_mission(const std::string& missionId);

[[nodiscard]] std::vector<std::string> trigger_mission_volume(const std::string& missionId);

struct BossReviveResult {
    bool        attempted = false;
    std::string character;
    std::string controller;

    bool  healthMeasured = false;
    float healthBefore = 0.0f;
    float maxHealth = 0.0f;
    bool  healthWritten = false;
    float healthAfter = 0.0f;
    bool  notifyCalled = false;
    std::string healthNote;

    bool collisionEnabled = false;
    bool bootCalled = false;
    bool stateCalled = false;

    std::string note;
    std::vector<std::string> lines;
    [[nodiscard]] std::vector<std::string> report_lines() const;
};
[[nodiscard]] BossReviveResult mission_revive_boss(const std::string& namePart);

[[nodiscard]] std::vector<std::string> mission_brain(const std::string& namePart,
                                                     const std::string& treePart);

[[nodiscard]] std::vector<std::string> mission_collide(const std::string& namePart);

[[nodiscard]] std::vector<std::string> mission_save_game();

[[nodiscard]] std::vector<std::string> survey_mission_levels(const std::string& missionId);

}

[[nodiscard]] std::vector<std::string> survey_mission_verbs();

namespace dsh::reflect {

[[nodiscard]] std::vector<std::string> survey_mission_verbs();
}
