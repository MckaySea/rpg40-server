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
| **Yousaf** | **Dialogue:** Write out what NPCs are going to say and label who is saying it. Determine which NPCs will give quests. Also, please name the NPCs as you wish. | To Do |
| **Ali** | **Visuals & Images:** Find and prepare visuals/images for NPCs, monsters, and objects. I will post a list of required assets for you to find soon. | To Do |
| **McKay** | **Feature development:** Change spawn rates on some maps, so you arent trapped behind blocked areas and cant move to the desired area that was intended | To Do |
| **McKay** | **Feature development:** Interaction system for npcs/objects/monsters | To Do |
| **McKay** | **Feature development:** Character customization | In the air |
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
