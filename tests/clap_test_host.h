// clap_test_host.h -- a minimal in-process CLAP host for the plugin tests: loads a .clap with
// dlopen, creates its first plugin, and provides timer support (the native GUIs repaint and poll
// the file browser from a host timer).
#ifndef HDS_CLAP_TEST_HOST_H
#define HDS_CLAP_TEST_HOST_H

#include <clap/clap.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static clap_id g_th_timer_id = 0;
static int g_th_has_timer = 0;
static inline bool th_register_timer(const clap_host_t *h, uint32_t ms, clap_id *id) { (void)h; (void)ms; *id = g_th_timer_id = 1; g_th_has_timer = 1; return true; }
static inline bool th_unregister_timer(const clap_host_t *h, clap_id id) { (void)h; (void)id; g_th_has_timer = 0; return true; }
static const clap_host_timer_support_t k_th_timers = { th_register_timer, th_unregister_timer };
static inline const void *th_get_extension(const clap_host_t *h, const char *id) { (void)h; return strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0 ? &k_th_timers : NULL; }
static inline void th_noop(const clap_host_t *h) { (void)h; }
static const clap_host_t k_th_host = { CLAP_VERSION, NULL, "hds-tests", "hds", "", "1", th_get_extension, th_noop, th_noop, th_noop };

typedef struct {
    void *so;
    const clap_plugin_entry_t *entry;
    const clap_plugin_t *plugin;
} TestPlugin;

static inline int test_plugin_load(TestPlugin *tp, const char *path) {
    memset(tp, 0, sizeof *tp);
    tp->so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!tp->so) { printf("FAIL load %s: %s\n", path, dlerror()); return -1; }
    tp->entry = (const clap_plugin_entry_t *)dlsym(tp->so, "clap_entry");
    if (!tp->entry || !tp->entry->init(path)) { printf("FAIL load %s: no clap_entry\n", path); return -1; }
    const clap_plugin_factory_t *f = (const clap_plugin_factory_t *)tp->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    tp->plugin = f->create_plugin(f, &k_th_host, f->get_plugin_descriptor(f, 0)->id);
    if (!tp->plugin || !tp->plugin->init(tp->plugin)) { printf("FAIL load %s: create/init failed\n", path); return -1; }
    return 0;
}

static inline void test_plugin_unload(TestPlugin *tp) {
    if (tp->plugin) tp->plugin->destroy(tp->plugin);
    if (tp->entry) tp->entry->deinit();
    memset(tp, 0, sizeof *tp);
}

// Runs the plugin's timer `n` times, ~20 ms apart, the way a host's GUI timer would.
static inline void test_plugin_pump_timer(const TestPlugin *tp, int n) {
    const clap_plugin_timer_support_t *t = (const clap_plugin_timer_support_t *)tp->plugin->get_extension(tp->plugin, CLAP_EXT_TIMER_SUPPORT);
    for (int i = 0; i < n; i++) {
        if (g_th_has_timer && t) t->on_timer(tp->plugin, g_th_timer_id);
        struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
    }
}

#endif // HDS_CLAP_TEST_HOST_H
