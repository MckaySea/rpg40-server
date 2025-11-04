// File: src/game_session.hpp
#include <string>
#include <vector>
#include <boost/asio/ip/tcp.hpp>

namespace net = boost::asio;
using tcp = net::ip::tcp;

enum class PlayerClass {
    UNSELECTED,
    FIGHTER,
    WIZARD,
    ROGUE
};

// Player Stats Structure
struct PlayerStats {
    int health = 0;
    int mana = 0;
    int attack = 0;
    int defense = 0;
    int speed = 0;
    int level = 1;
    int experience = 0;
    int experienceToNextLevel = 100;

    // Constructor for easy initialization
    PlayerStats() = default;
    PlayerStats(int h, int m, int a, int d, int s, int l = 1, int xp = 0, int nextXp = 100)
        : health(h), mana(m), attack(a), defense(d), speed(s),
        level(l), experience(xp), experienceToNextLevel(nextXp)
    {
    }
};

struct MonsterState {
    int id;
    std::string type;
    std::string assetKey;
};

struct PlayerState {
    PlayerClass currentClass = PlayerClass::UNSELECTED;
    std::string userId = "UNKNOWN";
    std::string playerName = "";
    std::string currentArea = "TOWN";
    std::vector<MonsterState> currentMonsters;

    // Stats and progression
    PlayerStats stats;
    std::vector<std::string> spells; // ADDED
    int availableSkillPoints = 0;
    bool hasSpentInitialPoints = false;
    bool isFullyInitialized = false; // true after name, class, and point spending
};

void do_session(tcp::socket socket);