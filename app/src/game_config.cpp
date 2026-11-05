// Vitox 0.0.30.0: per-game settings (see game_config.h).
#include "game_config.h"

#include "common.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace vitox {

namespace {
std::mutex g_m;
std::wstring g_dir;
std::map<std::string, GameConfig> g_games;

std::wstring file() { return g_dir + L"\\vitox-games.txt"; }

void save_locked() {
    if (g_dir.empty())
        return;
    std::ofstream f(file(), std::ios::trunc);
    f << "# Vitox per-game settings (library: Menu button on a game). Missing keys follow the global Settings.\n";
    for (const auto &[tid, c] : g_games) {
        if (c.is_global())
            continue;
        f << "[" << tid << "]\n";
        if (c.resolution > 0) {
            char b[32];
            snprintf(b, sizeof(b), "%.1f", c.resolution);
            f << "resolution_multiplier=" << b << "\n";
        }
        if (c.async >= 0)
            f << "async_pipeline_compilation=" << c.async << "\n";
        if (c.prewarm >= 0)
            f << "pipeline_prewarm=" << c.prewarm << "\n";
        if (c.stats >= 0)
            f << "stats_overlay=" << (c.stats == 0 ? "off" : c.stats == 1 ? "fps" : "full") << "\n";
    }
}
} // namespace

void game_config_load(const std::wstring &local_state) {
    std::lock_guard<std::mutex> lock(g_m);
    g_dir = local_state;
    g_games.clear();
    std::ifstream f(file());
    std::string section;
    for (std::string line; f && std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t eq = line.find('=');
        if (section.empty() || eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        GameConfig &c = g_games[section];
        if (k == "resolution_multiplier") {
            const float m = std::strtof(v.c_str(), nullptr);
            c.resolution = std::isfinite(m) && m >= 1.0f && m <= kResolutionMaxHard ? resolution_snap(m, kResolutionMaxHard) : 0;
        } else if (k == "async_pipeline_compilation" && (v == "0" || v == "1")) {
            c.async = v == "1";
        } else if (k == "pipeline_prewarm" && (v == "0" || v == "1")) {
            c.prewarm = v == "1";
        } else if (k == "stats_overlay") {
            c.stats = v == "off" ? 0 : v == "fps" ? 1 : v == "full" ? 2 : -1;
        }
    }
    log_line(Log::Lifecycle, "vitox games: %u title(s) with their own settings", static_cast<unsigned>(g_games.size()));
}

GameConfig game_config(const std::string &title_id) {
    std::lock_guard<std::mutex> lock(g_m);
    const auto it = g_games.find(title_id);
    return it == g_games.end() ? GameConfig{} : it->second;
}

void game_config_set(const std::string &title_id, const GameConfig &c) {
    std::lock_guard<std::mutex> lock(g_m);
    if (c.is_global())
        g_games.erase(title_id);
    else
        g_games[title_id] = c;
    save_locked();
}

void game_config_remove(const std::string &title_id) {
    std::lock_guard<std::mutex> lock(g_m);
    g_games.erase(title_id);
    save_locked();
}

} // namespace vitox
