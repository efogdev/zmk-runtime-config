#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_runtime_config/runtime_config.h>

LOG_MODULE_REGISTER(zmk_runtime_config, CONFIG_ZMK_LOG_LEVEL);

#define ZRC_NVS_PREFIX "rtcfg"

/*
 * Hash table sizing strategy:
 *
 * Robin Hood open addressing with a power-of-two table guarantees O(1)
 * average lookup with very low variance when the load factor stays below
 * 0.75.  For CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS in [30, 100] we pick
 * the next power of two above (max_params / 0.75), giving:
 *
 *   max_params=32  -> table=64   (load ≤ 0.50)
 *   max_params=64  -> table=128  (load ≤ 0.50)
 *   max_params=100 -> table=256  (load ≤ 0.39)
 *
 * The EMPTY sentinel is 0xFF (255).  Entry indices are stored as uint8_t,
 * so CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS must be ≤ 254.
 */
#define ZRC_HT_CAPACITY_SHIFT \
    ((CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS <= 32)  ? 6 : \
     (CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS <= 64)  ? 7 : \
     (CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS <= 128) ? 8 : 9)

#define ZRC_HT_CAPACITY  (1u << ZRC_HT_CAPACITY_SHIFT)
#define ZRC_HT_MASK      (ZRC_HT_CAPACITY - 1u)
#define ZRC_HT_EMPTY     UINT8_MAX   /* 0xFF — never a valid entry index */

BUILD_ASSERT(CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS <= 254,
             "MAX_PARAMS must be ≤ 254 (0xFF is the EMPTY sentinel)");
BUILD_ASSERT(ZRC_HT_CAPACITY > CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS,
             "Hash table must be larger than the maximum number of params");

/* ------------------------------------------------------------------ */
/* Entry storage                                                        */
/* ------------------------------------------------------------------ */

struct zrc_entry {
    char    key[ZRC_KEY_MAX_LEN];
    int32_t value;
    int32_t default_val;
    int32_t min_val;
    int32_t max_val;
};

static struct zrc_entry entries[CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS];
static uint8_t          num_entries;
static bool             ready;

/* ------------------------------------------------------------------ */
/* Robin Hood hash table                                                */
/*                                                                      */
/* ht_slots[i] holds the index into `entries[]` of the key whose       */
/* *ideal* slot is closest to i (Robin Hood invariant), or ZRC_HT_EMPTY*/
/* ------------------------------------------------------------------ */

static uint8_t ht_slots[ZRC_HT_CAPACITY];
static bool    ht_initialized;

/* FNV-1a — fast, excellent avalanche, no division */
static inline uint32_t fnv1a_32(const char *key)
{
    uint32_t h = 2166136261u;
    while (*key) {
        h ^= (uint8_t)*key++;
        h *= 16777619u;
    }
    return h;
}

/* Map hash to slot index — power-of-two table, so a single AND */
static inline uint32_t ideal_slot(uint32_t hash)
{
    return hash & ZRC_HT_MASK;
}

/* Probe distance of an entry currently sitting at `slot` */
static inline uint32_t probe_dist(uint32_t slot, uint32_t hash)
{
    return (slot - ideal_slot(hash)) & ZRC_HT_MASK;
}

static void ht_init(void)
{
    memset(ht_slots, ZRC_HT_EMPTY, sizeof(ht_slots));
    ht_initialized = true;
}

/* ------------------------------------------------------------------ */
/* Lookup — returns pointer into entries[] or NULL                     */
/* ------------------------------------------------------------------ */

static struct zrc_entry *find_entry(const char *key)
{
    const uint32_t hash = fnv1a_32(key);
    uint32_t slot = ideal_slot(hash);
    uint32_t dist = 0;

    for (;;) {
        const uint8_t idx = ht_slots[slot];

        /* Empty slot — key is definitely not in the table */
        if (idx == ZRC_HT_EMPTY) {
            return NULL;
        }

        /*
         * Robin Hood invariant: every entry sits no further from its
         * ideal slot than the current probe distance.  If the stored
         * entry's probe distance is *less* than ours, the key we are
         * looking for cannot be here or beyond — stop early.
         */
        if (probe_dist(slot, fnv1a_32(entries[idx].key)) < dist) {
            return NULL;
        }

        if (strcmp(entries[idx].key, key) == 0) {
            return &entries[idx];
        }

        slot = (slot + 1) & ZRC_HT_MASK;
        dist++;
    }
}

/* ------------------------------------------------------------------ */
/* Insertion — Robin Hood with backward-shift on eviction              */
/* ------------------------------------------------------------------ */

static void ht_insert(const char *key, uint8_t entry_idx)
{
    const uint32_t hash = fnv1a_32(key);
    uint32_t slot = ideal_slot(hash);
    uint32_t dist = 0;

    /*
     * We insert `entry_idx` and carry a "rich" entry's hash so we can
     * compute its probe distance without re-hashing on every iteration.
     */
    uint8_t  inserting_idx  = entry_idx;
    uint32_t inserting_hash = hash;

    for (;;) {
        uint8_t occupant = ht_slots[slot];

        if (occupant == ZRC_HT_EMPTY) {
            ht_slots[slot] = inserting_idx;
            return;
        }

        /*
         * Robin Hood: if the occupant is "richer" (closer to its ideal
         * slot) than us, steal its slot and continue inserting the
         * displaced entry.
         */
        uint32_t occupant_dist = probe_dist(slot, fnv1a_32(entries[occupant].key));
        if (occupant_dist < dist) {
            ht_slots[slot]  = inserting_idx;
            inserting_idx   = occupant;
            inserting_hash  = fnv1a_32(entries[occupant].key);
            dist            = occupant_dist;
        }

        slot = (slot + 1) & ZRC_HT_MASK;
        dist++;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int zrc_register(const char *key,
                 const int32_t default_val,
                 const int32_t min_val,
                 const int32_t max_val)
{
    if (unlikely(!ht_initialized)) {
        ht_init();
    }

    if (find_entry(key) != NULL) {
        return -EALREADY;
    }

    if (num_entries >= CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS) {
        LOG_ERR("Registry full (max %d)", CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS);
        return -ENOMEM;
    }

    const uint8_t idx = num_entries++;
    struct zrc_entry *e = &entries[idx];

    strncpy(e->key, key, ZRC_KEY_MAX_LEN - 1);
    e->key[ZRC_KEY_MAX_LEN - 1] = '\0';
    e->value       = default_val;
    e->default_val = default_val;
    e->min_val     = min_val;
    e->max_val     = max_val;

    ht_insert(key, idx);

    LOG_DBG("Registered '%s' default=%d range=[%d,%d]",
            key, default_val, min_val, max_val);
    return 0;
}

int32_t zrc_get(const char *key)
{
    const struct zrc_entry *e = find_entry(key);
    if (e == NULL) {
        LOG_WRN("Unknown param '%s'", key);
        return 0;
    }
    return e->value;
}

int zrc_set(const char *key, const int32_t value)
{
    struct zrc_entry *e = find_entry(key);
    if (e == NULL) {
        LOG_ERR("Unknown param '%s'", key);
        return -ENOENT;
    }

    if (value < e->min_val || value > e->max_val) {
        LOG_ERR("Value %d out of range [%d,%d] for '%s'",
                value, e->min_val, e->max_val, key);
        return -EINVAL;
    }

    e->value = value;

    char setting_key[ZRC_KEY_MAX_LEN + sizeof(ZRC_NVS_PREFIX) + 1];
    snprintf(setting_key, sizeof(setting_key), "%s/%s", ZRC_NVS_PREFIX, key);

    const int rc = settings_save_one(setting_key, &value, sizeof(value));
    if (rc != 0) {
        LOG_ERR("Failed to save '%s': %d", key, rc);
    } else {
        LOG_DBG("Saved '%s' = %d", key, value);
    }

    return rc;
}

bool zrc_ready() {
    return ready;
}

int zrc_reset(const char *key)
{
    struct zrc_entry *e = find_entry(key);
    if (e == NULL) {
        LOG_ERR("Unknown param '%s'", key);
        return -ENOENT;
    }

    e->value = e->default_val;

    char setting_key[ZRC_KEY_MAX_LEN + sizeof(ZRC_NVS_PREFIX) + 1];
    snprintf(setting_key, sizeof(setting_key), "%s/%s", ZRC_NVS_PREFIX, key);

    const int rc = settings_delete(setting_key);
    if (rc != 0 && rc != -ENOENT) {
        LOG_ERR("Failed to delete '%s': %d", key, rc);
        return rc;
    }

    LOG_DBG("Reset '%s' to default %d", key, e->default_val);
    return 0;
}

void zrc_foreach(const zrc_foreach_cb_t cb, void *user)
{
    for (uint8_t i = 0; i < num_entries; i++) {
        cb(entries[i].key, entries[i].value, entries[i].default_val,
           entries[i].min_val, entries[i].max_val, user);
    }
}

static int zrc_settings_load_cb(const char *name, const size_t len,
                                const settings_read_cb read_cb, void *cb_arg)
{
    struct zrc_entry *e = find_entry(name);
    if (e == NULL) {
        LOG_DBG("Ignoring unknown NVS param '%s'", name);
        return 0;
    }

    if (len != sizeof(int32_t)) {
        LOG_WRN("Unexpected data size for '%s': %d", name, (int)len);
        return 0;
    }

    int32_t val;
    const int rd = read_cb(cb_arg, &val, sizeof(val));
    if (rd == sizeof(int32_t)) {
        if (val >= e->min_val && val <= e->max_val) {
            e->value = val;
            LOG_DBG("Loaded '%s' = %d", name, val);
        } else {
            LOG_WRN("Loaded value %d out of range [%d,%d] for '%s', using default %d",
                    val, e->min_val, e->max_val, name, e->default_val);
        }
    } else {
        LOG_ERR("Failed to read '%s' from NVS (rd=%d)", name, rd);
    }

    ready = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zrc_settings, ZRC_NVS_PREFIX,
                                NULL, zrc_settings_load_cb, NULL, NULL);
                                