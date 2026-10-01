// hds_engine_instance.h: the HdsClapPlugin struct -- despite its name (kept as-is rather than
// renamed across the ~15 hds_clap_*_draw.c files that already take "HdsClapPlugin *p" as their
// own function parameter TYPE, which a rename would need to touch mechanically everywhere for no
// functional gain) this is now the SHARED top-level context struct for BOTH hosts of this native
// Cairo GUI: the CLAP plugin (clap_plugin.c, which populates and reads every field) and the
// standalone app (hds_standalone.c, which only touches .knobs/.gui_host/.ext_in_peak/.fx_gr_db PLUS a
// handful of small, purpose-specific scratch fields added since (sfz_browser_result/
// sfz_browser_target_osc, scl_browser_result, record_folder -- see each field's own comment) that
// exist specifically so a standalone-only tab's own draw-file code has somewhere per-instance to
// keep a transient file-picker result; unlike every OTHER field below, these are read/written by
// BOTH hosts' shared draw files, just genuinely exercised (opened a browser, picked something) only
// from the standalone side today). Every other field here (the CLAP host pointer, this instance's
// own OSC server/ports, the per-instance sample/engine/voice state, the CLAP params shadow, etc.)
// exists because clap_plugin.c's own multi-instance design needs it -- the standalone app is
// single-instance and already has its own equivalent state as plain globals (g_engine,
// g_sample_engine, g_streams_state, etc., see hds_standalone.c), so it simply leaves those fields
// unused rather than duplicating or rewiring them. Splitting this into a smaller GUI-only struct
// and a CLAP-only wrapper would be the more "proper" long-term shape, but would mean changing the
// parameter type on every single draw file's every function -- real, worthwhile follow-up, not
// attempted here.
//
// #include'd by clap_plugin.c (which populates the CLAP-specific fields) and hds_standalone.c
// (which only ever touches the three fields above) -- both AFTER backend.c (needs
// SynthSharedControls/SynthVoice/MonoNoteStack/MidiLiveState/HdsSampleSlotState/HdsStreamsState/
// HdsPendingLoadState/HdsEngineState/HdsSampleEngine/HdsStateRequestContext/HdsSmfImportContext/
// HdsSmfTrackImportContext/HdsSubscribeContext/HdsFxUploadContext, all defined there) and AFTER
// hds_fx_gui_host.h (needs HdsFxGuiHost).

// hds_clap_source_label: text for Source index `idx` (Program 1..COMBI_PROGRAM_COUNT, then Combi
// Mix, then Ext In
// 1..EXT_IN_PAIR_COUNT, then Drum Bus 1..DRUM_BUS_COUNT -- see DRUM_BUS_COUNT's own comment in
// backend.c) so a slot's Source combo and a DAW host's own port names (see clap_plugin.c's own
// clap_audio_ports_get()) name the same thing identically. Generic enough that the standalone
// app's own gui_host wiring uses this exact same function too, not a re-transcription of the same
// lines. Drum Buses have no dedicated per-zone assignment UI yet (see SampleSlot.drum_bus's own
// comment -- OSC/SFZ-only for now), but this label still needs to be correct now: without it, this
// same Source combo (already live for Program/Ext-In sources) would show a bogus "Ext In N" for
// every Drum Bus id once SOURCE_COUNT grew to include them.
static void hds_clap_source_label(int idx, char *buf, size_t buflen) {
    if (idx < COMBI_PROGRAM_COUNT) snprintf(buf, buflen, "Program %d", idx + 1);
    else if (idx == COMBI_PROGRAM_COUNT) snprintf(buf, buflen, "Combi Mix");
    else if (idx < DRUM_BUS_SOURCE_BASE) snprintf(buf, buflen, "Ext In %d", idx - COMBI_PROGRAM_COUNT);
    else snprintf(buf, buflen, "Drum Bus %d", idx - DRUM_BUS_SOURCE_BASE + 1);
}

// ---------------------------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------------------------
// Multi-instance refactor: every piece of process-wide state the engine's call graph reaches
// (the sample-streaming tiers, the Sequencer/transport tier) is now a field of this struct instead
// of a file-scope global -- see HdsSampleSlotState/HdsStreamsState/HdsPendingLoadState/
// HdsEngineState's own comments in backend.c for why each is its own separately-declared tier.
// sample_engine is the small bundle of pointers into the three sample-streaming tiers that the
// engine's own functions take as a single parameter (see HdsSampleEngine's comment) -- it's set up
// once in clap_factory_create_plugin() right after this struct is allocated, since its pointer
// members depend on this instance's own address. The standalone app's own equivalent state lives
// in plain globals instead (single-instance, no need for this per-instance bundling) -- see this
// header's own top comment.
// ---------------------------------------------------------------------------------------------

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;

    SynthSharedControls knobs;
    SynthVoice voices[MAX_VOICES];
    MonoNoteStack mono_stacks[COMBI_PROGRAM_COUNT + 1];
    float global_lfo_phases[COMBI_PROGRAM_COUNT + 1][LFO_COUNT];
    MidiLiveState midi_live;

    HdsSampleSlotState sample_slot_state;
    HdsStreamsState streams_state;
    HdsPendingLoadState pending_load_state;
    HdsEngineState engine;
    HdsSampleEngine sample_engine;

    // CLAP multi-out: one extra stereo audio-port per Combi Program (see clap_audio_ports_get()),
    // alongside the existing main stereo mix. A per-instance field, not a process()-local stack
    // array (BUFFER_SIZE * COMBI_PROGRAM_COUNT * 2 floats = 64KB) -- a real-time audio callback's
    // own stack is a scarcer, less predictable resource than this struct's already-heap/static-
    // lifetime allocation, and this buffer needs no more per-call isolation than chunk_buffer's
    // own comment already requires (per-instance is enough; concurrent instances already don't
    // share this struct).
    float per_program_render_buf[BUFFER_SIZE * COMBI_PROGRAM_COUNT * 2];

    // CLAP audio input (see clap_audio_ports_get()): one interleaved scratch buffer, same
    // per-instance-field reasoning as per_program_render_buf just above (a real-time callback's
    // own stack is scarcer than this struct's lifetime, and two instances must never share one
    // buffer). Holds up to EXT_IN_PAIR_COUNT stereo pairs interleaved exactly the way
    // render_audio_block's own ext_in_buffer parameter already expects (pair p at channels
    // [2p, 2p+1]) -- built fresh each process() call from whichever CLAP input ports the host
    // actually connected; see this file's own process() for how.
    float ext_in_render_buf[BUFFER_SIZE * EXT_IN_PAIR_COUNT * 2];

    // Routing tab's own per-ext-in-pair VU meters (see gui_routing_tab_visible's own comment in
    // backend.c): a persistent, per-instance peak-hold, updated every process() call by
    // render_audio_block()'s own ext_in_peak_out parameter (see clap_plugin_process()'s own call
    // site) and read directly by hds_clap_routing_draw.c's own render function -- this instance's
    // own real GUI lives in-process (no OSC round-trip, see hds_clap_gui_draw.c's own header
    // comment), so it can just read this struct field straight off the audio thread's last-written
    // value instead of needing the standalone app's own /hds/status/ext_in_peak/<pair> OSC poll.
    // gui_routing_tab_visible itself is set/cleared by hds_clap_gui_render() -- see that function's
    // own comment -- so this only actually updates (stays non-zero) while the Routing tab is the
    // active tab, matching the standalone app's own perf-gating intent exactly.
    float ext_in_peak[EXT_IN_PAIR_COUNT];

    // Same convention as ext_in_peak just above, for the FX tab's own per-slot GR (gain reduction)
    // meters: process_fx_chain()'s own gr_db_out out-param is a per-call LOCAL scratch array (see
    // its own comment in fx_chain.c) that used to only ever reach the OSC status message for the
    // old Qt frontend's own /hds/status/fx_gr/<slot> listener -- there was nowhere for the native
    // Cairo GUI to read it from at all. This array is that missing persistent home: copied from the
    // scratch array at the end of every process() call (clap_plugin.c) or audio callback
    // (hds_standalone.c), then handed to hds_fx_gui_draw.c via gui_host.fx_gr_db (see
    // HdsFxGuiHost's own comment) so the FX tab can draw a live per-slot dB readout.
    float fx_gr_db[FX_SLOT_COUNT];

    // Same convention as fx_gr_db just above, for FX_TYPE_BROADCAST_DELAY's own per-slot dump-
    // availability meter: render_audio_block's own dump_avail_out out-param writes directly into
    // this persistent array (0 for every slot not currently running that type), handed to
    // hds_fx_gui_draw.c via gui_host.fx_dump_avail_sec.
    float fx_dump_avail_sec[FX_SLOT_COUNT];

    // CLAP params extension (see HDS_CLAP_PARAMS/hds_clap_push_changed_params further down) --
    // one shadow value per exposed param, used purely to detect an OSC/GUI-driven change so it
    // can be echoed to the host as an output event. Sized/initialized once the table itself is
    // known to exist (clap_factory_create_plugin sets every entry to that param's default_value).
    double clap_param_shadow[64]; // >= HDS_CLAP_PARAM_COUNT; a plain fixed size since the table is a compile-time constant and this is just a shadow cache, not wire-visible

    int vu_decimator;

    // The Tempo knob's own value (SynthSharedControls.tempo_bpm, via ProgramState -- see
    // compute_current_tempo_bpm()) is NOT what the audio engine actually reads once
    // knobs.seq_sync_source is set to External: clap_plugin_process() then takes the CLAP host's
    // own process->transport->tempo instead every block (see that function's own comment) --
    // process->transport is a per-call stack parameter the GUI thread can never reach, so this
    // field is where clap_plugin_process() stashes the ACTUAL resolved tempo every block, for
    // hds_clap_global_draw.c's own Tempo knob to read and display while synced to the host
    // (matches Internal-mode's tempo_bpm exactly whenever sync is off, so it's always safe to read
    // regardless of the current sync mode).
    float host_tempo_bpm;

    lo_server osc_server;
    lo_address osc_reply_addr;
    bool osc_ready; // true once activate() successfully opened the OSC server this session
    int osc_listen_port, osc_reply_port; // the actual bound pair, once osc_ready (see the bind-retry in activate())

    // "/hds/request_state" support -- see HdsStateRequestContext's own comment in backend.c. Lets
    // a frontend that just connected to THIS instance's OSC port pull its current live state
    // instead of blindly pushing GUI defaults over a DAW-restored patch. Replies go directly to
    // whoever asked (lo_message_get_source), not a stored address -- these context structs just
    // carry what each handler needs to actually DO the request, not where to reply.
    HdsStateRequestContext state_request_ctx;
    // "/hds/song/import_smf" support -- see HdsSmfImportContext's own comment in backend.c.
    HdsSmfImportContext smf_import_ctx;
    // "/hds/song/import_smf_track" support (per-lane "Load MIDI...").
    HdsSmfTrackImportContext smf_import_track_ctx;
    // "/hds/subscribe" support (Instance Switcher) -- see HdsSubscribeContext/subscribe_handler's
    // own comment in backend.c. target points directly at THIS instance's own osc_reply_addr field
    // just above, which send_osc_status()/the status-push call site in process() already read from.
    HdsSubscribeContext subscribe_ctx;
#if HDS_NAM_ENABLED
    // FX slot NAM model/cab IR upload support -- see HdsFxUploadContext/fx_upload_begin_handler's
    // own comment in backend.c. Gated like the rest of NAM support since it exists purely to feed
    // that FX type.
    HdsFxUploadContext fx_upload_ctx;
#endif

    // The view hds_fx_gui_draw.c's FX-tab engine actually operates on -- see HdsFxGuiHost's own
    // header comment. Populated once, in clap_factory_create_plugin() (or the standalone app's own
    // equivalent setup), with a pointer into THIS instance's own knobs.fx[].
    HdsFxGuiHost gui_host;
    // Opaque HdsClapGuiWindow*/HdsStandaloneGuiWindow* (hds_clap_gui_x11.c / hds_standalone_gui_x11.c) --
    // NULL whenever no GUI is open. This plugin's own window is a whole-plugin concept (a tab bar
    // dispatching across several tabs), not an FX-tab-specific one, so it lives here rather than
    // inside gui_host.
    void *gui_window;

    // Transient result of the shared file browser's own .sfz pick (see HdsFxBrowser's own comment
    // in hds_fx_gui_draw.c on why this ONE modal instance serves every "pick a file" need) --
    // hds_clap_play_draw.c's own Multisample "Load .sfz..." button opens the browser with this as
    // its dest, then its own render function polls it every frame: a non-empty string here means a
    // pick just completed since the last check, gets fed straight into hds_load_sfz_for_osc() (the
    // exact same function the /hds/zone/load_sfz OSC method and --sfz-vcoN startup flags already
    // call -- see backend.c's own comment), and is immediately cleared back to "" so the load fires
    // exactly once. Unlike NAM's own persistent nam_model_path/nam_ir_path (which the DSP itself
    // reads every block), there's no persistent "current sfz path" field to hold this in for
    // long -- an SFZ load populates real, already-persistent Sample Bank zones (owner_program-
    // scoped, saved/restored with the rest of the program) instead, so this buffer only ever needs
    // to survive from one click to the next render.
    char sfz_browser_result[SAMPLE_PATH_LEN];
    int sfz_browser_target_osc; // which oscillator (0-3) the currently-open/just-closed pick was for

    // Same transient-pick convention as sfz_browser_result just above, for the Global sidebar's own
    // "Load Scale (.scl)..." button -- hds_clap_global_draw.c's own render function polls this every
    // frame and feeds a non-empty pick straight into hds_tuning_load_scl() (backend.c), then clears
    // it. No per-oscillator target needed here (unlike SFZ): a scale is one session-level setting.
    char scl_browser_result[SAMPLE_PATH_LEN];

    // The Recording tab's own remembered output folder (standalone-only tab, see
    // hds_clap_recording_draw.c) -- unlike sfz_browser_result/scl_browser_result above, this is NOT
    // a one-shot transient pick: it's the folder every subsequent take gets written into, so the
    // shared file browser's own `dest` points DIRECTLY at this field (no separate poll-and-clear
    // step needed, since picking a new folder has no immediate action to trigger -- it only matters
    // the next time Record is pressed). Defaults to "~/HDS_Recordings" the first time the Recording
    // tab renders (see that file's own lazy-init comment).
    char record_folder[RECORD_PATH_LEN];
} HdsClapPlugin;
