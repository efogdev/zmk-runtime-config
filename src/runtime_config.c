#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk_runtime_config/runtime_config.h>

LOG_MODULE_REGISTER(zmk_runtime_config, CONFIG_ZMK_LOG_LEVEL);

#define ZRC_NVS_PREFIX "rtcfg"

struct zrc_entry {
    char key[ZRC_KEY_MAX_LEN];
    int32_t value;
    int32_t default_val;
    int32_t min_val;
    int32_t max_val;
};

static struct zrc_entry entries[CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS];
static uint8_t num_entries = 0;

static struct zrc_entry *find_entry(const char *key) {
    for (uint8_t i = 0; i < num_entries; i++) {
        if (strcmp(entries[i].key, key) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

int zrc_register(const char *key, int32_t default_val, int32_t min_val, int32_t max_val) {
    if (find_entry(key) != NULL) {
        return -EALREADY;
    }

    if (num_entries >= CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS) {
        LOG_ERR("Registry full (max %d)", CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS);
        return -ENOMEM;
    }

    struct zrc_entry *e = &entries[num_entries++];
    strncpy(e->key, key, ZRC_KEY_MAX_LEN - 1);
    e->key[ZRC_KEY_MAX_LEN - 1] = '\0';
    e->value = default_val;
    e->default_val = default_val;
    e->min_val = min_val;
    e->max_val = max_val;

    LOG_DBG("Registered '%s' default=%d range=[%d,%d]", key, default_val, min_val, max_val);
    return 0;
}

int32_t zrc_get(const char *key) {
    const struct zrc_entry *e = find_entry(key);
    if (e == NULL) {
        LOG_WRN("Unknown param '%s'", key);
        return 0;
    }
    return e->value;
}

int zrc_set(const char *key, int32_t value) {
    struct zrc_entry *e = find_entry(key);
    if (e == NULL) {
        LOG_ERR("Unknown param '%s'", key);
        return -ENOENT;
    }

    if (value < e->min_val || value > e->max_val) {
        LOG_ERR("Value %d out of range [%d,%d] for '%s'", value, e->min_val, e->max_val, key);
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

int zrc_reset(const char *key) {
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

void zrc_foreach(zrc_foreach_cb_t cb, void *user) {
    for (uint8_t i = 0; i < num_entries; i++) {
        cb(entries[i].key, entries[i].value, entries[i].default_val,
           entries[i].min_val, entries[i].max_val, user);
    }
}

static int zrc_settings_load_cb(const char *name, size_t len,
                                  settings_read_cb read_cb, void *cb_arg) {
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

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zrc_settings, ZRC_NVS_PREFIX, NULL, zrc_settings_load_cb, NULL, NULL);
