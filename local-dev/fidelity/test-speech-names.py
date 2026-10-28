# SPDX-License-Identifier: GPL-2.0-or-later
"""Isolated audit of real speech names, parser and generic configuration layering.

Compile in keeperfx-dev; does not build the engine or touch runtime. File I/O and
sound playback are replaced by in-memory fixtures. Callers' ordering is modeled
explicitly; this is not a live campaign/menu integration test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(source, marker, semicolon=False):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1] + (";" if semicolon else "")
    raise AssertionError(marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="New directory for generated fixture, log and source-hash receipt")
    args = parser.parse_args()
    config = (ROOT / "src/config.c").read_text()
    sounds = (ROOT / "src/config_sounds.c").read_text()
    header = (ROOT / "src/gui_soundmsgs.h").read_text()
    prefix = r'''
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <map>
#include <vector>
using TbBool = bool;
constexpr int COMMAND_WORD_LEN = 256, CnfLd_Standard = 0,
    CnfLd_AcceptPartial = 1, CnfLd_IgnoreErrors = 2;
static int text_line_number, warnings;
#define SYNCDBG(...) ((void)0)
#define WARNLOG(...) (++warnings)
struct NamedCommand { const char* name; int num; };
static void sound_manager_save_snapshot() {}
static void sound_save_id_redirect_snapshot() {}
static void sound_manager_restore_snapshot() {}
static void sound_restore_id_redirect_snapshot() {}
static void cache_common_sound_ids() {}
struct ConfigFileData {
    const char* filename;
    TbBool (*load_func)(const char*, unsigned short);
    void (*pre_load_func)();
    void (*post_load_func)();
};
struct ModConfigItem { const char* name; struct { int mod_dir; } state; };
static struct {
    long after_base_cnt, after_campaign_cnt, after_map_cnt;
    ModConfigItem after_base_item[1], after_campaign_item[1], after_map_item[1];
} mods_conf;
enum { FGrp_FxData, FGrp_CmpgConfig, FGrp_CmpgLvls };
static unsigned long selected_level = 1;
static std::map<std::string, std::string> fixture_files;
static std::vector<std::string> loaded;
static const char* language = "eng";
static char* prepare_file_path(int group, const char*) {
    static char path[32];
    snprintf(path, sizeof(path), "%s", group == FGrp_FxData ? "base" : "campaign");
    return path;
}
static unsigned long get_selected_level_number() { return selected_level; }
static char* get_game_file_path_fmt(int, const char*, unsigned long level, const char*) {
    static char path[32];
    snprintf(path, sizeof(path), "map%05lu", level);
    return path;
}
static TbBool fixture_load(const char*, unsigned short);
static void load_config_for_mod_list(const ConfigFileData* data, unsigned short flags,
                                     const ModConfigItem* items, long count) {
    for (long i = 0; i < count; ++i)
        if (items[i].state.mod_dir) data->load_func(items[i].name, flags);
}
static ConfigFileData keeper_sounds_file_data = {"sounds.cfg", fixture_load, nullptr, nullptr};
'''
    body = block(header, "enum TbSpeechMessages", True)
    body += r'''
static char g_speech_overrides[SMsg_MAX][512];
static char campaign_speech_overrides[SMsg_MAX][512];
static TbBool campaign_speech_snapshot_valid;
'''
    body += block(sounds, "static const struct NamedCommand speech_desc[]", True)
    body += "\n" + "\n".join(block(config, marker) for marker in (
        "long get_id(", "TbBool skip_conf_to_next_line(", "TbBool skip_conf_spaces(",
        "TbBool conf_get_block_name(", "TbBool iterate_conf_blocks(",
        "int get_conf_parameter_single(", "TbBool load_config("))
    body += "\n" + "\n".join(block(sounds, marker) for marker in (
        "static TbBool parse_speech_section(", "static void parse_language_speech_sections(",
        "TbBool load_sounds_config(void)", "void sound_save_campaign_snapshot(void)",
        "void sound_restore_to_campaign_snapshot(void)"))
    tests = r'''
static void parse(std::string input) {
    const auto length = input.size();
    input.resize(length + 256, '\0');
    parse_language_speech_sections(input.data(), length, "fixture", 0, language);
}
static TbBool fixture_load(const char* path, unsigned short) {
    loaded.emplace_back(path);
    const auto it = fixture_files.find(path);
    if (it == fixture_files.end()) return false;
    parse(it->second); return true;
}
static void expect(int id, const char* value) { assert(!strcmp(g_speech_overrides[id], value)); }
int main() {
    static_assert(SMsg_MAX == 126, "Update bounds fixture for changed actual enum");
    load_sounds_config();
    int named = 0;
    for (const auto* entry = speech_desc; entry->name; ++entry) {
        const auto value = std::string("named-") + entry->name + ".wav";
        parse(std::string("[speech]\n") + entry->name + " = " + value + "\n");
        expect(entry->num, value.c_str());
        std::string lower(entry->name);
        for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        parse("[speech]\n" + lower + " = case.wav\n"); expect(entry->num, "case.wav");
        ++named;
    }
    assert(named == 109 && warnings == 0);
    load_sounds_config();
    // Native IDs 110..125 have no named aliases and must remain valid numerically.
    for (int id = 1; id < SMsg_MAX; ++id) {
        parse("[speech]\n" + std::to_string(id) + " = numeric.wav\n");
        expect(id, "numeric.wav");
    }
    load_sounds_config();
    parse("[speech]\n0 = bad.wav\n126 = bad.wav\n-1 = bad.wav\n1junk = bad.wav\nSMsg_GoldNotEnough = bad.wav\nGoldNotEnough = valid.wav\n");
    expect(0, ""); expect(1, ""); expect(SMsg_GoldNotEnough, "valid.wav");
    assert(warnings == 3); warnings = 0;
    parse("[speecheng]\nGoldNotEnough = english.wav\n[speechfre]\nGoldNotEnough = french.wav\n[speech]\nGoldNotEnough = generic.wav\n");
    expect(SMsg_GoldNotEnough, "english.wav");
    language = "fre";
    parse("[speecheng]\nGoldNotEnough = wrong.wav\n[speechfre]\ngoldnotenough = francais.wav\n");
    expect(SMsg_GoldNotEnough, "francais.wav");
    sound_save_campaign_snapshot();
    parse("[speech]\nPayday = map-only.wav\nGoldNotEnough = map.wav\n");
    sound_restore_to_campaign_snapshot();
    expect(SMsg_GoldNotEnough, "francais.wav"); expect(SMsg_Payday, "");

    fixture_files = {
        {"base", "[speech]\nGoldNotEnough = base.wav\n"},
        {"after-base", "[speech]\nGoldNotEnough = after-base.wav\n"},
        {"campaign", "[speech]\nGoldNotEnough = campaign.wav\nLevelWon = campaign-win.wav\n"},
        {"after-campaign", "[speech]\nGoldNotEnough = after-campaign.wav\n"},
        {"map00001", "[speech]\nGoldNotEnough = map.wav\nPayday = map-only.wav\n"},
        {"after-map", "[speech]\nGoldNotEnough = after-map.wav\n"}
    };
    mods_conf.after_base_cnt = mods_conf.after_campaign_cnt = mods_conf.after_map_cnt = 1;
    mods_conf.after_base_item[0] = {"after-base", {1}};
    mods_conf.after_campaign_item[0] = {"after-campaign", {1}};
    mods_conf.after_map_item[0] = {"after-map", {1}};
    loaded.clear(); load_sounds_config();
    assert((loaded == std::vector<std::string>{"base", "after-base", "campaign", "after-campaign", "map00001", "after-map"}));
    expect(SMsg_GoldNotEnough, "after-map.wav");
    sound_save_campaign_snapshot();
    // main_game restores first, then load_stats_files calls load_sounds_config.
    selected_level = 2; sound_restore_to_campaign_snapshot(); load_sounds_config();
    expect(SMsg_GoldNotEnough, "after-map.wav"); expect(SMsg_Payday, "");
    expect(SMsg_LevelWon, "campaign-win.wav");
    assert(!campaign_speech_snapshot_valid); // Generic reload is the active speech lifetime boundary.
    // Changing campaign/language also clears entries not provided by new layers.
    fixture_files.clear(); language = "ger"; load_sounds_config();
    for (int id = 0; id < SMsg_MAX; ++id) expect(id, "");

    // Characterize existing caller-order caveats; these are NOT acceptance assertions.
    fixture_files = {{"base", "[speech]\nGoldNotEnough = base.wav\n"},
        {"map00001", "[speech]\nGoldNotEnough = map.wav\nPayday = map-only.wav\n"},
        {"after-map", "[speech]\nGoldNotEnough = after-map.wav\n"}};
    selected_level = 1; load_sounds_config(); expect(SMsg_GoldNotEnough, "after-map.wav");
    fixture_load("map00001", 0); // load_level_file applies per-level sounds again.
    expect(SMsg_GoldNotEnough, "map.wav");
    load_sounds_config(); // sound_reset_to_fxdata_baseline uses this generic reload.
    expect(SMsg_Payday, "map-only.wav");
    assert(warnings == 0);
    puts("PASS: actual 109 named IDs, case folding, actual enum bounds, 125 numeric IDs, language precedence, snapshots, generic layer order, map/campaign reload clearing");
    puts("CHARACTERIZED: late explicit map reload overrides after-map; generic baseline reload retains selected map overrides (modeled callers, no playback claim)");
}
'''
    source = prefix + body + tests
    with tempfile.TemporaryDirectory(prefix="keeperfx-speech-names-") as directory:
        out = args.output.resolve() if args.output else Path(directory) / "report"
        out.mkdir(parents=True, exist_ok=False)
        (out / "speech-names.cpp").write_text(source)
        binary = Path(directory) / "test"
        subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-fsanitize=address,undefined", "-g", str(out / "speech-names.cpp"), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        (out / "speech-names.log").write_text(result.stdout + result.stderr)
        print(result.stdout, end="")
        print(result.stderr, end="")
        result.check_returncode()
        paths = ["src/config.c", "src/config.h", "src/config_sounds.c", "src/gui_soundmsgs.h",
                 "src/config_campaigns.c", "src/main_game.c", "src/dungeon_stats.c", "src/lvl_filesdk1.c",
                 "src/game_loop.c", "local-dev/fidelity/test-speech-names.py"]
        record = {"schema": 1, "passed": True, "namedIds": 109, "numericIds": 125,
                  "sanitizers": ["address", "undefined"],
                  "scope": "Actual enum, name table, get_id, speech parser, reset/snapshot and generic load_config functions; in-memory file fixtures and modeled call sequences; no runtime/playback claim",
                  "source": [{"path": p, "sha256": hashlib.sha256((ROOT / p).read_bytes()).hexdigest()} for p in paths],
                  "observedExistingCaveats": ["Explicit late map load overrides after_map mod priority", "Generic baseline reset can reload selected map speech"],
                  "log": "speech-names.log"}
        (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
