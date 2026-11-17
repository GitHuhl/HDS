// fx_chain.c -- the FX slot bus: the per-slot state lifecycle, the Series/Parallel routing
// dispatcher (process_fx_chain), and the shared WAV loader. Each FX_TYPE_* effect's own DSP lives in
// src/fx/ (one file per family, see src/fx/fx_effects.h). Linked into synth_engine, hds.clap and
// hds_fx.clap; see fx_chain.h for the public interface. Slot and source counts are runtime
// parameters, so the synth's 12-slot bus and HDS FX's 32-slot bus share the same objects.
//
// FXSlotParams/FXSlotEngine and every FX_TYPE_* value live in fx_types.h.

#include "fx_chain.h"
#include "fx/fx_effects.h" // per-effect run_fx_*() and meter readback (src/fx/*.c)

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HDS_NAM_ENABLED
#define HDS_NAM_ENABLED 1
#endif
#if HDS_NAM_ENABLED
#include "nam_bridge.h"
#endif

void reset_fx_engine(FXSlotEngine *engine, int slot_count) {
    for (int s = 0; s < slot_count; s++) { engine[s].active_type = FX_TYPE_OFF; engine[s].state = NULL; }
}

// Reads an entire WAV file into a newly malloc'd interleaved float buffer. Moved here (from
// sequencer.c, its original home) because it now has TWO independent callers that don't share a
// translation unit: sequencer.c's own audio lanes (standalone-only, via hds_standalone.c) and
// FX_TYPE_NAM's cab IR loader just below (needed in every translation unit that runs the FX bus at
// all, including both CLAP plugins) -- fx_chain.c is the one file already #include'd by all three
// (backend.c, hence hds_standalone.c and clap_plugin.c, plus hds_fx_plugin.c directly), so this is
// where a function two of them need independently of each other has to live. Deliberately OUTSIDE
// the `#if HDS_NAM_ENABLED` guard just below -- sequencer.c's own use of it has nothing to do with
// NAM at all, so it must stay available even in a hypothetical build with NAM compiled out.
// Deliberately separate from load_sample_slot() (which only caches a short pre-roll and hands the
// rest to on-demand disk streaming) -- an audio lane's whole clip lives in RAM at once, so this is
// a plain one-shot full-file decode. Keeps every channel present in the file (unlike the sampler's
// mono/stereo-only pre-roll) since the caller, not this loader, should decide how to handle >2
// channels. Returns NULL on any failure (out_frames/out_channels/out_sample_rate left at 0).
float *seq_audio_load_wav_from_disk(const char *path, int *out_frames, int *out_channels, int *out_sample_rate) {
    *out_frames = 0; *out_channels = 0; *out_sample_rate = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[seq-audio] failed to open '%s': %s\n", path, strerror(errno)); return NULL; }

    char chunk_id[4];
    fseek(f, 12, SEEK_SET);
    if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    while (strncmp(chunk_id, "fmt ", 4) != 0) {
        unsigned int chunk_size;
        if (fread(&chunk_size, 4, 1, f) != 1) { fclose(f); return NULL; }
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    }

    unsigned int subchunk1_size; unsigned short audio_format, num_channels;
    unsigned int sample_rate, byte_rate; unsigned short block_align, bits_per_sample;
    if (fread(&subchunk1_size, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&audio_format, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&num_channels, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&sample_rate, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&byte_rate, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&block_align, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&bits_per_sample, 2, 1, f) != 1) { fclose(f); return NULL; }
    (void)byte_rate; (void)block_align;

    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "[seq-audio] '%s': unsupported WAV format tag %u (need 1=PCM or 3=float)\n", path, audio_format);
        fclose(f); return NULL;
    }
    if (audio_format == 1 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "[seq-audio] '%s': unsupported %u-bit PCM\n", path, bits_per_sample);
        fclose(f); return NULL;
    }
    if (audio_format == 3 && bits_per_sample != 32) {
        fprintf(stderr, "[seq-audio] '%s': float WAV must be 32-bit\n", path);
        fclose(f); return NULL;
    }
    if (num_channels == 0) { fclose(f); return NULL; }
    if (subchunk1_size > 16) fseek(f, subchunk1_size - 16, SEEK_CUR);

    if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    while (strncmp(chunk_id, "data", 4) != 0) {
        unsigned int chunk_size;
        if (fread(&chunk_size, 4, 1, f) != 1) { fclose(f); return NULL; }
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    }
    unsigned int data_size;
    if (fread(&data_size, 4, 1, f) != 1) { fclose(f); return NULL; }

    int bytes_per_sample = bits_per_sample / 8;
    unsigned int total_frames = data_size / (unsigned int)(num_channels * bytes_per_sample);
    if (total_frames == 0) { fclose(f); return NULL; }

    float *pcm = malloc((size_t)total_frames * num_channels * sizeof(float));
    if (!pcm) { fprintf(stderr, "[seq-audio] '%s': out of memory (%u frames x %u ch)\n", path, total_frames, num_channels); fclose(f); return NULL; }

    for (unsigned int i = 0; i < total_frames * num_channels; i++) {
        float sample = 0.0f;
        bool ok = true;
        if (audio_format == 1) {
            if (bits_per_sample == 16) {
                short raw16;
                if (fread(&raw16, 2, 1, f) != 1) ok = false; else sample = (float)raw16 / 32768.0f;
            } else if (bits_per_sample == 24) {
                unsigned char b24[3];
                if (fread(b24, 1, 3, f) != 3) ok = false;
                else { int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); sample = (float)v / 2147483648.0f; }
            } else {
                int raw32;
                if (fread(&raw32, 4, 1, f) != 1) ok = false; else sample = (float)raw32 / 2147483648.0f;
            }
        } else {
            if (fread(&sample, 4, 1, f) != 1) ok = false;
        }
        if (!ok) { fprintf(stderr, "[seq-audio] '%s': truncated read at sample %u\n", path, i); free(pcm); fclose(f); return NULL; }
        pcm[i] = sample;
    }
    fclose(f);
    *out_frames = (int)total_frames;
    *out_channels = (int)num_channels;
    *out_sample_rate = (int)sample_rate;
    return pcm;
}

// Runs the FX_SLOT_COUNT-slot FX bus in place on one stereo sample. Called once per sample
// (not per voice) on the fully summed mix -- see render_audio_block, where this sits after all
// voices (and the external input) have been added together, and before master_volume is
// applied. Ported effects land as new cases in the inner switch; each one reads/writes its own
// fx_engine[s].state (lazily allocating it on first call after a type change) and is
// responsible for freeing it if it allocated anything, via a matching branch in the "type
// changed" block below. Until effects are ported in, every fx_type other than Off is unreached
// and simply passes the dry signal through unchanged, same as Off.
//
// routing_mode selects how source_id's OWN assigned slots combine. Both of this file's callers
// (backend.c's render_audio_block, hds_fx_plugin.c's process()) always pass 1 (Parallel) --
// neither exposes a way to pick Series anymore (chain_to_next fully expresses what Series used to
// mean, see its own comment on FXSlotParams in fx_types.h), so mode 0 below is effectively dead
// code, kept only because this function's signature still accepts it: 0 = Series, each of this
// source's slots fed the previous one's output (in physical slot-array order -- a slot not
// assigned to this source is simply skipped,
// so Series order among a source's own slots is still exactly array order), *l/*r updated in
// place after every owned slot. FXSlotParams.chain_to_next is entirely IGNORED in this mode --
// every slot is already chained unconditionally, so there's nothing left for it to express.
//
// 1 = Parallel, GROUPED: each owned slot starts a fresh "branch" fed by the SAME original dry
// input, UNLESS the previous owned slot had chain_to_next set, in which case this slot instead
// continues that branch -- fed by the previous slot's own output, not the shared dry input --
// exactly like Series would, just scoped to this one branch. A branch closes (and its final
// result is added into the running sum) the moment a slot doesn't continue it: either the next
// owned slot didn't ask to (chain_to_next left off, the default), or there is no next owned slot.
// The sum is then divided by the number of BRANCHES that closed, not the number of raw slots that
// participated -- chaining N slots together still counts as exactly one branch in the average, the
// same weight a single unchained slot would carry, matching how a parallel mixer bus should treat
// "one processed signal" whether it took one plugin or three to get there. An Off/bypassed slot
// still passes its own dry input through as its output unchanged (same "contributes its own
// unprocessed dry copy" rule this mode has always had -- unlike the flat filter chain's own
// Parallel mode, see render_audio_block's filter dispatch, where an Off slot is excluded from the
// sum entirely) and the branch it's part of continues exactly as if it had done real work.
// With no slot anywhere setting chain_to_next (every FXSlotParams' own default), every owned slot
// opens and immediately closes its own one-slot branch -- reproducing today's original Parallel
// behavior byte-for-byte, just computed through the more general branch bookkeeping.
//
// Both modes: a source using only 1 or 2 slots in Parallel isn't attenuated as if all 12 had
// participated (the divisor is real branch count, not FX_SLOT_COUNT). A source that owns zero
// slots (Series or Parallel alike) is a correct no-op: *l/*r pass through unchanged.
//
// Called once per source per sample (not per voice) -- see render_audio_block, which loops this
// over every source (Program + each ext-in pair) on that source's own dry bus, after all voices
// have been summed (for the Program bus) and before that source's contribution is added into
// whichever output pair(s) it's routed to. fx_engine[] stays indexed by physical slot id
// regardless of which source owns a slot this block, so a slot's lazily-allocated DSP state
// (e.g. a delay's buffered tail) survives a live source reassignment exactly like it already
// survives a live bypass toggle.
// Type-change bookkeeping (free a slot's old effect state when its Type changed) -- a per-
// PHYSICAL-SLOT task, completely independent of which source is dispatching. Used to run inside
// process_fx_chain() itself, once per call; now a separate, explicitly-named step the CALLER runs
// once per block (see render_audio_block, right before its per-sample loop), since a slot's Type
// can only ever change in response to an OSC message drained between blocks, never mid-block --
// with SOURCE_COUNT sources each independently calling process_fx_chain() every single sample,
// redoing this scan from inside that function meant redoing it SOURCE_COUNT times per sample for
// no reason (a real, measured underrun regression at any voice count, since it's a flat per-
// sample tax paid regardless of patch complexity). PRECONDITION for process_fx_chain() below:
// this must be called at least once after any fx_type change and before that slot is dispatched --
// render_audio_block satisfies this once per block; a test or other direct caller driving
// process_fx_chain() outside render_audio_block must call this itself first.
void sync_fx_engine_types(const FXSlotParams *fx_params, FXSlotEngine *fx_engine, int slot_count) {
    for (int s = 0; s < slot_count; s++) {
        int type = (int)fx_params[s].fx_type;
        if (type != fx_engine[s].active_type) {
            // FX_TYPE_NAM's state owns real C++ objects (via the bridge) that a bare free() would
            // leak rather than destroy -- see fx_nam_free_state()'s own comment. Every other
            // effect's state is plain data, safe for the generic free() below exactly as before.
#if HDS_NAM_ENABLED
            if (fx_engine[s].active_type == FX_TYPE_NAM && fx_engine[s].state) {
                fx_nam_free(fx_engine[s].state);
            }
#endif
            free(fx_engine[s].state);
            fx_engine[s].state = NULL;
            fx_engine[s].active_type = type;
        }
    }
}

// sc_snapshot_l/r: every source's own pre-FX dry signal THIS SAME SAMPLE, frozen before any
// source's own FX chain (including this call) has mutated its bus_l/bus_r in place -- see
// FXSlotParams.sidechain_source's own comment for why a live/mutating read would give a slot an
// order-dependent (sometimes pre-FX, sometimes post-FX) view of another source depending purely on
// which SOURCE_COUNT index happens to be processed first. Read-only here; only FX_TYPE_FAIRKID67
// currently uses it, via an explicit index lookup right after dry_l/dry_r below -- every other
// effect ignores it completely, same posture as any other per-slot field it doesn't read.
// gr_db_out[FX_SLOT_COUNT] (required, not NULL-able -- same convention as render_audio_block's own
// ext_in_peak_out): this call writes gr_db_out[s] for every slot s it owns (fx_params[s].fx_source
// == source_id), 0.0f for a bypassed/off/non-GR-capable slot, or that slot's own effect state's
// last_gr_db otherwise. A slot NOT owned by this source_id is left untouched by this call -- since
// render_audio_block calls this once per source and slots partition cleanly by fx_source, every
// slot ends up written by exactly the one call that owns it (see render_audio_block's own zeroing
// of the whole array before its source loop, which covers a source with zero owned slots).
// slot_out_l/r[FX_SLOT_COUNT] (required, not NULL-able, PERSISTENT across calls -- owned by the
// caller's own long-lived engine state, e.g. HdsEngineState/HdsFxPlugin, never a stack array):
// every slot s this call actually processes (owned or not -- see the Off/bypassed early-skip path
// below) gets its own just-computed post-mix output written here unconditionally, in POOL-SLOT
// order rather than per-source. This is what FXSlotParams.input_from_slot reads from (see that
// field's own comment) to let one slot's Input be explicitly another slot's real output, anywhere
// in the shared pool, independent of fx_source/chain_to_next's own same-source array-position
// adjacency. A slot whose OWN source hasn't run yet this sample simply still holds last sample's
// value here -- the same frozen-snapshot tradeoff sc_snapshot_l/r above already accepts.
void process_fx_chain(const FXSlotParams *fx_params, FXSlotEngine *fx_engine, int slot_count, int routing_mode, int source_id, const float *sc_snapshot_l, const float *sc_snapshot_r, int source_count, float *l, float *r, float *gr_db_out, float *dump_avail_out, float *slot_out_l, float *slot_out_r) {
    // A NaN/Inf sample reaching a slot is never a valid audio value, but several ported effects'
    // own persistent state (e.g. run_fx_tapedelay2's sweepL/R, accumulated from squared input) has
    // no boundary check of its own -- once poisoned, a non-finite value can propagate into a
    // floorf()-derived array index, which is undefined behavior in C and a real crash/heap-
    // corruption vector, not just a silent glitch. Sanitized once here, at the one chokepoint every
    // slot's input already flows through, rather than in each of the ~20 ported effects individually.
    if (!isfinite(*l)) *l = 0.0f;
    if (!isfinite(*r)) *r = 0.0f;
    float parallel_in_l = *l, parallel_in_r = *r;
    float parallel_sum_l = 0.0f, parallel_sum_r = 0.0f;
    int parallel_branch_count = 0;

    // Parallel-only branch state (unused, and harmless, in Series mode): branch_l/r is the
    // running output of whichever branch is currently open; branch_open is false until the first
    // owned slot of a new branch runs; continue_branch is true when the PREVIOUS owned slot's own
    // chain_to_next asked THIS slot to continue its branch rather than start a fresh one.
    float branch_l = parallel_in_l, branch_r = parallel_in_r;
    bool branch_open = false;
    bool continue_branch = false;

    for (int s = 0; s < slot_count; s++) {
        if ((int)fx_params[s].fx_source != source_id) continue; // not this source's slot
        int type = fx_engine[s].active_type;

        // Explicit cross-slot Input override (FXSlotParams.input_from_slot) always wins over
        // fx_source/chain_to_next when set -- see that field's own comment. Never trust a
        // self-reference even though the GUI already refuses to let one be picked: wire/state data
        // (a loaded project, a live OSC write) is never assumed sane just because the UI is.
        int input_from = (fx_params[s].input_from_slot >= 0.0f) ? (int)(fx_params[s].input_from_slot + 0.5f) : -1;
        if (input_from == s || input_from < 0 || input_from >= slot_count) input_from = -1;

        float dry_l, dry_r;
        if (input_from >= 0) {
            // Starts a fresh one-slot branch fed by another slot's own last-computed output,
            // completely independent of continue_branch -- this slot can never ALSO be a genuine
            // chain_to_next continuation target for real (the GUI only lets input_from_slot be set
            // on a chain head), so there's nothing to preserve by honoring continue_branch here.
            if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; }
            dry_l = slot_out_l[input_from]; dry_r = slot_out_r[input_from];
            branch_open = true;
        } else if (routing_mode == 0) {
            dry_l = *l; dry_r = *r; // Series -- chain_to_next ignored, always the running chain result
        } else if (continue_branch) {
            dry_l = branch_l; dry_r = branch_r; // Parallel, continuing an already-open branch
        } else if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) {
            // Parallel, NOT continuing a branch, and this slot does nothing (Off or bypassed): a
            // pure dry-passthrough slot like this would otherwise open its own one-slot branch,
            // averaged in at full weight against every real effect sharing this source -- diluting
            // them proportionally to how many unused/Off slots merely happen to share this source,
            // never a musically intended result. This is exactly what a shared FX-slot pool makes
            // likely (e.g. HDS FX's own pool defaults every never-configured slot to the same
            // source) -- confirmed directly: a single real effect sharing a source with 31 default
            // Off slots was audible at roughly 1/32 its real level before this fix. Skip it
            // entirely instead: no branch opens, gr_db_out already defaults to 0.0f for it below,
            // and the NEXT owned slot starts fresh rather than "continuing" a branch that never
            // really existed. A source with ONLY Off/bypassed slots therefore still ends with
            // parallel_branch_count == 0 -- the exact same, already-correct "no owned slots"
            // passthrough this function's own finalization below already handles.
            gr_db_out[s] = 0.0f;
            dump_avail_out[s] = 0.0f;
            continue_branch = false;
            slot_out_l[s] = parallel_in_l; slot_out_r[s] = parallel_in_r; // still publish something sane for any OTHER slot's input_from_slot pointed at this one
            continue;
        } else {
            if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; }
            dry_l = parallel_in_l; dry_r = parallel_in_r; // Parallel, starting a fresh branch
            branch_open = true;
        }

        // Sidechain key lookup (see this function's own sc_snapshot_l/r comment above and
        // FXSlotParams.sidechain_source's own comment): "Same as Source" (the default, and any
        // out-of-range value) self-keys off this slot's own dry input, exactly reproducing
        // ordinary non-external-sidechain compression.
        float sc_l, sc_r;
        int sc_src = (int)fx_params[s].sidechain_source;
        if (sc_src < 0 || sc_src >= source_count) {
            sc_l = dry_l; sc_r = dry_r;
        } else {
            sc_l = sc_snapshot_l[sc_src]; sc_r = sc_snapshot_r[sc_src];
        }

        float out_l, out_r;
        gr_db_out[s] = 0.0f; // default: bypassed/off/non-GR-capable -- overwritten below for the 4 GR-capable types
        dump_avail_out[s] = 0.0f; // default: not a broadcast-delay slot -- overwritten below for FX_TYPE_BROADCAST_DELAY
        if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) {
            out_l = dry_l; out_r = dry_r; // pass-through, dry
        } else {
            float wet_l = dry_l, wet_r = dry_r;
            switch (type) {
                case FX_TYPE_ENSEMBLE: run_fx_ensemble(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_CREAMCOAT: run_fx_creamcoat(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_DENSITY3: run_fx_density3(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_STEREOCHORUS: run_fx_stereochorus(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_STEREODOUBLER: run_fx_stereodoubler(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_TAPEDELAY2: run_fx_tapedelay2(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_POINTYGUITAR: run_fx_pointyguitar(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_GUITARCONDITIONER: run_fx_guitarconditioner(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_CABS2: run_fx_cabs2(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
#if HDS_NAM_ENABLED
                case FX_TYPE_NAM: run_fx_nam(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
#endif
                case FX_TYPE_GATE:
                    run_fx_gate(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_gate_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_EQ3: run_fx_eq3(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_LIMITER:
                    run_fx_limiter(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_limiter_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_FETCOMP:
                    run_fx_fetcomp(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_fetcomp_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_FAIRKID67:
                    run_fx_fairkid67(&fx_engine[s], &fx_params[s], &wet_l, &wet_r, sc_l, sc_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_fairkid67_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_PEAKMAX:
                    run_fx_peakmax(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_peakmax_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_MULTIFILTER: run_fx_multifilter(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_OPTOCOMP:
                    run_fx_optocomp(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_optocomp_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_DEESSER:
                    run_fx_deesser(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_deesser_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_HARDTUNE: run_fx_hardtune(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_FDNVERB: run_fx_fdnverb(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_SSL4K: run_fx_ssl4k(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_DBX160:
                    run_fx_dbx160(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = fx_dbx160_gr_db(fx_engine[s].state);
                    break;
                case FX_TYPE_BROADCAST_DELAY:
                    run_fx_broadcastdelay(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) dump_avail_out[s] = fx_broadcastdelay_dump_avail_sec(fx_engine[s].state);
                    break;
                case FX_TYPE_DBX120X: run_fx_dbx120x(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_OFF:
                default:
                    break; // pass-through
            }
            float mix = fmaxf(0.0f, fminf(100.0f, fx_params[s].mix)) / 100.0f;
            out_l = dry_l * (1.0f - mix) + wet_l * mix;
            out_r = dry_r * (1.0f - mix) + wet_r * mix;
        }

        slot_out_l[s] = out_l; slot_out_r[s] = out_r; // see this function's own slot_out_l/r comment -- published unconditionally, regardless of routing_mode

        if (routing_mode == 0) {
            *l = out_l;
            *r = out_r;
        } else {
            branch_l = out_l; branch_r = out_r;
            continue_branch = (fx_params[s].chain_to_next != 0.0f);
        }
    }

    if (routing_mode == 1) {
        if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; } // close the final still-open branch
        if (parallel_branch_count > 0) {
            *l = parallel_sum_l / (float)parallel_branch_count;
            *r = parallel_sum_r / (float)parallel_branch_count;
        }
        // parallel_branch_count == 0: *l/*r already untouched (still the original dry input),
        // which is the correct "this source owns no slots" passthrough.
    }
}
