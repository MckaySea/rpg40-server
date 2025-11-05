// File: LocalWebSocketServer/game_session.hpp
#pragma once 

#include <string>
#include <vector>
#include <deque> 
#include <chrono> 
#include <boost/optional.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- ADDED: Global game constants ---
static const int GRID_COLS = 40;
static const int GRID_ROWS = 22;
static const std::chrono::milliseconds MOVEMENT_DELAY{ 150 }; // Speed: ms per tile
static const int SERVER_TICK_RATE_MS = 50; // 20 ticks per second

enum class PlayerClass {
    UNSELECTED,
    FIGHTER,
    WIZARD,
    ROGUE
};

struct PlayerBroadcastData {
    std::string userId;
    std::string playerName;
    PlayerClass playerClass = PlayerClass::UNSELECTED;
    std::string currentArea = "TOWN";

    int posX = 0;
    int posY = 0;
};

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

struct MonsterState {
    int id;
    std::string type;
    std::string assetKey;
};

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

struct Point {
    int x, y;
    bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
    }
    bool operator!=(const Point& other) const {
        return !(*this == other);
    }
};


struct PlayerState {
    PlayerClass currentClass = PlayerClass::UNSELECTED;
    std::string userId = "UNKNOWN";
    std::string playerName = "";
    std::string currentArea = "TOWN";

    int posX = 0;
    int posY = 0;

    std::vector<MonsterState> currentMonsters;

    // Stats and progression
    PlayerStats stats;
    std::vector<std::string> spells;
    int availableSkillPoints = 0;
    bool hasSpentInitialPoints = false;
    bool isFullyInitialized = false;

    // Combat State
    bool isInCombat = false;
    boost::optional<MonsterInstance> currentOpponent;
    bool isDefending = false;

    // Movement State
    std::deque<Point> currentPath; // The A* path
    std::chrono::steady_clock::time_point lastMoveTime; // Timer
};