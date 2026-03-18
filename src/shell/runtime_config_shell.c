#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zmk_runtime_config/runtime_config.h>

LOG_MODULE_DECLARE(zmk_runtime_config, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_SHELL) && IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG_SHELL)

#define shprint(_sh, _fmt, ...) \
do { \
    if ((_sh) != NULL) \
        shell_print((_sh), _fmt, ##__VA_ARGS__); \
} while (0)

struct list_ctx {
    const struct shell *sh;
    uint8_t count;
};

static void list_cb(const char *key, int32_t value, int32_t default_val,
                    int32_t min_val, int32_t max_val, void *user) {
    struct list_ctx *ctx = user;
    shprint(ctx->sh, "  %-36s %d  (default: %d, range: [%d, %d])",
            key, value, default_val, min_val, max_val);
    ctx->count++;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv) {
    struct list_ctx ctx = { .sh = sh, .count = 0 };
    zrc_foreach(list_cb, &ctx);
    if (ctx.count == 0) {
        shprint(sh, "No parameters registered.");
    }
    return 0;
}

static int cmd_get(const struct shell *sh, size_t argc, char **argv) {
    if (argc < 2) {
        shprint(sh, "Usage: rtcfg get <key>");
        return -EINVAL;
    }
    shprint(sh, "%s = %d", argv[1], (int)zrc_get(argv[1]));
    return 0;
}

static int cmd_set(const struct shell *sh, size_t argc, char **argv) {
    if (argc < 3) {
        shprint(sh, "Usage: rtcfg set <key> <value>");
        return -EINVAL;
    }

    char *endptr;
    const int32_t val = (int32_t)strtol(argv[2], &endptr, 10);
    const int rc = zrc_set(argv[1], val);

    if (rc == 0) {
        shprint(sh, "%s = %d", argv[1], (int)val);
    } else if (rc == -ENOENT) {
        shprint(sh, "Error: unknown key '%s'", argv[1]);
    } else if (rc == -EINVAL) {
        shprint(sh, "Error: value out of range");
    } else {
        shprint(sh, "Error: %d", rc);
    }

    return rc;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv) {
    if (argc < 2) {
        shprint(sh, "Usage: rtcfg reset <key>");
        return -EINVAL;
    }

    const int rc = zrc_reset(argv[1]);
    if (rc == 0) {
        shprint(sh, "%s reset to default: %d", argv[1], (int)zrc_get(argv[1]));
    } else if (rc == -ENOENT) {
        shprint(sh, "Error: unknown key '%s'", argv[1]);
    } else {
        shprint(sh, "Error: %d", rc);
    }

    return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rtcfg,
    SHELL_CMD(list,  NULL, "List all runtime config parameters", cmd_list),
    SHELL_CMD(get,   NULL, "Get parameter value",                cmd_get),
    SHELL_CMD(set,   NULL, "Set parameter value",                cmd_set),
    SHELL_CMD(reset, NULL, "Reset parameter to default",         cmd_reset),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(rtcfg, &sub_rtcfg, "Runtime configuration", NULL);

#endif /* CONFIG_SHELL && CONFIG_ZMK_RUNTIME_CONFIG_SHELL */
