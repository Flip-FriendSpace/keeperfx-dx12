# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile actual config parsers and exercise language selection and state lifetime."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(marker)


def main():
    config = (ROOT / "src/config.c").read_text()
    sounds = (ROOT / "src/config_sounds.c").read_text()
    prefix = r'''
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <iterator>
#include <string>
using TbBool = bool;
constexpr int COMMAND_WORD_LEN = 256, SMsg_MAX = 200, CnfLd_Standard = 0;
static int text_line_number, warnings;
static char g_speech_overrides[SMsg_MAX][512];
static char campaign_speech_overrides[SMsg_MAX][512];
static TbBool campaign_speech_snapshot_valid;
static int keeper_sounds_file_data;
#define SYNCDBG(...) ((void)0)
#define WARNLOG(...) (++warnings)
static int speech_desc;
static int get_id(int, const char*) { return -1; }
static TbBool load_config(int*, int) { return true; }
static void sound_manager_save_snapshot() {}
static void sound_save_id_redirect_snapshot() {}
static void sound_manager_restore_snapshot() {}
static void sound_restore_id_redirect_snapshot() {}
static void cache_common_sound_ids() {}
'''
    body = "\n".join(function(config, marker) for marker in (
        "TbBool skip_conf_to_next_line(", "TbBool skip_conf_spaces(",
        "TbBool conf_get_block_name(", "TbBool iterate_conf_blocks(",
        "int get_conf_parameter_single("))
    body += "\n" + "\n".join(function(sounds, marker) for marker in (
        "static TbBool parse_speech_section(", "static void parse_language_speech_sections(",
        "TbBool load_sounds_config(void)", "void sound_save_campaign_snapshot(void)",
        "void sound_restore_to_campaign_snapshot(void)"))
    tests = r'''
static void parse(std::string input, const char* language) {
    const auto length = input.size();
    input.resize(length + 256, '\0');
    parse_language_speech_sections(input.data(), length, "fixture", 0, language);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1]);
    assert(file.good());
    std::string package((std::istreambuf_iterator<char>(file)), {});
    // The actual English package must leave every other installed voice bank native.
    for (const char* language : {"fre", "ger", "jpn", "pol", "spa", ""}) {
        load_sounds_config();
        parse(package, language);
        for (int id = 0; id < SMsg_MAX; ++id) assert(!g_speech_overrides[id][0]);
    }
    load_sounds_config(); parse(package, "eng");
    for (int id = 1; id <= 125; ++id) {
        char path[128];
        snprintf(path, sizeof(path), "mods/dk_modern_audio/audio/speech/%04d.wav", id);
        assert(!strcmp(path, g_speech_overrides[id]));
    }
    // Generic sections work as before; the selected language wins independently of order.
    const std::string mixed = "[speechfre]\n1 = french.wav\n[speech]\n1 = generic.wav\n2 = common.wav\n[speecheng]\n1 = english.wav\n";
    load_sounds_config(); parse(mixed, "fre");
    assert(!strcmp(g_speech_overrides[1], "french.wav"));
    assert(!strcmp(g_speech_overrides[2], "common.wav"));
    parse(package, "fre"); // A different-language mod cannot erase an earlier override.
    assert(!strcmp(g_speech_overrides[1], "french.wav"));
    parse("[speech]\n1 = later-map.wav\n", "fre");
    assert(!strcmp(g_speech_overrides[1], "later-map.wav"));
    // Exact names: [s], [speechenglish] and another language cannot be mistaken for [speech].
    load_sounds_config();
    parse("[s]\n1 = wrong.wav\n[speechenglish]\n2 = wrong.wav\n[speechger]\n3 = wrong.wav\n", "eng");
    assert(!g_speech_overrides[1][0] && !g_speech_overrides[2][0] && !g_speech_overrides[3][0]);
    parse("[SpEeChFrE]\r\n1 = case.wav\r\n", "FRE");
    assert(!strcmp(g_speech_overrides[1], "case.wav"));
    // Map overrides must not leak to the next map or to another campaign/language.
    sound_save_campaign_snapshot();
    parse("[speech]\n1 = map.wav\n3 = map-only.wav\n", "fre");
    sound_restore_to_campaign_snapshot();
    assert(!strcmp(g_speech_overrides[1], "case.wav") && !g_speech_overrides[3][0]);
    load_sounds_config(); sound_restore_to_campaign_snapshot();
    for (int id = 0; id < SMsg_MAX; ++id) assert(!g_speech_overrides[id][0]);
    assert(warnings == 0);
    puts("PASS: real parser, 125 English mappings, six native fallbacks, precedence, exact names, snapshot/reset");
}
'''
    with tempfile.TemporaryDirectory(prefix="keeperfx-speech-language-") as directory:
        source = Path(directory) / "speech.cpp"
        binary = Path(directory) / "speech"
        source.write_text(prefix + body + tests)
        subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(ROOT / "config/mods/dk_modern_audio/sounds.cfg")], check=True)


if __name__ == "__main__":
    main()
