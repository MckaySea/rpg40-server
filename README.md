McKay Seamons - https://gemini.google.com/share/72118c934ca1
Nicola
Yousaf
Ali


This is an "open ended assignment" meaning it will be graded by hand, and your
grade will be based on how many design criteria you fulfill. You will work
with a partner on this assignment, so my advice is to figure out early on who
will be responsible for each design criteria, and to set deadlines ahead of
the final deadline for when they'll be done. If your partner flakes, then
you'll have to pick up the work.

Purpose: To teach you how to work on programming assignments with other people
and to get experienced writing code on longer projects rather than little
functions or toy assignments. Also to learn how the design process works a
bit.

Design Criteria:
1) World Map that the user can move around -- The user can move through multiple zones. we have an overarching world map that allows users to travel to different zones as well. There are over 10 zones. I implemented a* using Chebyshev distance for 8 directional movement opposed to manhattan distance but the focus on diagonal movement made me change to octile distance for a more natural feel and looks way better for interpolation on the client.
2) Combat of some sort, and some way of winning the game -- Our game is an open ended mmorpg, the way you "Win" is by progressing your character through skills like mining, fishing, woodcutting, cooking, crafting and combat. You get gear through crafting and killing mobs sometimes even having the chance to get a drop with additional effects that can range from a list of about one hundred effects. Same goes with crafting. You beat the game by fighting and defeating the zone bosses and making your character as overpowered as possible!
3) Must have colors (Use #include "/public/colors.h") -- Game has a front end through javascript that includes multiple differnt sprites and colors - https://github.com/MckaySea/rpg-react-client
4) Must have 5 puzzles to win the game -- the puzzles are the players figuring out the world around them and navigating how to make their characters as strong as possible. There are many hidden secrets throughout the world as well such as skillbooks you can as a super rare drop and secret areas where you can find rare resource nodes. Players can trade items amongst each other too, creating a dynamic experience.
5) 40 lines of dialogue and/or descriptions of the world -- our npcs have hundreds of lines of dialogue as seeen in gamedata.cpp

Extra Credit:
1) Nonblocking I/O (also in colors.h)
2) Compose Original Music and Cover Art (Make it yourself) Upload music to
Youtube.
3) Inventory System

Each one of these is a letter grade. If you do none, you get a 0%, if you do
one you get an F (50%), two a D (60%), three a C (70%), four a B (80%), five
an A (100%), and each extra credit point is an extra 10% added to your grade.
So the max grade is 130%.
~

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
