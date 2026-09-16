// HDS Pi/touchscreen client -- an ImGui + GLFW touch UI for a 720x720px 4" panel, talking to the
// REAL HDS backend (src/backend.c) over OSC via liblo. This started as an exploratory Gemini
// draft for a Raspberry Pi embedded deployment; that draft proved out the UI *shape* (a six-page
// tab strip with a full-page modal patch browser, large touch targets) but invented its own
// fictional protocol (a hand-rolled OSC packer, port 7777, addresses like "/synth/adsr/attack")
// that never actually talked to HDS. This file keeps the UI shape and fixes the transport: real
// liblo (matching backend.c's own OSC library and avoiding its hand-rolled packer's un-bounds-
// checked fixed buffer), the backend's actual default ports (9000 send / 9001 receive -- see
// README.md's "Control protocol: OSC" section and backend.c's --osc-listen-port/--osc-reply-port
// flags), and the real /hds/... address space from backend.c's own OSC_PARAMS[] table.
//
// Scope, stated explicitly (a deliberate boundary, not an oversight): the ADSR page (Osc 1's Amp
// EG, Master Volume, Oscillator waveform) is wired to real backend fields, plus a live status
// readback (Peak level, live Tempo) via /hds/status/*. The Program Library page now reads and
// writes HDS's REAL saved Programs (~/HDS_Programs/<name>/<name>.hdsprog -- see frontend.py's own
// _pick_and_save_program()/_load_program_from_path() for the canonical implementation this
// mirrors): Load sends every field a loaded file has straight to the backend (a real, whole-patch
// load, not just the handful of fields this page's own sliders show), and Save captures this
// client's own observed backend state (see g_full_state below) through the same Program-scope
// allowlist frontend.py's _is_program_scope_address() uses. A loaded file's colocated .sfz (Sample
// Bank content) is NOT applied -- this client has no Mapping-tab equivalent -- and is flagged, not
// silently dropped. Comb/Seq/Rec/Glob pages remain placeholder text, unchanged from the original
// exploratory draft.
//
// Build: see the Makefile's `pi-touch` target. Needs GLFW3 + OpenGL + liblo development packages
// (apt: libglfw3-dev liblo-dev) -- Dear ImGui (pinned v1.91.5) and nlohmann/json (pinned v3.11.3,
// single-header, MIT licensed, github.com/nlohmann/json) are vendored into third_party/, rather
// than assumed to be system packages, mirroring this project's existing CLAP-SDK/miniaudio.h
// vendoring precedent.
//
// A Raspberry Pi 5 (or any other aarch64 Linux machine) needs no special build target at all --
// it's just another native Linux host for this exact Makefile: install the same handful of dev
// packages via apt and run `make`/`make pi-touch` directly on the Pi. No cross-compilation.

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <lo/lo.h>
#include "../third_party/json/json.hpp"
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/backends/imgui_impl_glfw.h"
#include "../third_party/imgui/backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

namespace fs = std::filesystem;

// --------------------------------------------------------------------------------------------
// OSC transport: liblo, matching backend.c's own transport exactly -- one lo_address to SEND
// knob writes to the backend's listen port, one bound lo_server to RECEIVE /hds/status/*
// (and, once at startup or on manual Refresh, a /hds/request_state reply) on the backend's reply
// port. Non-blocking, polled once per frame from the main loop (poll_osc(), called alongside
// glfwPollEvents()) -- the same single-threaded, no-locking shape backend.c's own main() loop
// uses for its OSC server, and frontend.py's own vu_timer-driven listen_for_status_frames() uses
// on the GUI side. Defaults match backend.c's own --osc-listen-port/--osc-reply-port defaults
// (9000/9001), so this client talks to a stock-launched backend with no extra flags on either side.
// --------------------------------------------------------------------------------------------
static lo_address g_hds_addr = nullptr;      // where we SEND knob writes
static lo_server g_status_server = nullptr;  // where we RECEIVE status + request_state replies

// Every real backend OSC address this client has ever observed a value for -- populated by our
// own sends (see send_osc_float below) and by generic_state_handler's catch-all reception of
// /hds/request_state's reply dump. This is BOTH "the last state we know the backend to be in" AND
// "what Save Patch writes out" -- the same unification frontend.py's own _last_wire_state serves,
// just built from a live OSC round-trip instead of a locally-computed widget snapshot, since this
// client's own UI only edits a handful of the ~1100 real Program-scope addresses and has no local
// widget state for the rest (filters, LFOs, FX, etc.) -- those only ever get INTO this map via an
// actual /hds/request_state pull, never invented locally.
static std::map<std::string, float> g_full_state;

static void send_osc_float(const char* path, float value) {
    g_full_state[path] = value;
    if (!g_hds_addr) return;
    lo_send(g_hds_addr, path, "f", value);
}

// Reads g_full_state with a fallback for an address never yet observed (before the first
// request_state reply lands, or for a field this backend build doesn't have).
static float get_state(const char* path, float default_val) {
    auto it = g_full_state.find(path);
    return it != g_full_state.end() ? it->second : default_val;
}

static void osc_error_handler(int num, const char* msg, const char* path) {
    fprintf(stderr, "[osc] liblo error %d (%s): %s\n", num, path ? path : "?", msg);
}

// --- Live status readback -----------------------------------------------------------------
// Updated from backend.c's own periodic /hds/status/* bundle (send_osc_status(), sent on the
// same cadence the VU meter/tempo readout in frontend.py already consumes). last_status_time
// is this client's own liveness heuristic, mirroring frontend.py's identical "no UDP connect-
// time signal, so infer liveness from whether status has arrived recently" design. Deliberately
// NOT folded into g_full_state -- these are one-way backend-to-controller telemetry, not
// Program-scope knob state, and Save Patch must never see them.
static float g_live_peak = 0.0f;
static float g_live_tempo_bpm = 0.0f;
static double g_last_status_time = 0.0;
static const double OSC_STATUS_TIMEOUT_S = 1.5;

static int status_float_handler(const char*, const char*, lo_arg** argv, int argc, lo_message, void* user_data) {
    if (argc != 1) return 0;
    *(float*)user_data = argv[0]->f;
    g_last_status_time = glfwGetTime();
    return 0;
}

// Generic catch-all for everything else liblo delivers to this server -- specifically, every
// real knob address in a /hds/request_state reply dump. Registered with NULL path/types (liblo's
// documented catch-all form), so it only ever sees messages the two specific handlers above
// didn't already claim (each of those returns 0, meaning "handled, stop dispatching further" --
// see liblo's own callback-return convention) -- /hds/status/peak and .../tempo_bpm_live never
// reach here. Silently ignores non-float messages (string-valued addresses like /hds/device/
// output, and the zero-arg /hds/status/state_dump_complete sentinel) -- this client has no use
// for either.
static int generic_state_handler(const char* path, const char* types, lo_arg** argv, int argc, lo_message, void*) {
    if (argc == 1 && types[0] == 'f') g_full_state[path] = argv[0]->f;
    return 0;
}

// WaveType's real enum order from backend.c (WAVE_SAW=0, WAVE_SQUARE=1, WAVE_TRIANGLE=2,
// WAVE_SINE=3, WAVE_UNISON_SAW=4, WAVE_NOISE=5, WAVE_PULSE=6, WAVE_SAMPLE=7, WAVE_OFF=8). This
// page only offers the six shapes a performer would sensibly toggle live from a combo box --
// Unison Saw (needs detune/width knobs), Sample (needs a loaded zone), and Off (silences the
// oscillator outright) all need more context than a single dropdown gives them, so they're left
// out of this quick-edit page rather than half-wired.
static const char* g_wave_names[] = { "Sawtooth", "Square", "Triangle", "Sine", "Noise", "Pulse" };
static const int g_wave_values[] = { 0, 1, 2, 3, 5, 6 };
static const int g_wave_count = 6;

static void poll_osc() {
    if (g_status_server) {
        while (lo_server_recv_noblock(g_status_server, 0) > 0) {}
    }
}

// --------------------------------------------------------------------------------------------
// Program-scope address filtering -- a direct port of frontend.py's own _is_program_scope_
// address() (see src/frontend.py), which is the live, already-battle-tested definition of "what
// belongs in a saved .hdsprog" used by Program Save/Load there. Ported rather than re-derived so
// this client's Save produces files with exactly the same field boundary frontend.py's own Save
// does -- a Program stays a self-contained patch description, with no device/routing/session
// state leaking in.
// --------------------------------------------------------------------------------------------
static bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
static bool ends_with(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static bool is_program_scope_address(const std::string& addr) {
    if (addr == "/hds/performance_mode") return false;
    if (starts_with(addr, "/hds/seq/")) return false;
    if (addr == "/hds/master_volume") return false;
    if (addr == "/hds/transpose_semitones") return false;
    static const char* excluded_prefixes[] = { "/hds/device/", "/hds/record/", "/hds/routing/", "/hds/ext_in/", "/hds/audio/" };
    for (const char* p : excluded_prefixes) if (starts_with(addr, p)) return false;
    if (starts_with(addr, "/hds/fx/") && ends_with(addr, "/source")) return false;
    if (starts_with(addr, "/hds/fx_routing/")) return addr == "/hds/fx_routing/0/mode";
    // Combi-lane mirrors (/hds/combi/N/...) -- excluded here even though frontend.py's own
    // _is_program_scope_address doesn't need to mention them: THAT function only ever filters
    // frontend.py's _last_wire_state, which structurally never contains a /hds/combi/... address
    // in the first place (no widget layer sends them -- see check_osc_address_parity.py's own
    // comment on this). This client's g_full_state, by contrast, is populated via a full
    // /hds/request_state dump, which DOES include every Combi lane's own mirrored copy of every
    // Program-scoped field -- without this exclusion, Save would wrongly bundle all 8 lanes'
    // fields into what's supposed to describe a single Program.
    if (starts_with(addr, "/hds/combi/")) return false;
    return true;
}

// --------------------------------------------------------------------------------------------
// Program Library: real ~/HDS_Programs/<name>/<name>.hdsprog files (see frontend.py's own
// programs_folder/_scan_programs_folder() -- same convention, same directory). Category is a
// plain metadata field on the file (never applied to any OSC address, see load_program below) --
// an open string, not a fixed enum, so the category tabs below are built from whatever's actually
// in use across saved files rather than a hardcoded list.
// --------------------------------------------------------------------------------------------
struct LoadedProgram {
    std::string name;      // JSON "name", falls back to the folder name if absent
    std::string category;  // JSON "category", "" if the file predates this field or left it blank
    std::string path;      // full path to the .hdsprog file
    bool has_sfz = false;  // JSON "sfz_file" present -- flagged in the UI, never applied (see below)
};

static std::vector<LoadedProgram> g_programs;
static std::vector<std::string> g_categories; // "All" first, then whatever's actually in use, sorted

static std::string home_programs_dir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/HDS_Programs";
}

static void rescan_programs() {
    g_programs.clear();
    std::set<std::string> cat_set;
    std::error_code ec;
    std::string base = home_programs_dir();
    if (fs::is_directory(base, ec)) {
        for (auto& entry : fs::directory_iterator(base, ec)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            fs::path hdsprog_path = entry.path() / (name + ".hdsprog");
            if (!fs::is_regular_file(hdsprog_path)) continue;
            std::ifstream f(hdsprog_path);
            if (!f) continue;
            nlohmann::json j;
            try { f >> j; } catch (...) { continue; } // skip a corrupt/partial file, don't crash the browser
            LoadedProgram lp;
            lp.name = j.value("name", name);
            lp.category = j.value("category", "");
            lp.path = hdsprog_path.string();
            lp.has_sfz = j.contains("sfz_file") && j["sfz_file"].is_string() && !j["sfz_file"].get<std::string>().empty();
            // Read the category BEFORE moving lp into the vector -- std::move(lp) below leaves
            // lp's own members in an unspecified (in practice, empty-string) moved-from state,
            // so inserting from lp.category AFTER that line would silently insert nothing.
            if (!lp.category.empty()) cat_set.insert(lp.category);
            g_programs.push_back(std::move(lp));
        }
    }
    std::sort(g_programs.begin(), g_programs.end(), [](const LoadedProgram& a, const LoadedProgram& b) { return a.name < b.name; });
    g_categories = { "All" };
    for (const auto& c : cat_set) g_categories.push_back(c);
}

static std::string g_loaded_program_name = "(none loaded)";
static std::string g_load_status; // last Load's outcome, shown in the browser -- error or a colocated-.sfz notice

// Loads a real saved Program: sends EVERY address:value pair in the file's own "fields" map
// straight to the backend, exactly matching frontend.py's own _apply_program_fields_to_combi_lane
// in spirit -- a real whole-patch load (filters, LFOs, FX, everything the file actually has), not
// just the handful of fields this page's own sliders show. Those sliders read live from
// g_full_state (see get_state above), which send_osc_float already keeps in sync as a side
// effect, so no separate "now update the widgets" step is needed here.
static void load_program(const LoadedProgram& lp) {
    std::ifstream f(lp.path);
    if (!f) { g_load_status = "Could not open " + lp.path; return; }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        g_load_status = std::string("Could not parse ") + lp.path + ": " + e.what();
        return;
    }
    if (j.contains("fields") && j["fields"].is_object()) {
        for (auto& [addr, val] : j["fields"].items()) {
            if (val.is_number()) send_osc_float(addr.c_str(), val.get<float>());
        }
    }
    g_loaded_program_name = lp.name;
    g_load_status = lp.has_sfz
        ? "Loaded '" + lp.name + "' (its colocated .sfz Sample Bank was NOT applied -- this client has no Mapping-tab equivalent)"
        : "Loaded '" + lp.name + "'";
}

// Saves this client's own observed backend state (g_full_state, Program-scope-filtered) as a new
// .hdsprog -- the real file format frontend.py's own Save writes, so either app can load what the
// other saved. No colocated .sfz is ever written (this client tracks no Sample Bank state at all).
static std::string g_save_status;

static void save_program(const std::string& name, const std::string& category) {
    if (name.empty()) { g_save_status = "Name cannot be empty"; return; }
    nlohmann::json fields = nlohmann::json::object();
    for (const auto& [addr, val] : g_full_state) {
        if (is_program_scope_address(addr)) fields[addr] = val;
    }
    nlohmann::json j;
    j["format_version"] = 1;
    j["name"] = name;
    j["category"] = category;
    j["fields"] = fields;

    std::string dir = home_programs_dir() + "/" + name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { g_save_status = "Could not create " + dir + ": " + ec.message(); return; }
    std::string path = dir + "/" + name + ".hdsprog";
    std::ofstream out(path);
    if (!out) { g_save_status = "Could not write " + path; return; }
    out << j.dump(2);
    g_save_status = fields.empty()
        ? "Saved '" + name + "' -- but g_full_state was empty (no /hds/request_state reply received "
          "yet?). Try Refresh, then Save again."
        : "Saved '" + name + "' (" + std::to_string(fields.size()) + " fields)";
    g_loaded_program_name = name;
    rescan_programs();
}

enum SynthPage { PAGE_PROG, PAGE_COMB, PAGE_SEQ, PAGE_REC, PAGE_ADSR, PAGE_GLOB };

const char* custom_strcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return nullptr;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++;
                n++;
            }
            if (!*n) return haystack;
        }
    }
    return nullptr;
}

// Small rotary-knob widget -- Dear ImGui core has no built-in rotary control (only sliders), so
// this draws one directly via ImDrawList (a filled circle + a line indicator at the current
// angle) and reads input through a plain InvisibleButton, matching this file's own established
// habit of building the UI primitives it needs directly rather than pulling in a separate
// vendored knob library. Touch/drag convention: dragging vertically changes the value (the same
// gesture every touch-synth knob already uses), which works identically for mouse and touch since
// GLFW/ImGui report a touch drag as ordinary mouse-delta input.
static bool draw_pan_knob(const char* str_id, float* value, float min_val, float max_val, float diameter) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float radius = diameter * 0.5f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 center = ImVec2(pos.x + radius, pos.y + radius);

    ImGui::InvisibleButton(str_id, ImVec2(diameter, diameter));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (active && io.MouseDelta.y != 0.0f) {
        float range = max_val - min_val;
        *value -= io.MouseDelta.y * (range / 200.0f);
        if (*value < min_val) *value = min_val;
        if (*value > max_val) *value = max_val;
        changed = true;
    }

    float t = (*value - min_val) / (max_val - min_val);
    // 270-degree sweep, starting 135 degrees clockwise from straight up (a standard knob arc).
    float angle = 0.75f * (float)M_PI + t * 1.5f * (float)M_PI;
    ImU32 col_bg = ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
    draw_list->AddCircleFilled(center, radius, col_bg, 32);
    draw_list->AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), 32, 2.0f);
    ImVec2 tip = ImVec2(center.x + cosf(angle) * radius * 0.75f, center.y + sinf(angle) * radius * 0.75f);
    draw_list->AddLine(center, tip, ImGui::GetColorU32(ImGuiCol_Text), 3.0f);

    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", *value);
    ImVec2 tsize = ImGui::CalcTextSize(buf);
    draw_list->AddText(ImVec2(center.x - tsize.x * 0.5f, pos.y + diameter + 2.0f), ImGui::GetColorU32(ImGuiCol_Text), buf);
    ImGui::Dummy(ImVec2(diameter, tsize.y + 4.0f));  // reserve the value-text row below the circle
    return changed;
}

// --------------------------------------------------------------------------------------------
// Program page: quick-edit Oscillator controls (Shape/Volume/Pan/Mute for all 4 oscillators) --
// the same real backend fields frontend.py's own Oscillators tab edits, not just the single Osc
// 1 waveform combo the ADSR page already had. Defaults match backend.c's init_default_controls()
// exactly per oscillator (Osc1: Saw/80%, Osc2: Sine/50%, Osc3: Noise/0%, Osc4: Off/0%) -- Osc4's
// real default shape (WAVE_OFF=8) isn't one of the six shapes g_wave_values offers (see its own
// comment), so it shows as "Sawtooth" until a real /hds/vco4/shape value arrives, the same
// documented limitation the ADSR page's own waveform combo already has for Unison Saw/Sample/Off.
//
// Split into two panels: draw_osc_type_row (its own bordered panel -- "Oscillator Type" -- an
// Osc-N label with a small Mute toggle directly below it on the far left, then the Shape combo,
// then the Pan knob) and draw_oscillator_row (its own bordered panel -- "Oscillator Mix" -- just
// the Osc-N label and a vertical Vol fader, sized to fill essentially the whole panel height).
// Keeping Mute/Pan out of the Mix panel is what lets its fader actually use the available height
// instead of splitting it three ways with a checkbox and a knob.
// --------------------------------------------------------------------------------------------
static void draw_osc_type_row(int osc_num, int default_shape_value) {
    char addr[64];
    ImGui::PushID(osc_num);

    ImGui::BeginGroup();  // Osc N label + Mute, stacked, so SameLine after it aligns to the top
    char label[16];
    snprintf(label, sizeof(label), "Osc %d", osc_num);
    ImGui::TextUnformatted(label);

    snprintf(addr, sizeof(addr), "/hds/vco%d/mute", osc_num);
    bool muted = get_state(addr, 0.0f) != 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 6.0f));  // shrink from the global touch size
    if (ImGui::Checkbox("M", &muted)) {
        send_osc_float(addr, muted ? 1.0f : 0.0f);
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::SameLine();

    snprintf(addr, sizeof(addr), "/hds/vco%d/shape", osc_num);
    float shape_raw = get_state(addr, (float)default_shape_value);
    int wave_idx = 0;
    for (int i = 0; i < g_wave_count; i++) if (g_wave_values[i] == (int)(shape_raw + 0.5f)) { wave_idx = i; break; }
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##Shape", &wave_idx, g_wave_names, g_wave_count)) {
        send_osc_float(addr, (float)g_wave_values[wave_idx]);
    }
    ImGui::SameLine();

    snprintf(addr, sizeof(addr), "/hds/vco%d/pan", osc_num);
    float pan_pct = get_state(addr, 0.0f) * 100.0f;
    float pan_before = pan_pct;
    draw_pan_knob("##Pan", &pan_pct, -100.0f, 100.0f, 40.0f);
    if (pan_pct != pan_before) {
        send_osc_float(addr, pan_pct / 100.0f);
    }

    ImGui::PopID();
}

// One column per oscillator (Osc N label + a vertical Vol fader) -- the fader's height is passed
// in from the call site as essentially the whole available panel height, since Mute/Pan no longer
// compete with it for vertical space.
static void draw_oscillator_row(int osc_num, float default_vol_pct, float fader_height) {
    char addr[64];
    ImGui::PushID(osc_num);
    ImGui::BeginGroup();

    char label[16];
    snprintf(label, sizeof(label), "Osc %d", osc_num);
    ImGui::TextUnformatted(label);

    // Drag/display in real 0-100 UI units (matching the Pan knob's own convention), converting to
    // the 0.0-1.0 wire scale only at read/send time -- VSliderFloat's format string renders `*v`
    // directly, so driving it with the raw 0.0-1.0 wire fraction (the previous version of this
    // code) meant "%.0f%%" could only ever print "0%" or "1%", never a real percentage.
    snprintf(addr, sizeof(addr), "/hds/vco%d/vol", osc_num);
    float vol_pct = get_state(addr, default_vol_pct / 100.0f) * 100.0f;
    if (ImGui::VSliderFloat("##Vol", ImVec2(56, fader_height), &vol_pct, 0.0f, 100.0f, "%.0f%%")) {
        send_osc_float(addr, vol_pct / 100.0f);
    }

    ImGui::EndGroup();
    ImGui::PopID();
}

int main() {
    g_hds_addr = lo_address_new("127.0.0.1", "9000");
    g_status_server = lo_server_new_with_proto("9001", LO_UDP, osc_error_handler);
    if (!g_status_server) {
        fprintf(stderr, "[osc] failed to bind reply port 9001 -- status readback and startup "
                         "state pull will not work, but knob writes to the backend still will.\n");
    } else {
        lo_server_add_method(g_status_server, "/hds/status/peak", "f", status_float_handler, &g_live_peak);
        lo_server_add_method(g_status_server, "/hds/status/tempo_bpm_live", "f", status_float_handler, &g_live_tempo_bpm);
        lo_server_add_method(g_status_server, NULL, NULL, generic_state_handler, nullptr);
        lo_send(g_hds_addr, "/hds/request_state", "");
    }
    rescan_programs();

    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(720, 720, "HDS Touch Console", NULL, NULL);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.FramePadding = ImVec2(20.0f, 20.0f);
    style.GrabMinSize = 44.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    SynthPage currentPage = PAGE_ADSR;

    bool show_program_selector = false;
    bool show_save_popup = false;
    int selected_program_idx = -1;
    int current_category_tab = 0;
    char search_query[64] = "";
    char save_name_buf[128] = "";
    char save_category_buf[64] = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        poll_osc();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(720, 540));
        // NoScrollbar/NoScrollWithMouse: this is a locked kiosk touchscreen with no scroll gesture
        // available on the physical device, so no page may ever rely on scrolling to reach a control.
        ImGui::Begin("ActiveViewPanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        switch (currentPage) {
            case PAGE_PROG:
            {
                ImGui::Text("PROGRAM LIBRARY [PROG]");
                ImGui::Separator(); ImGui::Spacing();

                ImGui::Text("Loaded Program:");
                ImGui::PushItemWidth(680);
                char strip_label[160];
                snprintf(strip_label, sizeof(strip_label), "%s  [Tap to Browse...]", g_loaded_program_name.c_str());
                if (ImGui::Button(strip_label, ImVec2(680, 0))) {
                    show_program_selector = true;
                }
                ImGui::PopItemWidth();
                ImGui::Spacing();

                if (ImGui::Button("Save Patch As...", ImVec2(220, 44))) {
                    save_name_buf[0] = '\0';
                    strncpy(save_name_buf, g_loaded_program_name == "(none loaded)" ? "" : g_loaded_program_name.c_str(), sizeof(save_name_buf) - 1);
                    save_category_buf[0] = '\0';
                    show_save_popup = true;
                    g_save_status.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh", ImVec2(140, 44))) {
                    rescan_programs();
                    if (g_hds_addr) lo_send(g_hds_addr, "/hds/request_state", "");
                }

                if (!g_load_status.empty()) { ImGui::Spacing(); ImGui::TextWrapped("%s", g_load_status.c_str()); }
                ImGui::Spacing();

                // Type and Mix panels sit side by side (not stacked) and are both non-scrolling
                // children -- this is a locked kiosk touchscreen with no scroll gesture available,
                // so every control must be reachable without one. Height is whatever's actually
                // left in the page (not a hardcoded guess), so the Mix panel's faders can use
                // essentially the whole remaining height rather than a cramped fixed amount.
                const ImGuiWindowFlags kNoScrollChild = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
                float osc_panel_height = ImGui::GetContentRegionAvail().y - 10.0f;

                ImGui::Text("Oscillator Type:");
                ImGui::SameLine(330);
                ImGui::Text("Oscillator Mix:");

                if (osc_panel_height > 40.0f) {
                    ImGui::BeginChild("OscTypesSection", ImVec2(320, osc_panel_height), ImGuiChildFlags_Borders, kNoScrollChild);
                    draw_osc_type_row(1, 0);  // Saw    -- matches init_default_controls()
                    draw_osc_type_row(2, 3);  // Sine
                    draw_osc_type_row(3, 5);  // Noise
                    draw_osc_type_row(4, 8);  // Off
                    ImGui::EndChild();

                    ImGui::SameLine();

                    ImGui::BeginChild("OscillatorsSection", ImVec2(260, osc_panel_height), ImGuiChildFlags_Borders, kNoScrollChild);
                    // Measure real remaining space INSIDE this child (not a guess back-derived from
                    // the outer panel height) and subtract the label's own actual rendered height --
                    // robust to font/DPI differences. Floored at a sane minimum so VSliderFloat can
                    // never receive a near-zero or negative height: ImGui treats a NEGATIVE size as
                    // "auto-stretch to remaining space", which is what caused the previous "-60.0f"
                    // guess (wrong on a taller font/DPI setup) to make the fader's effective height
                    // erratic and inconsistent across the 4 columns, and its clickable travel so thin
                    // that any touch/drag snapped straight to 0% or 100% instead of a real in-between
                    // value.
                    float mix_avail_height = ImGui::GetContentRegionAvail().y;
                    float label_row_height = ImGui::GetTextLineHeightWithSpacing();
                    float fader_height = mix_avail_height - label_row_height - 4.0f;
                    if (fader_height < 80.0f) fader_height = 80.0f;
                    draw_oscillator_row(1, 80.0f, fader_height);  // 80%   -- matches init_default_controls()
                    ImGui::SameLine();
                    draw_oscillator_row(2, 50.0f, fader_height);  // 50%
                    ImGui::SameLine();
                    draw_oscillator_row(3, 0.0f, fader_height);   // 0%
                    ImGui::SameLine();
                    draw_oscillator_row(4, 0.0f, fader_height);   // 0%
                    ImGui::EndChild();
                }

                if (show_save_popup) ImGui::OpenPopup("Save Program");
                if (ImGui::BeginPopupModal("Save Program", &show_save_popup, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Name:");
                    ImGui::SetNextItemWidth(400);
                    ImGui::InputText("##SaveName", save_name_buf, sizeof(save_name_buf));
                    ImGui::Text("Category (optional):");
                    ImGui::SetNextItemWidth(400);
                    ImGui::InputText("##SaveCategory", save_category_buf, sizeof(save_category_buf));
                    if (!g_save_status.empty()) { ImGui::Spacing(); ImGui::TextWrapped("%s", g_save_status.c_str()); }
                    ImGui::Spacing();
                    if (ImGui::Button("Save", ImVec2(150, 44))) {
                        save_program(save_name_buf, save_category_buf);
                        if (starts_with(g_save_status, "Saved '")) { show_save_popup = false; ImGui::CloseCurrentPopup(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(150, 44))) { show_save_popup = false; ImGui::CloseCurrentPopup(); }
                    ImGui::EndPopup();
                }

                if (show_program_selector) {
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(720, 540), ImGuiCond_Always);

                    ImGuiWindowFlags modal_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

                    if (ImGui::Begin("FullPageProgramSelectorModal", &show_program_selector, modal_flags)) {

                        ImGui::Text("PROGRAM LIBRARY  (%d saved)", (int)g_programs.size());
                        ImGui::SameLine(ImGui::GetWindowWidth() - 50);
                        if (ImGui::Button("X", ImVec2(35, 35))) { show_program_selector = false; }
                        ImGui::Separator(); ImGui::Spacing();

                        ImGui::Text("Filter Search:"); ImGui::SameLine();
                        ImGui::PushItemWidth(450);
                        ImGui::InputText("##SearchInput", search_query, IM_ARRAYSIZE(search_query));
                        ImGui::PopItemWidth();
                        ImGui::Spacing();

                        ImGui::Text("Category:"); ImGui::SameLine();
                        if (current_category_tab >= (int)g_categories.size()) current_category_tab = 0;
                        for (int i = 0; i < (int)g_categories.size(); i++) {
                            bool is_selected = (current_category_tab == i);
                            if (is_selected) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]); }

                            if (ImGui::Button(g_categories[i].c_str(), ImVec2(0, 0))) {
                                current_category_tab = i;
                            }

                            if (is_selected) { ImGui::PopStyleColor(); }
                            if (i < (int)g_categories.size() - 1) { ImGui::SameLine(); }
                        }
                        ImGui::Spacing(); ImGui::Separator();

                        ImGuiTableFlags table_flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
                        float table_height = ImGui::GetContentRegionAvail().y - 65.0f;

                        if (ImGui::BeginTable("SpreadsheetSelector", 2, table_flags, ImVec2(0, table_height))) {
                            ImGui::TableSetupColumn("Patch Name", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                            ImGui::TableHeadersRow();

                            for (int row = 0; row < (int)g_programs.size(); row++) {
                                const LoadedProgram& prog = g_programs[row];

                                if (current_category_tab != 0 && prog.category != g_categories[current_category_tab]) {
                                    continue;
                                }
                                if (strlen(search_query) > 0 && custom_strcasestr(prog.name.c_str(), search_query) == nullptr) {
                                    continue;
                                }

                                ImGui::TableNextRow();

                                ImGui::TableSetColumnIndex(0);
                                bool is_row_selected = (selected_program_idx == row);
                                char select_id[192];
                                snprintf(select_id, sizeof(select_id), "%s##row_%d", prog.name.c_str(), row);
                                if (ImGui::Selectable(select_id, is_row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                                    selected_program_idx = row;
                                }
                                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                    load_program(prog);
                                    show_program_selector = false;
                                }

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%s", prog.category.empty() ? "-" : prog.category.c_str());
                            }
                            ImGui::EndTable();
                        }

                        ImGui::Separator();
                        ImGui::TextDisabled("Selected: %s", selected_program_idx >= 0 && selected_program_idx < (int)g_programs.size()
                                                                 ? g_programs[selected_program_idx].name.c_str() : "(none)");
                        ImGui::SameLine(ImGui::GetWindowWidth() - 260);

                        if (ImGui::Button("Cancel", ImVec2(100, 44))) {
                            show_program_selector = false;
                        }
                        ImGui::SameLine();
                        bool have_selection = selected_program_idx >= 0 && selected_program_idx < (int)g_programs.size();
                        ImGui::BeginDisabled(!have_selection);
                        if (ImGui::Button("Load Patch", ImVec2(120, 44))) {
                            load_program(g_programs[selected_program_idx]);
                            show_program_selector = false;
                        }
                        ImGui::EndDisabled();

                        ImGui::End();
                    }
                }
            }
            break;
            case PAGE_COMB:
                ImGui::Text("COMBINATION MATRIX [COMB]");
                ImGui::Separator();
                ImGui::Text("Placeholder for splitting, layering, and multi-timbral routing.");
                break;

            case PAGE_SEQ:
                ImGui::Text("STEP SEQUENCER [SEQ]");
                ImGui::Separator();
                ImGui::Text("Placeholder for gate arrays, patterns, and clock divisions.");
                break;

            case PAGE_REC:
                ImGui::Text("RECORDER MODULE [REC]");
                ImGui::Separator();
                ImGui::Text("Placeholder for session capture controls and buffer looping.");
                break;

            case PAGE_ADSR:
            {
                ImGui::Text("HYPERPIXEL 4\" HARDWARE INTERFACE LAYOUT");
                ImGui::Separator(); ImGui::Spacing();

                ImGui::Text("Oscillator 1 Waveform:");
                ImGui::PushItemWidth(680);
                float shape_raw = get_state("/hds/vco1/shape", 0.0f);
                int wave_idx = 0;
                for (int i = 0; i < g_wave_count; i++) if (g_wave_values[i] == (int)(shape_raw + 0.5f)) { wave_idx = i; break; }
                if (ImGui::Combo("##WaveformCombo", &wave_idx, g_wave_names, g_wave_count)) {
                    send_osc_float("/hds/vco1/shape", (float)g_wave_values[wave_idx]);
                }
                ImGui::PopItemWidth();
                ImGui::Spacing(); ImGui::Spacing();

                // Ranges/defaults match frontend.py's own Osc 1 Amp EG panel exactly (ADSRPanel("Osc
                // 1 Amp EG", (1,1000), (1,2000), (1,2000)) -- attack 1-1000ms, decay/release
                // 1-2000ms, sustain 0-100%) -- these are EG index 0's real wire units (milliseconds
                // for A/D/R, percent for S), sent as-is with no scaling, matching
                // transmit_knob_packet()'s own `add_f(f"/hds/eg/{eg_idx}/attack",
                // panel.dials["A"].value())` in frontend.py exactly.
                ImGui::Text("Osc 1 Amp Envelope (EG 0):");
                float slider_width = 145.0f, slider_height = 240.0f;

                float attack_ms = get_state("/hds/eg/0/attack", 10.0f);
                if (ImGui::VSliderFloat("A##Attack", ImVec2(slider_width, slider_height), &attack_ms, 1.0f, 1000.0f, "%.0f ms")) {
                    send_osc_float("/hds/eg/0/attack", attack_ms);
                } ImGui::SameLine(0, 20);
                float decay_ms = get_state("/hds/eg/0/decay", 150.0f);
                if (ImGui::VSliderFloat("D##Decay", ImVec2(slider_width, slider_height), &decay_ms, 1.0f, 2000.0f, "%.0f ms")) {
                    send_osc_float("/hds/eg/0/decay", decay_ms);
                } ImGui::SameLine(0, 20);
                float sustain_pct = get_state("/hds/eg/0/sustain", 80.0f);
                if (ImGui::VSliderFloat("S##Sustain", ImVec2(slider_width, slider_height), &sustain_pct, 0.0f, 100.0f, "%.0f%%")) {
                    send_osc_float("/hds/eg/0/sustain", sustain_pct);
                } ImGui::SameLine(0, 20);
                float release_ms = get_state("/hds/eg/0/release", 300.0f);
                if (ImGui::VSliderFloat("R##Release", ImVec2(slider_width, slider_height), &release_ms, 1.0f, 2000.0f, "%.0f ms")) {
                    send_osc_float("/hds/eg/0/release", release_ms);
                }
                ImGui::Spacing(); ImGui::Spacing();

                ImGui::Text("Master Gain Level:");
                ImGui::PushItemWidth(680);
                float master_volume = get_state("/hds/master_volume", 0.80f);
                if (ImGui::SliderFloat("##MasterVolSlider", &master_volume, 0.0f, 1.0f, "%.2f")) {
                    send_osc_float("/hds/master_volume", master_volume);
                }
                ImGui::PopItemWidth();
            }
            break;

            case PAGE_GLOB:
                ImGui::Text("GLOBAL SETTINGS [GLOB]");
                ImGui::Separator();
                ImGui::Text("Placeholder for MIDI channels, tuning scales, and network setups.");
                break;
        }

        // Live status readback -- real data from backend.c's own periodic /hds/status/* bundle,
        // right-justified on the same top row as whatever title text this page just drew (every
        // page's own title is the first item in the switch above, so it always starts at exactly
        // this window's top-left content position) -- shown on every page, not tucked away in one
        // alone, since "is the backend actually there and alive" is a global concern. Liveness
        // mirrors frontend.py's own heuristic exactly: UDP has no connect-time signal, so infer it
        // from whether a status message has arrived recently.
        {
            bool connected = (g_last_status_time != 0.0) && (glfwGetTime() - g_last_status_time < OSC_STATUS_TIMEOUT_S);
            char status_buf[64];
            snprintf(status_buf, sizeof(status_buf), "%s   Peak: %.2f   Tempo: %.1f BPM",
                      connected ? "LIVE" : "NO SIGNAL", g_live_peak, g_live_tempo_bpm);
            ImVec2 text_size = ImGui::CalcTextSize(status_buf);
            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().WindowPadding.y));
            float full_row_width = ImGui::GetContentRegionAvail().x;
            float right_x = ImGui::GetStyle().WindowPadding.x + full_row_width - text_size.x;
            ImGui::SetCursorPos(ImVec2(right_x, ImGui::GetStyle().WindowPadding.y));
            ImGui::TextColored(connected ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", status_buf);
        }

        ImGui::End();

        // --------------------------------------------------------------------
        // BOTTOM HALF: HARDWARE FOOTER NAVIGATION STRIP (720x180)
        // --------------------------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(0, 540));
        ImGui::SetNextWindowSize(ImVec2(720, 180));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("GlobalNavigationStrip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        float button_width = 101.0f;
        float button_height = 130.0f;

        if (ImGui::Button("Prog", ImVec2(button_width, button_height))) { currentPage = PAGE_PROG; } ImGui::SameLine(0, 10);
        if (ImGui::Button("Comb", ImVec2(button_width, button_height))) { currentPage = PAGE_COMB; } ImGui::SameLine(0, 10);
        if (ImGui::Button("Seq", ImVec2(button_width, button_height))) { currentPage = PAGE_SEQ; } ImGui::SameLine(0, 10);
        if (ImGui::Button("Rec", ImVec2(button_width, button_height))) { currentPage = PAGE_REC; } ImGui::SameLine(0, 10);
        if (ImGui::Button("ADSR\nPlcHldr", ImVec2(button_width + 10.0f, button_height))) { currentPage = PAGE_ADSR; } ImGui::SameLine(0, 10);
        if (ImGui::Button("Glob", ImVec2(button_width, button_height))) { currentPage = PAGE_GLOB; }

        ImGui::End();
        ImGui::PopStyleVar();

        // Rendering Output Loop Execution
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Hardware Destruction Cleanup Lifecycle
    if (g_status_server) lo_server_free(g_status_server);
    if (g_hds_addr) lo_address_free(g_hds_addr);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
