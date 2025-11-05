// File: LocalWebSocketServer/game_session.hpp
#include <string>
#include <vector>
#include <boost/optional.hpp> // CHANGED: Replaced <optional>
#include <boost/asio/ip/tcp.hpp>

namespace net = boost::asio;
using tcp = net::ip::tcp;

enum class PlayerClass {
    UNSELECTED,
    FIGHTER,
    WIZARD,
    ROGUE
};

// ADDED: Simple struct for sharing public player data
// Moved here because it depends on PlayerClass
struct PlayerBroadcastData {
    std::string userId;
    std::string playerName;
    PlayerClass playerClass = PlayerClass::UNSELECTED;
    std::string currentArea = "TOWN";
};

// Player Stats Structure
struct PlayerStats {
    int health = 0;
    int maxHealth = 0;
    int mana = 0;
    int maxMana = 0;
    int attack = 0;
    int defense = 0;
    int speed = 0;
    int level = 1;
    int experience = 0;
    int experienceToNextLevel = 100;

    PlayerStats() = default;
    PlayerStats(int h, int m, int a, int d, int s, int l = 1, int xp = 0, int nextXp = 100)
        : health(h), maxHealth(h), mana(m), maxMana(m), attack(a), defense(d), speed(s),
        level(l), experience(xp), experienceToNextLevel(nextXp)
    {
    }
};

// Represents the template for a monster in an area
struct MonsterState {
    int id;
    std::string type;
    std::string assetKey;
};

// Represents an active monster in combat
struct MonsterInstance {
    int id;
    std::string type;
    std::string assetKey;
    int health;
    int maxHealth;
    int attack;
    int defense;
    int speed;
    int xpReward;

    MonsterInstance(int i, std::string t, std::string a, int h, int atk, int def, int spd, int xp)
        : id(i), type(t), assetKey(a), health(h), maxHealth(h), attack(atk), defense(def), speed(spd), xpReward(xp) {
    }
};


struct PlayerState {
    PlayerClass currentClass = PlayerClass::UNSELECTED;
    std::string userId = "UNKNOWN";
    std::string playerName = "";
    std::string currentArea = "TOWN";
    std::vector<MonsterState> currentMonsters;

    // Stats and progression
    PlayerStats stats;
    std::vector<std::string> spells;
    int availableSkillPoints = 0;
    bool hasSpentInitialPoints = false;
    bool isFullyInitialized = false;

    // ADDED: Combat State
    bool isInCombat = false;
    boost::optional<MonsterInstance> currentOpponent; // CHANGED: Using boost::optional
    bool isDefending = false;
};

void do_session(tcp::socket socket);