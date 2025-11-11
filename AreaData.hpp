#pragma once
#include <string>
#include <vector>
#include "game_session.hpp"   //  we need this for InteractableObject and Point

struct ZoneTransition {
    int x, y;
    std::string targetArea;
};

struct MonsterSpawn {
    int id; // <-- ADD THIS
    std::string name;
    int x, y;
    int minCount;
    int maxCount;
};

struct AreaData {
    std::string name;
    std::string backgroundImage;
    const std::vector<std::vector<int>>* grid = nullptr;

    std::vector<InteractableObject> interactables;
    std::vector<ZoneTransition> zones;
    std::vector<MonsterSpawn> monsters;
};
