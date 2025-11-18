McKay Seamons

    Nicola

    Yousaf

    Ali

Design Criteria

1) World Map that the user can move around

Our project has an overarching world map that allows users to travel to different zones, and there are over 10 zones.

We implemented A* pathfinding. We initially used Chebyshev distance for 8-directional movement, but the focus on diagonal movement led us to switch to Octile distance, which provides a more natural feel and looks much better for interpolation on the client.

2) Combat of some sort, and some way of winning the game

Our game is an open-ended MMORPG. The way you "Win" is by progressing your character.

    Skills: You can progress through skills like mining, fishing, woodcutting, cooking, crafting, and combat.

    Gear: You get gear through crafting and killing mobs, with a chance to get drops with additional effects from a list of about one hundred. The same randomness applies to crafting.

    Goal: You "beat the game" by fighting and defeating the zone bosses and making your character as overpowered as possible.

3) Must have colors

The game has a frontend built with JavaScript that includes multiple different sprites and colors.

    Link: https://github.com/MckaySea/rpg-react-client

4) Must have 5 puzzles to win the game

The "puzzles" are the players figuring out the world around them and navigating how to make their characters as strong as possible. There are many hidden secrets, such as skillbooks as super rare drops and secret areas with rare resource nodes. Players can also trade items amongst each other, creating a dynamic experience.

5) 40 lines of dialogue and/or descriptions of the world

Our NPCs have hundreds of lines of dialogue, as seen in the gamedata.cpp file.

--------------------------------------------------------------------------------------------------------






## **Project Readme: Server Updates for Team**

**McKay - \!**

I've made some significant updates to the server code to make collaboration smoother and help everyone understand the project better:

  * **Code Commenting:** I had Gemini add comments to **every line of code** to explain what's happening within the server.
  * **Namespace Policy:** Remember, we are **not** using the `std` namespace, so you must **prefix** standard library calls (like `cout`) with `std::` (e.g., `std::cout`).
  * **Modular Design:** I've separated the logic into **multiple files** now. This should make it much easier for the team to make targeted edits.

If you need anything explained or require help with the server code, just ask me\!

-----

## ** Current Project To-Dos**

I'll be updating this section as we progress and assigning specific tasks.

| Team Member | Task Description | Status |
| :--- | :--- | :--- |
| **Yousaf** | **Dialogue:** Write out what NPCs are going to say and label who is saying it. Determine which NPCs will give quests. Also, please name the NPCs as you wish. | Close to finished |
| **Ali** | **Visuals & Images:** Find and prepare visuals/images for NPCs, monsters, and objects. I will post a list of required assets for you to find soon. | HALF DONE |
| **McKay** | **Feature development:** Change spawn rates on some maps, so you arent trapped behind blocked areas and cant move to the desired area that was intended | HALF DONE |
| **McKay** | **Feature development:** Quests now that interaction system is done, with a dialogue system| To Do |
| **McKay** | **Feature development:** Skills for melee players like wizards have spells. | DONE |
| **Nicola** | **Feature development:** Use map tool to update map obstacles for the grids | In progress |

-----

## ** Ali's Monster Asset Templates**

Here are the current monster templates and placeholder assets.

### **Server-Side Monster Mapping (C++)**

This map is used to link the monster name (type) to a unique Asset Key for data handling.

```cpp
// Monster Templates (Type, AssetKey)
const std::map<std::string, std::string> MONSTER_ASSETS = {
    {"SLIME", "SLM"}, {"GOBLIN", "GB"}, {"WOLF", "WLF"}, {"BAT", "BAT"},
    {"SKELETON", "SKL"}, {"GIANT SPIDER", "SPDR"}, {"ORC BRUTE", "ORC"}
};
```

### **Client-Side Monster Asset Details (Example)**

This object holds the specific visual data for the client. **Ali**, please note the file upload instruction below\!

```javascript
const MONSTER_ASSETS = {
  SLM: { url: "https://placehold.co/50x50/3cb371/ffffff?text=SLM", width: 50, height: 50 },
  GB: { url: "https://placehold.co/50x55/8b4513/ffffff?text=GB", width: 50, height: 55 },
  WLF: { url: "https://placehold.co/60x40/5A5A5A/ffffff?text=WOLF", width: 60, height: 40 },
  BAT: { url: "https://placehold.co/40x30/4b0082/ffffff?text=BAT", width: 40, height: 30 },
  SKL: { url: "https://placehold.co/45x60/808080/ffffff?text=SKL", width: 45, height: 60 },
  SPDR: { url: "https://placehold.co/70x20/000000/ffffff?text=SPDR", width: 70, height: 20 },
  ORC: { url: "https://placehold.co/60x40/5A5A5A/ffffff?text=WOLF", width: 60, height: 70 },
}
```

> ** Ali's Instruction:** For the `url` values above, please **replace the placeholder text** (`https://placehold.co/...`) with the file path to the image you upload into the **`alipics` folder** in the repository. I will then integrate them into the client\!
