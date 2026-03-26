#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ZRC_KEY_MAX_LEN 32

/**
 * Register a runtime-configurable integer parameter.
 *
 * Idempotent: safe to call multiple times with the same key.
 * Must be called before settings_load() completes to ensure NVS values are applied.
 *
 * @param key        Unique parameter name (e.g. "p2sm/ema_alpha"). Max ZRC_KEY_MAX_LEN chars.
 * @param default_val Compile-time default value.
 * @param min_val    Minimum allowed value (inclusive).
 * @param max_val    Maximum allowed value (inclusive).
 * @return 0 on success, -ENOMEM if registry is full, -EALREADY if already registered.
 */
int zrc_register(const char *key, int32_t default_val, int32_t min_val, int32_t max_val);

/**
 * Get the current value for a registered parameter.
 * Returns default_val if the key is not registered.
 */
int32_t zrc_get(const char *key);

/**
 * Set a parameter value and persist it to NVS.
 * @return 0 on success, -ENOENT if key unknown, -EINVAL if value out of range.
 */
int zrc_set(const char *key, int32_t value);

/**
 * Reset a parameter to its registered default and remove the NVS entry.
 * @return 0 on success, -ENOENT if key unknown.
 */
int zrc_reset(const char *key);

typedef void (*zrc_foreach_cb_t)(const char *key, int32_t value, int32_t default_val,
                                  int32_t min_val, int32_t max_val, void *user);

/** Iterate over all registered parameters. */
void zrc_foreach(zrc_foreach_cb_t cb, void *user);

/**
 * Get a registered value, falling back to default_val if the key is unknown.
 * Use this macro at call sites so they need no knowledge of whether the module
 * is present: include this header under IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG),
 * otherwise define ZRC_GET(key, default_val) as (default_val).
 */
#define ZRC_GET(key, default_val) zrc_get(key)
