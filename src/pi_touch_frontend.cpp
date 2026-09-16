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
// Scope, stated explicitly (a deliberate boundary, not an oversight): this pass wires ONLY the
// ADSR page -- Osc 1's Amp EG (EG index 0: attack/decay/sustain/release), Master Volume, and
// Oscillator waveform -- to real backend fields, plus a live status readback (Peak level, live
// Tempo) via /hds/status/*. The Program Library browser's data is still the original hardcoded
// placeholder table (program_library[]) -- HDS's real saved Programs live as
// ~/HDS_Programs/<name>/<name>.hdsprog JSON files (see frontend.py's own _pick_and_load_program()
// for the real format), and reading/applying those from C++ is a larger, separate piece of work
// this pass does not attempt; its "Load Patch"/double-click actions are now clearly-marked no-ops
// rather than silently firing a message at a nonexistent address. Comb/Seq/Rec/Glob pages remain
// placeholder text, unchanged from the original exploratory draft.
//
// Build: see the Makefile's `pi-touch` target. Needs GLFW3 + OpenGL + liblo development packages
// (apt: libglfw3-dev liblo-dev) -- Dear ImGui itself is vendored into third_party/imgui/ (pinned
// to tag v1.91.5, MIT licensed, github.com/ocornut/imgui) rather than assumed to be a system
// package, mirroring this project's existing CLAP-SDK/miniaudio.h vendoring precedent.

#include <cstdio>
#include <cstring>
#include <cctype>
#include <lo/lo.h>
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/backends/imgui_impl_glfw.h"
#include "../third_party/imgui/backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// --------------------------------------------------------------------------------------------
// OSC transport: liblo, matching backend.c's own transport exactly -- one lo_address to SEND
// knob writes to the backend's listen port, one bound lo_server to RECEIVE /hds/status/*
// (and, once at startup, a /hds/request_state reply) on the backend's reply port. Non-blocking,
// polled once per frame from the main loop (poll_osc(), called alongside glfwPollEvents()) --
// the same single-threaded, no-locking shape backend.c's own main() loop uses for its OSC
// server, and frontend.py's own vu_timer-driven listen_for_status_frames() uses on the GUI side.
// Defaults match backend.c's own --osc-listen-port/--osc-reply-port defaults (9000/9001), so
// this client talks to a stock-launched backend with no extra flags on either side.
// --------------------------------------------------------------------------------------------
static lo_address g_hds_addr = nullptr;      // where we SEND knob writes
static lo_server g_status_server = nullptr;  // where we RECEIVE status + request_state replies

static void send_osc_float(const char* path, float value) {
    if (!g_hds_addr) return;
    lo_send(g_hds_addr, path, "f", value);
}

static void osc_error_handler(int num, const char* msg, const char* path) {
    fprintf(stderr, "[osc] liblo error %d (%s): %s\n", num, path ? path : "?", msg);
}

// --- Live status readback -----------------------------------------------------------------
// Updated from backend.c's own periodic /hds/status/* bundle (send_osc_status(), sent on the
// same cadence the VU meter/tempo readout in frontend.py already consumes). last_status_time
// is this client's own liveness heuristic, mirroring frontend.py's identical "no UDP connect-
// time signal, so infer liveness from whether status has arrived recently" design.
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

// --- Pulling real current state at startup ---------------------------------------------------
// Sending /hds/request_state (see backend.c's own comment on it) makes the backend reply with
// its CURRENT live values for every OSC address it has, chunked across several bundles. This
// client only cares about a handful of them (EG 0's four ADSR fields, master_volume, vco1_shape)
// -- each gets its own exact-address liblo method registered below, via the same "one handler
// per address, pointer to the field it writes" shape backend.c's own dispatch table already
// uses in spirit (just direct pointers here instead of a byte-offset table, since there are only
// six addresses to care about). Every OTHER address in the dump (there are over a thousand) has
// no registered handler and no catch-all is registered either, so liblo silently drops them --
// exactly what's wanted; this client isn't trying to mirror the whole backend's state, just the
// handful of fields its own ADSR page actually shows. Pulling this way, rather than starting
// from hardcoded widget defaults and blindly pushing them, avoids the exact bug frontend.py's own
// request_state path was added to fix: silently overwriting a DAW-restored CLAP patch (or
// another controller's already-live settings) the instant this client's own boot-time state
// happened to differ.
static float g_attack_ms = 10.0f;
static float g_decay_ms = 150.0f;
static float g_sustain_pct = 80.0f;
static float g_release_ms = 300.0f;
static float g_master_volume = 0.80f;

// WaveType's real enum order from backend.c (WAVE_SAW=0, WAVE_SQUARE=1, WAVE_TRIANGLE=2,
// WAVE_SINE=3, WAVE_UNISON_SAW=4, WAVE_NOISE=5, WAVE_PULSE=6, WAVE_SAMPLE=7, WAVE_OFF=8). This
// page only offers the six shapes a performer would sensibly toggle live from a combo box --
// Unison Saw (needs detune/width knobs), Sample (needs a loaded zone), and Off (silences the
// oscillator outright) all need more context than a single dropdown gives them, so they're left
// out of this quick-edit page rather than half-wired.
static const char* g_wave_names[] = { "Sawtooth", "Square", "Triangle", "Sine", "Noise", "Pulse" };
static const int g_wave_values[] = { 0, 1, 2, 3, 5, 6 };
static const int g_wave_count = 6;
static int g_current_wave_idx = 0; // index into g_wave_names/g_wave_values, NOT the raw WaveType value

static int vco1_shape_handler(const char*, const char*, lo_arg** argv, int argc, lo_message, void* user_data) {
    (void)user_data;
    if (argc != 1) return 0;
    int raw = (int)(argv[0]->f + 0.5f);
    for (int i = 0; i < g_wave_count; i++) {
        if (g_wave_values[i] == raw) { g_current_wave_idx = i; return 0; }
    }
    // A shape outside this page's six-entry subset (Unison Saw/Sample/Off) is real backend state
    // this quick-edit page just doesn't represent -- leave the combo wherever it already was
    // rather than silently snapping to an arbitrary entry.
    return 0;
}

static void poll_osc() {
    if (g_status_server) {
        while (lo_server_recv_noblock(g_status_server, 0) > 0) {}
    }
}

enum SynthPage { PAGE_PROG, PAGE_COMB, PAGE_SEQ, PAGE_REC, PAGE_ADSR, PAGE_GLOB };

struct ProgramEntry {
    const char* slot;
    const char* name;
    const char* category;
    const char* author;
    const char* coreType;
    const char* tags;
};

ProgramEntry program_library[] = {
    { "A001", "Neon Horizon Bass", "Bass", "SubFreq", "Sawtooth", "Warm, Analog" },
    { "A002", "Deep Sub Drops", "Bass", "WaveGen", "Sine Wave", "Sub, Clean" },
    { "A003", "Cinematic Pad 5", "Pads", "M.Vinci", "Triangle", "Ethereal, Soft" },
    { "A004", "Cosmic Stardust", "Pads", "Astro", "Sine Wave", "Ambient, Bright" },
    { "A005", "Cyber Shred Lead", "Leads", "PixelBit", "Square Wave", "Aggressive" },
    { "A006", "Plucked Mallets", "Plucks", "SubFreq", "Triangle", "Percussive" },
    { "A007", "Glitch Matrix FX", "FX", "Noisizer", "Noise", "Modulated" }
};
const int total_programs = sizeof(program_library) / sizeof(ProgramEntry);

const char* selector_categories[] = { "All", "Bass", "Pads", "Leads", "Plucks", "FX" };

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

int main() {
    g_hds_addr = lo_address_new("127.0.0.1", "9000");
    g_status_server = lo_server_new_with_proto("9001", LO_UDP, osc_error_handler);
    if (!g_status_server) {
        fprintf(stderr, "[osc] failed to bind reply port 9001 -- status readback and startup "
                         "state pull will not work, but knob writes to the backend still will.\n");
    } else {
        lo_server_add_method(g_status_server, "/hds/status/peak", "f", status_float_handler, &g_live_peak);
        lo_server_add_method(g_status_server, "/hds/status/tempo_bpm_live", "f", status_float_handler, &g_live_tempo_bpm);
        lo_server_add_method(g_status_server, "/hds/eg/0/attack", "f", status_float_handler, &g_attack_ms);
        lo_server_add_method(g_status_server, "/hds/eg/0/decay", "f", status_float_handler, &g_decay_ms);
        lo_server_add_method(g_status_server, "/hds/eg/0/sustain", "f", status_float_handler, &g_sustain_pct);
        lo_server_add_method(g_status_server, "/hds/eg/0/release", "f", status_float_handler, &g_release_ms);
        lo_server_add_method(g_status_server, "/hds/master_volume", "f", status_float_handler, &g_master_volume);
        lo_server_add_method(g_status_server, "/hds/vco1/shape", "f", vco1_shape_handler, nullptr);
        lo_send(g_hds_addr, "/hds/request_state", "");
    }

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
    int selected_program_idx = 0;
    int current_category_tab = 0;
    char search_query[64] = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        poll_osc();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(720, 540));
        ImGui::Begin("ActiveViewPanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        switch (currentPage) {
            case PAGE_PROG:
            {
                ImGui::Text("PROGRAM SETTINGS [PROG]");
                ImGui::Separator(); ImGui::Spacing();
                ImGui::TextDisabled("Browsing a placeholder patch list -- not yet reading HDS's");
                ImGui::TextDisabled("real ~/HDS_Programs/*.hdsprog files (see this file's own");
                ImGui::TextDisabled("header comment for why that's a separate, later piece of work).");
                ImGui::Spacing();

                ImGui::Text("Active Program Profile:");
                ImGui::PushItemWidth(680);

                char strip_label[128];
                snprintf(strip_label, sizeof(strip_label), "%s - %s [Tap to Browse...]",
                         program_library[selected_program_idx].slot,
                         program_library[selected_program_idx].name);

                if (ImGui::Button(strip_label, ImVec2(680, 0))) {
                    show_program_selector = true;
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::TextDisabled("Allocated Core Model: %s", program_library[selected_program_idx].coreType);
                ImGui::TextDisabled("Designer / Author: %s", program_library[selected_program_idx].author);

                if (show_program_selector) {
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(720, 540), ImGuiCond_Always);

                    ImGuiWindowFlags modal_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

                    if (ImGui::Begin("FullPageProgramSelectorModal", &show_program_selector, modal_flags)) {

                        ImGui::Text("PROGRAM LIBRARY SELECTOR");
                        ImGui::SameLine(ImGui::GetWindowWidth() - 50);
                        if (ImGui::Button("X", ImVec2(35, 35))) { show_program_selector = false; }
                        ImGui::Separator(); ImGui::Spacing();

                        ImGui::Text("Filter Search:"); ImGui::SameLine();
                        ImGui::PushItemWidth(450);
                        ImGui::InputText("##SearchInput", search_query, IM_ARRAYSIZE(search_query));
                        ImGui::PopItemWidth();
                        ImGui::Spacing();

                        ImGui::Text("Category:"); ImGui::SameLine();
                        for (int i = 0; i < IM_ARRAYSIZE(selector_categories); i++) {
                            bool is_selected = (current_category_tab == i);
                            if (is_selected) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]); }

                            if (ImGui::Button(selector_categories[i], ImVec2(80, 0))) {
                                current_category_tab = i;
                            }

                            if (is_selected) { ImGui::PopStyleColor(); }
                            if (i < IM_ARRAYSIZE(selector_categories) - 1) { ImGui::SameLine(); }
                        }
                        ImGui::Spacing(); ImGui::Separator();

                        ImGuiTableFlags table_flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
                        float table_height = ImGui::GetContentRegionAvail().y - 65.0f;

                        if (ImGui::BeginTable("SpreadsheetSelector", 6, table_flags, ImVec2(0, table_height))) {
                            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                            ImGui::TableSetupColumn("Patch Name", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                            ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                            ImGui::TableSetupColumn("Engine Core", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Sound Tags", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableHeadersRow();

                            for (int row = 0; row < total_programs; row++) {
                                ProgramEntry& prog = program_library[row];

                                if (current_category_tab != 0 && strcmp(prog.category, selector_categories[current_category_tab]) != 0) {
                                    continue;
                                }

                                if (strlen(search_query) > 0) {
                                    if (custom_strcasestr(prog.name, search_query) == nullptr &&
                                        custom_strcasestr(prog.tags, search_query) == nullptr &&
                                        custom_strcasestr(prog.author, search_query) == nullptr) {
                                        continue;
                                        }
                                }

                                ImGui::TableNextRow();

                                ImGui::TableSetColumnIndex(0);
                                bool is_row_selected = (selected_program_idx == row);

                                char select_id[32];
                                snprintf(select_id, sizeof(select_id), "%s##row_%d", prog.slot, row);
                                if (ImGui::Selectable(select_id, is_row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                                    selected_program_idx = row;
                                }

                                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                    // NOT wired to anything real this pass -- there is no
                                    // "/hds/program/change" address in HDS's real OSC surface
                                    // (a Program is loaded by pushing its whole field set, not one
                                    // integer index -- see this file's header comment). Selecting a
                                    // row just closes the browser; loading real saved Programs from
                                    // this client is future work.
                                    show_program_selector = false;
                                }

                                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", prog.name);
                                ImGui::TableSetColumnIndex(2); ImGui::Text("%s", prog.category);
                                ImGui::TableSetColumnIndex(3); ImGui::Text("%s", prog.author);
                                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", prog.coreType);
                                ImGui::TableSetColumnIndex(5); ImGui::Text("%s", prog.tags);
                            }
                            ImGui::EndTable();
                        }

                        ImGui::Separator();
                        ImGui::TextDisabled("Selected: Slot %s", program_library[selected_program_idx].slot);
                        ImGui::SameLine(ImGui::GetWindowWidth() - 260);

                        if (ImGui::Button("Cancel", ImVec2(100, 44))) {
                            show_program_selector = false;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Load Patch", ImVec2(120, 44))) {
                            // See the double-click handler's own comment above -- not wired yet.
                            show_program_selector = false;
                        }

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
                if (ImGui::Combo("##WaveformCombo", &g_current_wave_idx, g_wave_names, g_wave_count)) {
                    send_osc_float("/hds/vco1/shape", (float)g_wave_values[g_current_wave_idx]);
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

                if (ImGui::VSliderFloat("A##Attack", ImVec2(slider_width, slider_height), &g_attack_ms, 1.0f, 1000.0f, "%.0f ms")) {
                    send_osc_float("/hds/eg/0/attack", g_attack_ms);
                } ImGui::SameLine(0, 20);
                if (ImGui::VSliderFloat("D##Decay", ImVec2(slider_width, slider_height), &g_decay_ms, 1.0f, 2000.0f, "%.0f ms")) {
                    send_osc_float("/hds/eg/0/decay", g_decay_ms);
                } ImGui::SameLine(0, 20);
                if (ImGui::VSliderFloat("S##Sustain", ImVec2(slider_width, slider_height), &g_sustain_pct, 0.0f, 100.0f, "%.0f%%")) {
                    send_osc_float("/hds/eg/0/sustain", g_sustain_pct);
                } ImGui::SameLine(0, 20);
                if (ImGui::VSliderFloat("R##Release", ImVec2(slider_width, slider_height), &g_release_ms, 1.0f, 2000.0f, "%.0f ms")) {
                    send_osc_float("/hds/eg/0/release", g_release_ms);
                }
                ImGui::Spacing(); ImGui::Spacing();

                ImGui::Text("Master Gain Level:");
                ImGui::PushItemWidth(680);
                if (ImGui::SliderFloat("##MasterVolSlider", &g_master_volume, 0.0f, 1.0f, "%.2f")) {
                    send_osc_float("/hds/master_volume", g_master_volume);
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

        ImGui::End();

        // --------------------------------------------------------------------
        // BOTTOM HALF: HARDWARE FOOTER NAVIGATION STRIP (720x180)
        // --------------------------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(0, 540));
        ImGui::SetNextWindowSize(ImVec2(720, 180));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("GlobalNavigationStrip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        // Live status readback -- real data from backend.c's own periodic /hds/status/* bundle,
        // shown on every page (not tucked away in ADSR alone) since "is the backend actually
        // there and alive" is a global concern. Liveness mirrors frontend.py's own heuristic
        // exactly: UDP has no connect-time signal, so infer it from whether a status message has
        // arrived recently.
        bool connected = (g_last_status_time != 0.0) && (glfwGetTime() - g_last_status_time < OSC_STATUS_TIMEOUT_S);
        ImGui::TextColored(connected ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                            "%s   Peak: %.2f   Tempo: %.1f BPM", connected ? "LIVE" : "NO SIGNAL", g_live_peak, g_live_tempo_bpm);

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
