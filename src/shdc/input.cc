/*
    code for loading and parsing the input .glsl file with custom-tags
*/
#include "input.h"
#include "types/reflection/type.h"
#include "types/reflection/bindings.h"
#include "types/option.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "fmt/format.h"
#include "pystring.h"

namespace shdc {

using namespace refl;

static int find_slot(const std::map<std::string, int>& map, const std::string& key) {
    auto it = map.find(key);
    if (it != map.end()) {
        return map.at(key);
    } else {
        return -1;
    }
}

int Input::find_ub_slot(const std::string& name) const {
    return find_slot(ub_slots, name);
}

int Input::find_img_slot(const std::string& name) const {
    return find_slot(img_slots, name);
}

int Input::find_smp_slot(const std::string& name) const {
    return find_slot(smp_slots, name);
}

int Input::find_sbuf_slot(const std::string& name) const {
    return find_slot(sbuf_slots, name);
}

const ImageSampleTypeTag* Input::find_image_sample_type_tag(const std::string& tex_name) const {
    auto it = image_sample_type_tags.find(tex_name);
    if (it != image_sample_type_tags.end()) {
        return &image_sample_type_tags.at(tex_name);
    } else {
        return nullptr;
    }
}

const SamplerTypeTag* Input::find_sampler_type_tag(const std::string& smp_name) const {
    auto it = sampler_type_tags.find(smp_name);
    if (it != sampler_type_tags.end()) {
        return &sampler_type_tags.at(smp_name);
    } else {
        return nullptr;
    }
}

static std::string load_file_into_str(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return std::string();
    }
    fseek(f, 0, SEEK_END);
    const size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*) malloc(file_size + 1);
    fread((void*)buf, file_size, 1, f);
    buf[file_size] = 0;
    std::string str(buf);
    free((void*)buf);
    return str;
}

/* removes comments from string
    - FIXME: doesn't detect block-comment in block-comment bugs
    - also removes comments in string literals (no problem for shader langs)
*/
static bool remove_comments(std::string& str) {
    bool in_winged_comment = false;
    bool in_block_comment = false;
    bool maybe_start = false;
    bool maybe_end = false;
    const size_t len = str.length();
    for (size_t pos = 0; pos < len; pos++) {
        const char c = str[pos];
        if (!(in_winged_comment || in_block_comment)) {
            // not currently in a comment
            if (maybe_start) {
                // next character after a '/'
                if (c == '/') {
                    // start of a winged comment
                    in_winged_comment = true;
                    str[pos - 1] = ' ';
                    str[pos] = ' ';
                } else if (c == '*') {
                    // start of a block comment
                    in_block_comment = true;
                    str[pos - 1] = ' ';
                    str[pos] = ' ';
                }
                maybe_start = false;
            } else {
                if (c == '/') {
                    // maybe start of a winged or block comment
                    maybe_start = true;
                }
            }
        } else if (in_winged_comment || in_block_comment) {
            if (in_winged_comment) {
                if ((c == '\r') || (c == '\n')) {
                    // end of line reached
                    in_winged_comment = false;
                } else {
                    str[pos] = ' ';
                }
            } else {
                // in block comment (preserve newlines)
                if ((c != '\r') && (c != '\n')) {
                    str[pos] = ' ';
                }
                if (maybe_end) {
                    if (c == '/') {
                        // end of block comment
                        in_block_comment = false;
                    }
                    maybe_end = false;
                } else {
                    if (c == '*') {
                        // potential end of block comment
                        maybe_end = true;
                    }
                }
            }
        }
    }
    return true;
}

static const std::string module_tag = "@module";
static const std::string ctype_tag = "@ctype";
static const std::string header_tag = "@header";
static const std::string vs_tag = "@vs";
static const std::string fs_tag = "@fs";
static const std::string cs_tag = "@cs";
static const std::string block_tag = "@block";
static const std::string inclblock_tag = "@include_block";
static const std::string end_tag = "@end";
static const std::string prog_tag = "@program";
static const std::string glsl_options_tag = "@glsl_options";
static const std::string hlsl_options_tag = "@hlsl_options";
static const std::string msl_options_tag = "@msl_options";
static const std::string include_tag = "@include";
static const std::string image_sample_type_tag = "@image_sample_type";
static const std::string sampler_type_tag = "@sampler_type";

static bool normalize_pragma_sokol(std::vector<std::string>& toks, std::string &line, int line_index, Input& inp) {
    // Returns true if it saw no errors, even if it did nothing.
    // If it sees #pragma sokol, it modifies both `toks` and `line`
    // in-place so that they no longer contain them.
    if (toks.size() < 2) {
        return true;
    }
    size_t expect_tag_index = 0;
    if (toks[0] == "#pragma" && toks[1] == "sokol") {
        expect_tag_index = 2;
    } else if (toks.size() >= 3 && toks[0] == "#" && toks[1] == "pragma" && toks[2] == "sokol") {
        // The GLSL spec allows whitespace between # and pragma, so we might as well handle it.
        expect_tag_index = 3;
    } else {
        // Not a `#pragma sokol`, but not an error either.
        return true;
    }
    // If it's not a tag, emit an error.
    if (toks.size() <= expect_tag_index || toks[expect_tag_index][0] != '@') {
        inp.out_error = inp.error(line_index, fmt::format(
            "'#pragma sokol' should be followed by a @tag, got `{}`.",
            toks[expect_tag_index]));
        return false;
    }
    toks.erase(toks.begin(), toks.begin() + expect_tag_index);
    // We don't know where in the line itself this is, so just drop everything
    // before the first @.
    auto at_pos = line.find('@');
    assert(at_pos != std::string::npos);
    line.erase(line.begin(), line.begin() + at_pos);
    return true;;
}

// validate source tags for errors, on error returns false and sets error object in inp
static bool validate_module_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = inp.error(line_index, "@module tag must have exactly one arg (@lib name)");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@module tag cannot be inside a tag block (missing @end?).");
        return false;
    }
    if (!inp.module.empty()) {
        inp.out_error = inp.error(line_index, "only one @module tag per file allowed.");
        return false;
    }
    return true;
}

static bool validate_ctype_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 3) {
        inp.out_error = inp.error(line_index, "@ctype tag must have exactly two args (@ctype glsltype ctype)");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@ctype tag cannot be inside a tag block (missing @end?).");
        return false;
    }
    if (!Type::is_valid_glsl_type(tokens[1])) {
        inp.out_error = inp.error(line_index, fmt::format("first arg of @ctype tag must be one of {}", Type::valid_glsl_types_as_str()));
        return false;
    }
    return true;
}

static bool validate_header_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() < 2) {
        inp.out_error = inp.error(line_index, "@header tag must have at least one arg (@header ...)");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@header tag cannot be inside a tag block (missing @end?).");
        return false;
    }
    return true;
}

static bool validate_block_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = inp.error(line_index, "@block tag must have exactly one arg (@block name).");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@block tag cannot be inside other tag block (missing @end?).");
        return false;
    }
    if (inp.snippet_map.count(tokens[1]) > 0) {
        inp.out_error = inp.error(line_index, fmt::format("@block, @vs, @fs and @cs tag names must be unique (@block {}).", tokens[1]));
        return false;
    }
    return true;
}

static bool validate_vs_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = inp.error(line_index, "@vs tag must have exactly one arg (@vs name).");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@vs tag cannot be inside other tag block (missing @end?).");
        return false;
    }
    if (inp.snippet_map.count(tokens[1]) > 0) {
        inp.out_error = inp.error(line_index, fmt::format("@block, @vs, @fs and @cs tag names must be unique (@vs {}).", tokens[1]));
        return false;
    }
    return true;
}

static bool validate_fs_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = inp.error(line_index, "@fs tag must have exactly one arg (@fs name).");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@fs tag cannot be inside other tag block (missing @end?).");
        return false;
    }
    if (inp.snippet_map.count(tokens[1]) > 0) {
        inp.out_error = inp.error(line_index, fmt::format("@block, @vs, @fs and @cs tag names must be unique (@fs {}).", tokens[1]));
        return false;
    }
    return true;
}

static bool validate_cs_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = inp.error(line_index, "@cs tag must have exactly one arg (@cs name).");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@cs tag cannot be inside other tag block (missing @end?).");
        return false;
    }
    if (inp.snippet_map.count(tokens[1]) > 0) {
        inp.out_error = inp.error(line_index, fmt::format("@block, @vs, @fs and @cs tag names must be unique (@cs {}).", tokens[1]));
        return false;
    }
    return true;
}

static bool validate_inclblock_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = inp.error(line_index, "@include_block tag must have exactly one arg (@include_block block_name).");
        return false;
    }
    if (!in_snippet) {
        inp.out_error = inp.error(line_index, "@include_block must be inside a @block, @vs or @fs block.");
        return false;
    }
    if (inp.snippet_map.count(tokens[1]) != 1) {
        inp.out_error = inp.error(line_index, fmt::format("@block '{}' not found for including.", tokens[1]));
        return false;
    }
    return true;
}

static bool validate_end_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if (tokens.size() != 1) {
        inp.out_error = inp.error(line_index, "@end tag must be the only word in a line.");
        return false;
    }
    if (!in_snippet) {
        inp.out_error = inp.error(line_index, "@end tag must come after a @block, @vs or @fs tag.");
        return false;
    }
    return true;
}

static bool validate_program_tag(const std::vector<std::string>& tokens, bool in_snippet, int line_index, Input& inp) {
    if ((tokens.size() != 3) && (tokens.size() != 4)) {
        inp.out_error = inp.error(line_index, "@program tag must have 2 or 3 args (@program name [vs_name fs_name]|[cs_name]).");
        return false;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index, "@program tag cannot be inside a block tag.");
        return false;
    }
    if (inp.programs.count(tokens[1]) > 0) {
        inp.out_error = inp.error(line_index, fmt::format("@program '{}' already defined.", tokens[1]));
        return false;
    }
    if (tokens.size() == 4) {
        if (inp.vs_map.count(tokens[2]) != 1) {
            inp.out_error = inp.error(line_index, fmt::format("@vs '{}' not found for @program '{}'.", tokens[2], tokens[1]));
            return false;
        }
        if (inp.fs_map.count(tokens[3]) != 1) {
            inp.out_error = inp.error(line_index, fmt::format("@fs '{}' not found for @program '{}'.", tokens[3], tokens[1]));
            return false;
        }
    } else {
        if (inp.cs_map.count(tokens[2]) != 1) {
            inp.out_error = inp.error(line_index, fmt::format("@cs '{}' not found for @program '{}.", tokens[2], tokens[1]));
            return false;
        }
    }
    return true;
}

static bool validate_options_tag(const std::vector<std::string>& tokens, const Snippet& cur_snippet, int line_index, Input& inp) {
    if (tokens.size() < 2) {
        inp.out_error = inp.error(line_index, fmt::format("{} must have at least 1 arg ('fixup_clipspace', 'flip_vert_y')", tokens[0]));
        return false;
    }
    if (cur_snippet.type != Snippet::VS) {
        inp.out_error = inp.error(line_index, fmt::format("{} must be inside a @vs block", tokens[0]));
        return false;
    }
    for (int i = 1; i < (int)tokens.size(); i++) {
        if (Option::from_string(tokens[i]) == Option::INVALID) {
            inp.out_error = inp.error(line_index, fmt::format("unknown option '{}' (must be 'fixup_clipspace', 'flip_vert_y')", tokens[i]));
            return false;
        }
    }
    return true;
}

static bool validate_image_sample_type_tag(const std::vector<std::string>& tokens, int line_index, Input& inp) {
    if (tokens.size() != 3) {
        inp.out_error = inp.error(line_index, fmt::format("@image_sample_type must have 2 args (@image_sample_type [texture] {})", ImageSampleType::valid_image_sample_types_as_str()));
        return false;
    }
    if (nullptr != inp.find_image_sample_type_tag(tokens[1])) {
        inp.out_error = inp.error(line_index, "duplicate @image_sample_type (texture name must be unique)");
        return false;
    }
    if (!ImageSampleType::is_valid_str(tokens[2])) {
        inp.out_error = inp.error(line_index, fmt::format("second arg of @image_sample_type tag must be one of {}", ImageSampleType::valid_image_sample_types_as_str()));
        return false;
    }
    return true;
}

static bool validate_sampler_type_tag(const std::vector<std::string>& tokens, int line_index, Input& inp) {
    if (tokens.size() != 3) {
        inp.out_error = inp.error(line_index, fmt::format("@sampler_type must have 2 args (@sampler_type [sampler] {})", SamplerType::valid_sampler_types_as_str()));
        return false;
    }
    if (nullptr != inp.find_sampler_type_tag(tokens[1])) {
        inp.out_error = inp.error(line_index, "duplicate @sampler_type (sampler name must be unique)");
        return false;
    }
    if (!SamplerType::is_valid_str(tokens[2])) {
        inp.out_error = inp.error(line_index, fmt::format("second arg of @sampler_type tag must be one of {}", SamplerType::valid_sampler_types_as_str()));
        return false;
    }
    return true;
}

/* This parses the split input line array for custom tags (@vs, @fs, @block,
    @end and @program), and fills the respective members. If a parsing error
    happens, the inp.error object is setup accordingly.
*/
static bool parse(Input& inp) {
    bool in_snippet = false;
    bool add_line = false;
    Snippet cur_snippet;
    std::vector<std::string> tokens;
    int line_index = 0;
    for (const Line& line_info : inp.lines) {
        const std::string& line = line_info.line;
        add_line = in_snippet;
        pystring::split(line, tokens);
        if (tokens.size() > 0) {
            if (tokens[0] == module_tag) {
                if (!validate_module_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                inp.module = tokens[1];
            } else if (tokens[0] == ctype_tag) {
                if (!validate_ctype_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                if (inp.ctype_map.count(tokens[1]) > 0) {
                    inp.out_error = inp.error(line_index, fmt::format("type '{}' already defined!", tokens[1]));
                    return false;
                }
                inp.ctype_map[tokens[1]] = tokens[2];
            } else if (tokens[0] == header_tag) {
                if (!validate_header_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                std::vector<std::string> skip_first_token = tokens;
                skip_first_token.erase(skip_first_token.begin());
                inp.headers.push_back(pystring::join(" ", skip_first_token));
            } else if (tokens[0] == glsl_options_tag) {
                if (!validate_options_tag(tokens, cur_snippet, line_index, inp)) {
                    return false;
                }
                for (int i = 1; i < (int)tokens.size(); i++) {
                    uint32_t option_bit = Option::from_string(tokens[i]);
                    cur_snippet.options[Slang::GLSL410] |= option_bit;
                    cur_snippet.options[Slang::GLSL430] |= option_bit;
                    cur_snippet.options[Slang::GLSL300ES] |= option_bit;
                    cur_snippet.options[Slang::GLSL310ES] |= option_bit;
                }
                add_line = false;
            } else if (tokens[0] == hlsl_options_tag) {
                if (!validate_options_tag(tokens, cur_snippet, line_index, inp)) {
                    return false;
                }
                for (int i = 1; i < (int)tokens.size(); i++) {
                    uint32_t option_bit = Option::from_string(tokens[i]);
                    cur_snippet.options[Slang::HLSL4] |= option_bit;
                    cur_snippet.options[Slang::HLSL5] |= option_bit;
                }
                add_line = false;
            } else if (tokens[0] == msl_options_tag) {
                if (!validate_options_tag(tokens, cur_snippet, line_index, inp)) {
                    return false;
                }
                for (int i = 1; i < (int)tokens.size(); i++) {
                    uint32_t option_bit = Option::from_string(tokens[i]);
                    cur_snippet.options[Slang::METAL_MACOS] |= option_bit;
                    cur_snippet.options[Slang::METAL_IOS] |= option_bit;
                    cur_snippet.options[Slang::METAL_SIM] |= option_bit;
                }
                add_line = false;
            } else if (tokens[0] == block_tag) {
                if (!validate_block_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                cur_snippet = Snippet(Snippet::BLOCK, tokens[1]);
                add_line = false;
                in_snippet = true;
            } else if (tokens[0] == vs_tag) {
                if (!validate_vs_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                cur_snippet = Snippet(Snippet::VS, tokens[1]);
                add_line = false;
                in_snippet = true;
            } else if (tokens[0] == fs_tag) {
                if (!validate_fs_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                cur_snippet = Snippet(Snippet::FS, tokens[1]);
                add_line = false;
                in_snippet = true;
            } else if (tokens[0] == cs_tag) {
                if (!validate_cs_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                cur_snippet = Snippet(Snippet::CS, tokens[1]);
                add_line = false;
                in_snippet = true;
            } else if (tokens[0] == inclblock_tag) {
                if (!validate_inclblock_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                const Snippet& src_snippet = inp.snippets[inp.snippet_map[tokens[1]]];
                for (int line_index : src_snippet.lines) {
                    cur_snippet.lines.push_back(line_index);
                }
                add_line = false;
            } else if (tokens[0] == end_tag) {
                if (!validate_end_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                cur_snippet.index = (int)inp.snippets.size();
                inp.snippet_map[cur_snippet.name] = cur_snippet.index;
                switch (cur_snippet.type) {
                    case Snippet::BLOCK:
                        inp.block_map[cur_snippet.name] = cur_snippet.index;
                        break;
                    case Snippet::VS:
                        inp.vs_map[cur_snippet.name] = cur_snippet.index;
                        break;
                    case Snippet::FS:
                        inp.fs_map[cur_snippet.name] = cur_snippet.index;
                        break;
                    case Snippet::CS:
                        inp.cs_map[cur_snippet.name] = cur_snippet.index;
                        break;
                    default: break;
                }
                inp.snippets.push_back(std::move(cur_snippet));
                cur_snippet = Snippet();
                add_line = false;
                in_snippet = false;
            } else if (tokens[0] == prog_tag) {
                if (!validate_program_tag(tokens, in_snippet, line_index, inp)) {
                    return false;
                }
                if (tokens.size() == 4) {
                    inp.programs[tokens[1]] = Program::from_vs_fs(tokens[1], tokens[2], tokens[3], line_index);
                } else {
                    inp.programs[tokens[1]] = Program::from_cs(tokens[1], tokens[2], line_index);
                }
                add_line = false;
            } else if (tokens[0] == image_sample_type_tag) {
                if (!validate_image_sample_type_tag(tokens, line_index, inp)) {
                    return false;
                }
                inp.image_sample_type_tags[tokens[1]] = ImageSampleTypeTag(tokens[1], ImageSampleType::from_str(tokens[2]), line_index);
                add_line = false;
            } else if (tokens[0] == sampler_type_tag) {
                if (!validate_sampler_type_tag(tokens, line_index, inp)) {
                    return false;
                }
                inp.sampler_type_tags[tokens[1]] = SamplerTypeTag(tokens[1], SamplerType::from_str(tokens[2]), line_index);
                add_line = false;
            } else if (tokens[0][0] == '@') {
                inp.out_error = inp.error(line_index, fmt::format("unknown meta tag: {}", tokens[0]));
                return false;
            }
        }
        if (add_line) {
            cur_snippet.lines.push_back(line_index);
        }
        line_index++;
    }
    if (in_snippet) {
        inp.out_error = inp.error(line_index - 1, "final @end missing.");
        return false;
    }
    return true;
}

static bool validate_include_tag(const std::vector<std::string>& tokens, int line_nr, const std::string& path, Input& inp) {
    if (tokens.size() != 2) {
        inp.out_error = ErrMsg::error(path, line_nr, "@include tag must have exactly one arg (@include filename).");
        return false;
    }
    return true;
}

static bool load_and_preprocess(const std::string& path, const std::vector<std::string>& include_dirs,
                                Input& inp, int parent_line_index) {
    std::string path_used = path;
    std::string str = load_file_into_str(path_used);
    if (str.empty()) {
        // check include directories
        for (const std::string& include_dir : include_dirs) {
            path_used = pystring::os::path::join(include_dir, path);
            str = load_file_into_str(path_used);
            if (!str.empty()) {
                break;
            }
        }
        // failure?
        if (str.empty()) {
            if (inp.base_path == path) {
                inp.out_error = ErrMsg::error(path, 0, fmt::format("Failed to open input file '{}'", path));
            } else {
                inp.out_error = ErrMsg::error(inp.filenames.back(), parent_line_index, fmt::format("Failed to open @include file '{}'", path));
            }
            return false;
        }
    }
    // check for include cycles
    for (const std::string& filename : inp.filenames) {
        if (filename == path_used) {
            inp.out_error = ErrMsg::error(inp.filenames.back(), parent_line_index, fmt::format("Detected @include file cycle: '{}'", path_used));
            return false;
        }
    }
    // add to filenames
    int filename_index = (int)inp.filenames.size();
    inp.filenames.push_back(path_used);

    // remove comments before splitting into lines
    if (!remove_comments(str)) {
        inp.out_error = ErrMsg::error(path_used, 0, fmt::format("(FIXME) Error during removing comments in '{}'", path_used));
    }

    // split source file into lines
    int line_index = 0;
    std::vector<std::string> lines;
    pystring::splitlines(str, lines);

    // preprocess
    std::vector<std::string> tokens;
    for (std::string& line : lines) {
        // look for @include tags
        pystring::split(line, tokens);
        if (tokens.size() > 0) {
            if (!normalize_pragma_sokol(tokens, line, line_index, inp)) {
                return false;
            }
            if (tokens[0] == include_tag) {
                if (!validate_include_tag(tokens, line_index, path_used, inp)) {
                    return false;
                }
                // insert included file
                const std::string& include_filename = tokens[1];
                if (!load_and_preprocess(include_filename, include_dirs, inp, line_index)) {
                    return false;
                }
            } else {
                // otherwise process file as normal
                inp.lines.push_back({line, filename_index, line_index});
            }
        } else {
            // this is an empty line, but add it anyway so the error line
            // indices are always correct
            inp.lines.push_back({ line, filename_index, line_index});
        }
        line_index++;
    }

    return true;
}

/* load file and parse into an Input object,
   check valid and error fields in returned object
*/
Input Input::load_and_parse(const std::string& path, const std::string& module_override) {
    std::string dir;
    std::string filename;
    pystring::os::path::split(dir, filename, path);
    std::vector<std::string> include_dirs = { dir };

    Input inp;
    inp.base_path = path;
    if (load_and_preprocess(path, include_dirs, inp, 0)) {
        parse(inp);
    }
    if (!module_override.empty()) {
        inp.module = module_override;
    }
    return inp;
}

ErrMsg Input::error(int line_index, const std::string& msg) const {
    if (line_index < (int)lines.size()) {
        const Line& line = lines[line_index];
        return ErrMsg::error(filenames[line.filename], line.index, msg);
    } else {
        return ErrMsg::error(base_path, 0, msg);
    }
};

ErrMsg Input::warning(int line_index, const std::string& msg) const {
    if (line_index < (int)lines.size()) {
        const Line& line = lines[line_index];
        return ErrMsg::warning(filenames[line.filename], line.index, msg);
    } else {
        return ErrMsg::warning(base_path, 0, msg);
    }
};

/* print a debug-dump of content to stderr */
void Input::dump_debug(ErrMsg::Format err_fmt) const {
    fmt::print(stderr, "Input:\n");
    if (out_error.valid()) {
        fmt::print(stderr, "  error: {}\n", out_error.as_string(err_fmt));
    } else {
        fmt::print(stderr, "  error: not set\n");
    }
    fmt::print(stderr, "  base_path: {}\n", base_path);
    fmt::print(stderr, "  module: {}\n", module);
    {
        fmt::print(stderr, "  lines:\n");
        int filename = -1;
        for (const Line& line : lines) {
            if (filename != line.filename) {
                fmt::print(stderr, "    {}:\n", filenames[line.filename]);
                filename = line.filename;
            }
            fmt::print(stderr, "    {}: {}\n", line.index + 1, line.line);
        }
    }
    fmt::print(stderr, "  types:\n");
    for (const auto& item: ctype_map) {
        fmt::print(stderr, "    {}: {}\n", item.first, item.second);
    }
    {
        int snippet_nr = 0;
        fmt::print(stderr, "  snippets:\n");
        for (const Snippet& snippet : snippets) {
            fmt::print(stderr, "    snippet {}:\n", snippet_nr++);
            fmt::print(stderr, "      name: {}\n", snippet.name);
            fmt::print(stderr, "      type: {}\n", Snippet::type_to_str(snippet.type));
            fmt::print(stderr, "      lines:\n");
            int line_nr = 1;
            for (int line_index : snippet.lines) {
                fmt::print(stderr, "        {:3}({:3}): {}\n", line_nr++, line_index+1, lines[line_index].line);
            }
        }
    }
    fmt::print(stderr, "  snippet_map:\n");
    for (const auto& item : snippet_map) {
        fmt::print(stderr, "    {} => snippet {}\n", item.first, item.second);
    }
    fmt::print(stderr, "  block_map:\n");
    for (const auto& item : block_map) {
        fmt::print(stderr, "    {} => snippet {}\n", item.first, item.second);
    }
    fmt::print(stderr, "  vs_map:\n");
    for (const auto& item : vs_map) {
        fmt::print(stderr, "    {} => snippet {}\n", item.first, item.second);
    }
    fmt::print(stderr, "  fs_map:\n");
    for (const auto& item : fs_map) {
        fmt::print(stderr, "    {} => snippet {}\n", item.first, item.second);
    }
    fmt::print(stderr, "  cs_map:\n");
    for (const auto& item : cs_map) {
        fmt::print(stderr, "    {} => snippet {}\n", item.first, item.second);
    }
    fmt::print(stderr, "  programs:\n");
    for (const auto& item : programs) {
        const std::string& key = item.first;
        const Program& prog = item.second;
        fmt::print(stderr, "    program {}:\n", key);
        fmt::print(stderr, "      name: {}\n", prog.name);
        fmt::print(stderr, "      vs: {}\n", prog.vs_name);
        fmt::print(stderr, "      fs: {}\n", prog.fs_name);
        fmt::print(stderr, "      cs: {}\n", prog.cs_name);
        fmt::print(stderr, "      line_index: {}\n", prog.line_index);
    }
    fmt::print(stderr, "    image sample type tags:\n");
    for (const auto& [key, val]: image_sample_type_tags) {
        fmt::print(stderr, "      {}: {} (line: {})\n", key, ImageSampleType::to_str(val.type), val.line_index);
    }
    fmt::print(stderr, "    sampler type tags:\n");
    for (const auto& [key, val]: sampler_type_tags) {
        fmt::print(stderr, "      {}: {} (line: {})\n", key, SamplerType::to_str(val.type), val.line_index);
    }
    fmt::print("\n");
}

} // namespace shdc
