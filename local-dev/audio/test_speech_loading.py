# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise actual sound config loaders, registry and snapshots with synthetic I/O.

Run in keeperfx-dev. No game data or audio device is needed. Config file I/O,
path roots and WAV decoding are fixtures; layer selection, config parsing,
SoundManager registration/clearing/snapshots and bank snapshot operations are
compiled from the current source. Caller wiring has separate source assertions.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
block = runpy.run_path(str(ROOT / "local-dev/fidelity/test-speech-names.py"))["block"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    files = {p: (ROOT / p).read_text() for p in (
        "src/config.c", "src/config.h", "src/config_sounds.c", "src/gui_soundmsgs.h",
        "src/sound_manager.cpp", "src/bflib_sndlib.cpp", "src/dungeon_stats.c",
        "src/lvl_filesdk1.c", "src/game_saves.c")}
    config, sounds = files["src/config.c"], files["src/config_sounds.c"]
    manager, bank = files["src/sound_manager.cpp"], files["src/bflib_sndlib.cpp"]
    prefix = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
using TbBool = bool;
using LevelNumber = int32_t;
using SoundSmplTblID = int;
constexpr int COMMAND_WORD_LEN = 256, MIN_CONFIG_FILE_SIZE = 4;
static int text_line_number, warnings, errors, g_speech_queue_limit = 4;
#define SYNCDBG(...) ((void)0)
#define WARNLOG(...) (++warnings)
#define WARNMSG(...) (++warnings)
#define ERRORLOG(...) (++errors)
#define set_flag(value, flags) ((value) |= (flags))
#define MODS_DIR_NAME "mods"
struct NamedCommand { const char* name; int num; };
struct ConfigFileData {
 const char* filename; TbBool (*load_func)(const char*, unsigned short);
 void (*pre_load_func)(); TbBool (*post_load_func)();
};
struct ModExistState { int mod_dir, fx_data, cmpg_config, cmpg_lvls; };
struct ModConfigItem { const char* name; ModExistState state; };
static struct {
 long after_base_cnt, after_campaign_cnt, after_map_cnt;
 ModConfigItem after_base_item[2], after_campaign_item[2], after_map_item[2];
} mods_conf;
enum { FGrp_Main, FGrp_FxData, FGrp_CmpgConfig, FGrp_CmpgLvls };
static LevelNumber selected_level = 1;
static const char* levels_root = "campaign/levels";
static const char* configs_root = "campaign/config";
static struct { int lang_id; } install_info;
static const char* get_language_lwrstr(int id) { return id == 1 ? "fre" : "eng"; }
static LevelNumber get_selected_level_number() { return selected_level; }
static std::map<std::string, std::string> fixture_files;
static std::vector<std::string> loaded;
static char* path(const char* mod, int group, const char* name) {
 static char buffer[512];
 const char* sub = group == FGrp_FxData ? "fxdata" : group == FGrp_CmpgConfig ? configs_root : group == FGrp_CmpgLvls ? levels_root : "";
 snprintf(buffer, sizeof(buffer), "%s%s%s%s%s", mod, *mod && *sub ? "/" : "", sub, *sub || *mod ? "/" : "", name);
 return buffer;
}
static char* prepare_file_path(int group, const char* name) { return path("", group, name); }
static char* prepare_file_path_mod(const char* mod, int group, const char* name) { return path(mod, group, name); }
static char* get_game_file_path_fmt(int group, const char* format, ...) {
 char name[128]; va_list ap; va_start(ap, format); vsnprintf(name,sizeof(name),format,ap); va_end(ap);
 return path("",group,name);
}
static char* get_mod_file_path_fmt(const char* mod, int group, const char* format, ...) {
 char name[128]; va_list ap; va_start(ap, format); vsnprintf(name,sizeof(name),format,ap); va_end(ap);
 return path(mod,group,name);
}
static long LbFileLengthRnc(const char* name) {
 loaded.emplace_back(name); auto it=fixture_files.find(name); return it == fixture_files.end() ? -1 : it->second.size();
}
static long LbFileLoadAt(const char* name, char* buf) {
 const auto& value=fixture_files.at(name); memcpy(buf,value.data(),value.size()); return value.size();
}
constexpr int SStack_Limit = 0, SStack_Duck = 1;
struct SoundStackPolicy { int mode, max_instances; };
static std::vector<std::string> g_custom_bank;
static std::unordered_map<int,int> g_id_redirects, g_id_redirects_snapshot;
static std::unordered_map<int,SoundStackPolicy> g_stack_policies, g_stack_policies_snapshot;
static std::unordered_map<int,int> g_tick_samples_last_tick;
static size_t g_custom_bank_watermark;
static void detach_custom_sound_sources(size_t) {}
static int custom_sound_bank_size() { return g_custom_bank.size(); }
static int get_custom_offset() { return 2000; }
static TbBool custom_sound_load_wav(const char* filename, int index) {
 if (!strcmp(filename,"missing.wav")) return false;
 assert(index == static_cast<int>(g_custom_bank.size())); g_custom_bank.emplace_back(filename); return true;
}
extern "C" void custom_sound_bank_clear();
class SoundManager {
 struct CustomSoundEntry { std::string filepath; int sample_id; bool loaded; };
 struct SoundEntry { int sample_id, count; };
 std::map<std::string,CustomSoundEntry> custom_sounds_, snapshot_custom_sounds_;
 std::map<std::string,SoundEntry> sound_registry_, snapshot_registry_;
 std::vector<int> creature_sound_overrides_, snapshot_creature_overrides_;
 size_t total_custom_sounds_=0, snapshot_total_custom_sounds_=0;
 int next_custom_sample_id_=0;
 bool snapshot_valid_=false;
public:
 int loadCustomSound(const std::string&,const std::string&);
 bool loadWavFile(const std::string&,int);
 int getSoundId(const char*) const;
 bool registerSound(const char*,int,int);
 void clearCustomSounds(); void clearRegistry(); void saveSnapshot(); void restoreSnapshot();
};
'''
    body = block(files["src/config.h"], "enum TbConfigLoadFlags", True)
    body += "\n" + block(files["src/gui_soundmsgs.h"], "enum TbSpeechMessages", True)
    body += r'''
static char g_speech_overrides[SMsg_MAX][512], campaign_speech_overrides[SMsg_MAX][512];
static TbBool campaign_speech_snapshot_valid;
'''
    body += "\n".join(block(bank, marker) for marker in (
        'extern "C" void custom_sound_bank_clear(', 'extern "C" void sound_register_id_redirect(',
        'extern "C" void sound_register_stack_policy(', 'extern "C" void sound_save_id_redirect_snapshot(',
        'extern "C" void sound_restore_id_redirect_snapshot('))
    body += "\n" + "\n".join(block(manager, marker) for marker in (
        "SoundSmplTblID SoundManager::loadCustomSound(", "bool SoundManager::loadWavFile(",
        "SoundSmplTblID SoundManager::getSoundId(", "bool SoundManager::registerSound(",
        "void SoundManager::clearCustomSounds(", "void SoundManager::clearRegistry(",
        "void SoundManager::saveSnapshot(", "void SoundManager::restoreSnapshot("))
    body += r'''
static SoundManager sm;
static void sound_manager_clear_custom_sounds() { sm.clearCustomSounds(); }
static void sound_manager_clear_registry() { sm.clearRegistry(); }
static void sound_manager_save_snapshot() { sm.saveSnapshot(); }
static void sound_manager_restore_snapshot() { sm.restoreSnapshot(); }
static bool sound_manager_register(const char* n,int id,int count) { return sm.registerSound(n,id,count); }
static int sound_manager_load_named_sound(const char* n,const char* p,int count) {
 assert(count == 1); return sm.loadCustomSound(n,p);
}
static int cached_refusal;
static TbBool cache_common_sound_ids() { cached_refusal=sm.getSoundId("REFUSAL"); return true; }
static TbBool load_sounds_config_file(const char*,unsigned short);
static const ConfigFileData keeper_sounds_file_data={"sounds.cfg",load_sounds_config_file,nullptr,cache_common_sound_ids};
TbBool load_sounds_config_for_level(LevelNumber);
'''
    body += "\n" + block(sounds, "static const struct NamedCommand speech_desc[]", True)
    body += "\n" + "\n".join(block(config, marker) for marker in (
        "long get_id(", "TbBool skip_conf_to_next_line(", "TbBool skip_conf_spaces(",
        "TbBool conf_get_block_name(", "TbBool iterate_conf_blocks(", "int get_conf_parameter_single(",
        "short get_level_fgroup(", "static void load_config_for_mod(", "static void load_config_for_mod_list(",
        "static TbBool load_config_layers(", "TbBool load_config(", "TbBool load_config_for_level(", "TbBool load_global_config("))
    body += "\n" + block(sounds, "enum StackTokenResult", True)
    body += "\n" + "\n".join(block(sounds, marker) for marker in (
        "static enum StackTokenResult parse_stack_token(", "static void register_stack_policy_range(",
        "static TbBool parse_sound_line(", "static TbBool parse_sounds_section(", "static TbBool parse_system_section(",
        "static TbBool parse_speech_section(", "static void parse_language_speech_sections(",
        # Skip the forward declaration when extracting the definition.
        "static TbBool load_sounds_config_file(const char *fname, unsigned short flags)\n{",
        "TbBool load_sounds_config(void)", "TbBool load_sounds_config_for_level(",
        "void sound_save_campaign_snapshot(void)", "void sound_restore_to_campaign_snapshot(void)",
        "void sound_reset_to_fxdata_baseline(void)"))
    tests = r'''
static void expect(int id,const char* value) { assert(!strcmp(g_speech_overrides[id],value)); }
static void expect_sound(const char* name,const char* file) {
 const int index=sm.getSoundId(name)-get_custom_offset();
 assert(index >= 0 && index < static_cast<int>(g_custom_bank.size()));
 assert(g_custom_bank[index] == file);
}
int main() {
 fixture_files={
 {"fxdata/sounds.cfg","[sounds]\nREFUSAL = 119\nBASE = base.wav\n[speech]\nGoldNotEnough = base.wav\n"},
 {"mods/early/sounds.cfg","[sounds]\nROOT = root.wav\nDUPLICATE = root-old.wav\n[speecheng]\n1 = root-eng.wav\n"},
 {"mods/early/fxdata/sounds.cfg","[sounds]\nEARLY = early.wav\nDUPLICATE = fxdata-new.wav\n[speecheng]\nGoldNotEnough = early.wav\n"},
 {"mods/middle/fxdata/sounds.cfg","[sounds]\nMIDDLE = middle.wav\n[speech]\n1 = middle-global.wav\n"},
 {"mods/late/fxdata/sounds.cfg","[sounds]\nLATE = late.wav\n[speech]\nGoldNotEnough = late.wav\n[speecheng]\n125 = late-eng.wav\n"},
 {"campaign/config/sounds.cfg","[sounds]\nCAMPAIGN = campaign.wav\n[speech]\nLevelWon = campaign.wav\n"},
 {"campaign/levels/map00001.sounds.cfg","[system]\nSpeechQueueLimit = 8\n[sounds]\nMAPONE = map1.wav\n72 = redirect1.wav\nREFUSAL = 18\n[speech]\nGoldNotEnough = map1.wav\nPayday = only1.wav\n125 = map1.wav\n"},
 {"campaign/levels/map00002.sounds.cfg","[sounds]\nMAPTWO = map2.wav\n72 = redirect2.wav\n[speech]\nGoldNotEnough = map2.wav\n"},
 {"mods/late/campaign/config/sounds.cfg","[speech]\nLevelWon = mod-campaign.wav\n"},
 {"mods/late/campaign/levels/map00001.sounds.cfg","[speecheng]\n125 = mod-map1.wav\n"},
 {"mods/late/campaign/levels/map00002.sounds.cfg","[speecheng]\n125 = mod-map2.wav\n"}
 };
 mods_conf.after_base_cnt=mods_conf.after_campaign_cnt=mods_conf.after_map_cnt=1;
 mods_conf.after_base_item[0]={"early",{1,1,1,1}};
 mods_conf.after_campaign_item[0]={"middle",{1,1,1,1}};
 mods_conf.after_map_item[0]={"late",{1,1,1,1}};
 assert(load_sounds_config());
 expect(SMsg_GoldNotEnough,"late.wav"); expect(125,"mod-map1.wav"); expect(SMsg_Payday,"only1.wav");
 assert(cached_refusal==18 && g_speech_queue_limit==8);
 expect_sound("ROOT","root.wav"); expect_sound("MAPONE","map1.wav");
 expect_sound("DUPLICATE","fxdata-new.wav");
 const auto bank_before_repeat=g_custom_bank;
 const auto duplicate=sm.getSoundId("DUPLICATE");
 assert(sm.loadCustomSound("DUPLICATE","fxdata-new.wav")==duplicate);
 assert(g_custom_bank==bank_before_repeat);
 assert(sm.loadCustomSound("DUPLICATE","missing.wav")==-1);
 assert(sm.getSoundId("DUPLICATE")==duplicate && g_custom_bank==bank_before_repeat);
 assert(warnings==2); warnings=0; // Both actual load failure diagnostics ran.
 assert(g_custom_bank[g_id_redirects.at(72)-get_custom_offset()]=="redirect1.wav");
 assert(std::count(loaded.begin(),loaded.end(),"campaign/levels/map00001.sounds.cfg")==1);
 // Creature custom registration follows sound loading, with real bank IDs.
 const int creature_id=sm.loadCustomSound("IMP_FOOT","imp-foot.wav");
 expect_sound("IMP_FOOT","imp-foot.wav"); assert(creature_id>=2000);
 sound_save_campaign_snapshot();
 // Save load explicitly selects map2 while the frontend still selects map1.
 loaded.clear(); assert(load_sounds_config_for_level(2)); assert(selected_level==1);
 expect(SMsg_Payday,""); expect(125,"mod-map2.wav");
 assert(sm.getSoundId("MAPONE")==0 && sm.getSoundId("IMP_FOOT")==0);
 expect_sound("ROOT","root.wav"); expect_sound("MAPTWO","map2.wav");
 assert(cached_refusal==119 && g_speech_queue_limit==4);
 assert(g_custom_bank[g_id_redirects.at(72)-get_custom_offset()]=="redirect2.wav");
 sm.loadCustomSound("IMP_FOOT","imp-foot2.wav"); expect_sound("IMP_FOOT","imp-foot2.wav");
 // Return to menu while a map remains selected: no campaign or map config reads.
 loaded.clear(); sound_reset_to_fxdata_baseline();
 expect(SMsg_Payday,""); expect(SMsg_LevelWon,""); expect(125,"late-eng.wav"); expect(1,"middle-global.wav");
 assert(sm.getSoundId("CAMPAIGN")==0 && sm.getSoundId("MAPTWO")==0 && sm.getSoundId("IMP_FOOT")==0);
 expect_sound("ROOT","root.wav"); expect_sound("LATE","late.wav"); expect_sound("DUPLICATE","fxdata-new.wav");
 assert(g_id_redirects.empty() && cached_refusal==119 && g_speech_queue_limit==4);
 for(const auto& p:loaded) assert(p.find("campaign/")==std::string::npos);
 const auto baseline_bank=g_custom_bank;
 sm.loadCustomSound("AFTER_MENU","extra.wav"); sound_restore_to_campaign_snapshot();
 assert(g_custom_bank==baseline_bank && sm.getSoundId("AFTER_MENU")==0);
 expect_sound("ROOT","root.wav"); expect(SMsg_Payday,"");
 // Optional map absence is harmless; global and campaign layers still apply.
 assert(load_sounds_config_for_level(3)); expect(SMsg_Payday,""); expect(125,"late-eng.wav");
 assert(g_id_redirects.empty() && sm.getSoundId("MAPONE")==0);
 // Selected language wins only inside its file, preserving native fallbacks.
 install_info.lang_id=1; load_sounds_config_for_level(1);
 expect(125,"map1.wav"); expect(SMsg_GoldNotEnough,"late.wav");
 sound_reset_to_fxdata_baseline(); expect(125,"");
 // NativeAudio removes the enabled mod entry; reset cannot resurrect it.
 mods_conf.after_base_cnt=0; sound_reset_to_fxdata_baseline(); assert(sm.getSoundId("ROOT")==0);
 mods_conf.after_campaign_cnt=mods_conf.after_map_cnt=0; sound_reset_to_fxdata_baseline();
 expect(SMsg_GoldNotEnough,"base.wav"); expect(1,""); assert(sm.getSoundId("LATE")==0);
 // The same actual loader honours freeplay and fallback LEVELS_LOCATION roots.
 configs_root="levels"; levels_root="levels";
 fixture_files["levels/map00007.sounds.cfg"]="[speech]\nPayday = freeplay.wav\n";
 load_sounds_config_for_level(7); expect(SMsg_Payday,"freeplay.wav");
 configs_root="campgns/keeporig"; levels_root="campgns/keeporig";
 fixture_files["campgns/keeporig/map00001.sounds.cfg"]="[speech]\nPayday = fallback.wav\n";
 load_sounds_config_for_level(1); expect(SMsg_Payday,"fallback.wav");
 assert(warnings==0 && errors==0);
 puts("PASS: actual loaders/parsers/registry and bank snapshots; named/numeric speech, mod priority, map1->saved map2, optional map, menu global layers, native audio, languages, freeplay/fallback roots, custom creature bank IDs");
}
'''
    # This wiring check complements (rather than replaces) the behavioral fixture.
    assert "load_level_sounds_config(" not in block(files["src/lvl_filesdk1.c"], "static TbBool load_level_file(")
    stats = block(files["src/dungeon_stats.c"], "TbBool load_stats_files_for_level(")
    assert stats.index("load_sounds_config_for_level(sound_level)") < stats.index("load_default_creaturemodel_config(")
    assert "load_stats_files_for_level(centry->level_num)" in block(files["src/game_saves.c"], "int load_game_chunks(")
    with tempfile.TemporaryDirectory(prefix="keeperfx-speech-loading-") as directory:
        out = args.output.resolve() if args.output else Path(directory)/"report"
        out.mkdir(parents=True, exist_ok=False)
        source = out/"speech-loading.cpp"
        source.write_text(prefix+body+tests)
        binary = Path(directory)/"test"
        subprocess.run(shlex.split(os.environ.get("CXX","c++"))+[
            "-std=c++17","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
            "-fsanitize=address,undefined","-g",str(source),"-o",str(binary)],check=True)
        result = subprocess.run([str(binary)],capture_output=True,text=True)
        (out/"speech-loading.log").write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr,end="")
        result.check_returncode()
        receipt={"schema":1,"passed":True,"sanitizers":["address","undefined"],
            "scope":__doc__,"source":[{"path":p,"sha256":hashlib.sha256((ROOT/p).read_bytes()).hexdigest()} for p in files]}
        (out/"verification.json").write_text(json.dumps(receipt,indent=2)+"\n")


if __name__ == "__main__":
    main()
