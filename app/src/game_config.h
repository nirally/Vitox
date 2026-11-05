// Vitox 0.0.30.0: per-game settings (the library's game menu, opened with the Menu button on a card). Each value either
// follows the global Settings ("Global") or overrides it for that title only, at the next start of that title.
// Saved in LocalState\vitox-games.txt as [TITLE_ID] sections; a title with only "Global" values has no section.
#pragma once

#include <string>

namespace vitox {

struct GameConfig {
    float resolution = 0; // internal resolution multiplier; 0 = Global
    int async = -1;       // asynchronous pipeline compilation: -1 Global, 0 off, 1 on
    int prewarm = -1;     // pipeline pre-warm at launch: -1 Global, 0 off, 1 on
    int stats = -1;       // performance overlay: -1 Global, 0 off, 1 FPS only, 2 full
    bool is_global() const { return resolution == 0 && async < 0 && prewarm < 0 && stats < 0; }
};

void game_config_load(const std::wstring &local_state);
GameConfig game_config(const std::string &title_id);
void game_config_set(const std::string &title_id, const GameConfig &c); // saved at once ("all Global" removes the section)
void game_config_remove(const std::string &title_id);

} // namespace vitox
