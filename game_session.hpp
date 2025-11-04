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

struct MonsterState {
    int id;            // Unique ID for selection tracking
    std::string type;  // e.g., "GOBLIN", "SLIME"
    std::string assetKey; // Short key for client image mapping, e.g., "GB", "SLM"
};

struct PlayerState {
    PlayerClass currentClass = PlayerClass::UNSELECTED;
    std::string userId = "UNKNOWN";
    std::string currentArea = "TOWN";
    std::vector<MonsterState> currentMonsters; 
};

void do_session(tcp::socket socket);