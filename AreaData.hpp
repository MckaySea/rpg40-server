// File: AreaData.hpp
#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include "game_session.hpp"   

struct ZoneTransition {
	int x, y;
	std::string targetArea;
};

struct MonsterSpawn {
	int id;
	std::string name;
	int x, y;
	int minCount;
	int maxCount;
};

// NEW: Represents a monster that is "live" in the world.
struct LiveMonster {
	int spawn_id;                 // The unique ID from the MonsterSpawn template
	std::string monster_type;     // e.g., "SLIME"
	std::string asset_key;        // e.g., "SLM"
	Point position;
	Point original_spawn_point;
	bool is_alive = true;
	std::chrono::steady_clock::time_point respawn_time; // Time when it should respawn
};

struct AreaData {
	std::string name;
	std::string backgroundImage;
	const std::vector<std::vector<int>>* grid = nullptr;

	std::vector<InteractableObject> interactables;
	std::vector<ZoneTransition> zones;

	std::vector<MonsterSpawn> monsters;

	std::map<int, LiveMonster> live_monsters; // Keyed by spawn_id
	std::mutex monster_mutex;

};