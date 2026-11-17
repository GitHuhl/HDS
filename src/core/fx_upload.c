// fx_upload.c -- FX slot NAM model / cab IR upload over OSC (/hds/fx/upload_begin|chunk|end).
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"

#if HDS_NAM_ENABLED
#include "fx/nam_bridge.h"
#endif

// ============================================================================
// Sequencer transport (Play/Pause/Stop) + per-lane MIDI record/playback
// ============================================================================

// ============================================================================
// HdsEngineState -- the last "tier" of the per-synth-instance state consolidation started at
// HdsSampleSlotState (see that struct's own comment for the full explanation of why this is
// split into several separately-declared structs, and for the overall multi-instance-refactor
// rationale). This tier holds the pieces that only became visible this late in the file
// (SeqMidiLane, TransportState) plus render_audio_block's own former function-local statics
// (FXSlotEngine, the LFO mod-state arrays, the limiter) -- everything that didn't need to move
// to an earlier tier.
//
// Deliberately NOT a bounded MAX_HDS_INSTANCES array of these -- exactly one HdsEngineState (and
// one of each earlier tier) is calloc'd/BSS-allocated per synth "session" (the standalone app's
// own single static instance below; one embedded per CLAP plugin instance), matching CLAP's own
// create_plugin/destroy lifecycle with no artificial instance cap.
//
// This phase (multi-instance Phase 2, "pure consolidation") only regroups this state into one
// struct reached through a single new global, g_engine -- every existing global name below is
// kept alive as a macro expanding to the matching g_engine field, so every one of this file's
// existing read/write sites keeps compiling completely unchanged. Later phases replace these
// macros, one subsystem at a time, with a real `engine` parameter threaded explicitly through
// every function that needs it (see /root/.claude/plans/wobbly-marinating-prism.md, "Multi-
// instance CLAP support").
// ============================================================================

#if HDS_NAM_ENABLED
// --- FX slot NAM model/cab IR upload over OSC -- lets a REMOTE frontend (a different machine, no
// shared filesystem) get a .nam model or cab IR .wav onto THIS instance's own disk, unlike Sample
// Bank zones, which stay purely path-based on purpose: a sample LIBRARY can be gigabytes, genuinely
// impractical to ship over a control-protocol UDP connection, while a .nam model or cab IR is
// typically well under a few MB -- small enough that "just send the bytes" is actually useful
// rather than requiring a shared network filesystem. A same-machine load keeps using the plain
// path fields above unchanged -- a controller only needs to switch to this upload protocol when
// the current target isn't local.
//
// Three-message protocol, ONE shared receive buffer (see HdsEngineState's own field comment on why
// not per-slot): upload_begin allocates/resets it, upload_chunk writes into it at an explicit byte
// offset (not a running index -- a chunk landing out of order over a real, possibly-lossy network
// still lands in the right place), upload_end verifies total size AND a whole-file CRC32 before
// writing anything to disk or touching a real FX slot's path at all. Unlike the periodic status
// stream (fine to silently drop a stale value -- a fresher one is always coming), a truncated or
// corrupted upload must never reach hds_nam_create()/the WAV loader as if it were the real file, so
// a verification failure here just discards the buffer and reports it via the
// /hds/status/fx_upload_result reply -- a controller detects this and retries the WHOLE transfer
// from scratch --
// simpler than a per-chunk ACK/retry scheme, and given how small these files are and how cheap a
// retry is, a perfectly adequate answer to "OSC/UDP is not a reliable transport."
//
// Deliberately does NOT delete the temp file it writes: poll_fx_nam_requests() (see its own
// comment) only actually loads it on a LATER block, once this handler has already returned -- and
// nothing here can wait for that -- so there's no correct point to delete it from. A slot's own
// temp files (named by pid+slot+kind, so a retry overwrites its own previous attempt rather than
// colliding with anything) are left under /tmp for the OS's own normal housekeeping to eventually
// reclaim, the same posture a small number of never-cleaned scratch files anywhere else already
// gets in practice.
static uint32_t hds_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

int fx_upload_begin_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsFxUploadContext *ctx = (HdsFxUploadContext *)user_data;
    HdsEngineState *engine = ctx->engine;
    int slot = argv[0]->i;
    bool is_ir = argv[1]->i != 0;
    int total_size = argv[2]->i;
    uint32_t expected_crc32 = (uint32_t)argv[3]->i; // bit-pattern reinterpret -- see this section's own comment on signed/unsigned wire representation
    // A fresh begin always wins over whatever was previously in flight (dropped, never finished,
    // or simply superseded by a retry) -- matches this whole subsystem's "only one upload realistically
    // in flight at a time" design.
    free(engine->fx_upload_buffer);
    engine->fx_upload_buffer = NULL;
    engine->fx_upload_slot = -1;
    engine->fx_upload_total_size = 0;
    engine->fx_upload_received = 0;
    if (slot < 0 || slot >= FX_SLOT_COUNT || total_size <= 0) return 0;
    engine->fx_upload_buffer = malloc((size_t)total_size);
    if (!engine->fx_upload_buffer) return 0;
    engine->fx_upload_slot = slot;
    engine->fx_upload_is_ir = is_ir;
    engine->fx_upload_total_size = (size_t)total_size;
    engine->fx_upload_expected_crc32 = expected_crc32;
    return 0;
}

int fx_upload_chunk_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argc; (void)msg;
    HdsFxUploadContext *ctx = (HdsFxUploadContext *)user_data;
    HdsEngineState *engine = ctx->engine;
    if (engine->fx_upload_slot < 0 || !engine->fx_upload_buffer) return 0; // no upload in progress -- ignore
    int offset = argv[0]->i;
    int32_t blob_size = argv[1]->blob.size;
    const uint8_t *blob_data = (const uint8_t *)&argv[1]->blob.data;
    if (offset < 0 || blob_size < 0) return 0;
    size_t end = (size_t)offset + (size_t)blob_size;
    if (end > engine->fx_upload_total_size) return 0; // malformed/out-of-range -- ignore rather than overflow the buffer
    memcpy(engine->fx_upload_buffer + offset, blob_data, (size_t)blob_size);
    engine->fx_upload_received += (size_t)blob_size; // best-effort progress count only -- upload_end's CRC32 check is the real gate, not this
    return 0;
}

int fx_upload_end_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    HdsFxUploadContext *ctx = (HdsFxUploadContext *)user_data;
    HdsEngineState *engine = ctx->engine;
    int slot = engine->fx_upload_slot;
    bool is_ir = engine->fx_upload_is_ir;
    bool ok = false;
    const char *error = "no upload in progress";
    if (slot >= 0 && engine->fx_upload_buffer && engine->fx_upload_total_size > 0) {
        uint32_t actual_crc32 = hds_crc32(engine->fx_upload_buffer, engine->fx_upload_total_size);
        if (actual_crc32 != engine->fx_upload_expected_crc32) {
            error = "crc32 mismatch -- transfer corrupted or incomplete";
        } else {
            char temp_path[SAMPLE_PATH_LEN];
            snprintf(temp_path, sizeof(temp_path), "/tmp/hds_fx_upload_%d_%d_%s.tmp", (int)getpid(), slot, is_ir ? "ir" : "model");
            FILE *f = fopen(temp_path, "wb");
            if (!f) {
                error = "failed to open temp file for writing";
            } else {
                size_t written = fwrite(engine->fx_upload_buffer, 1, engine->fx_upload_total_size, f);
                fclose(f);
                if (written != engine->fx_upload_total_size) {
                    error = "failed to write temp file completely";
                } else {
                    // Sets the EXACT SAME field a local (same-machine) path-based load already
                    // uses -- poll_fx_nam_requests() picks up the change on its next per-block
                    // poll exactly as if the user had typed this path in themselves. No separate
                    // load call site to keep in sync with that one.
                    char *dest_field = is_ir ? ctx->knobs->fx[slot].nam_ir_path : ctx->knobs->fx[slot].nam_model_path;
                    snprintf(dest_field, SAMPLE_PATH_LEN, "%s", temp_path);
                    ok = true;
                    error = "ok";
                }
            }
        }
    }
    free(engine->fx_upload_buffer);
    engine->fx_upload_buffer = NULL;
    engine->fx_upload_slot = -1;
    engine->fx_upload_total_size = 0;
    engine->fx_upload_received = 0;
    lo_address source = lo_message_get_source(msg);
    if (source) lo_send(source, "/hds/status/fx_upload_result", "iiis", slot, (int)is_ir, ok ? 1 : 0, error);
    return 0;
}
#endif // HDS_NAM_ENABLED
