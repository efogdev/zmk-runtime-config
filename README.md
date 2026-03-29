# ZMK Runtime Config

A parameter registry for ZMK modules that want runtime-tunable values without a reflash.

## What it does

Modules register named integer parameters with a default and a valid range. Values are read from NVS on boot (falling back to the default if nothing was saved), and written back whenever `zrc_set` is called. A small LRU cache avoids repeated NVS lookups for hot parameters.

This module doesn't do anything useful on its own — it's a support library. Other modules (ec11-ish, adaptive-rgb, etc.) use it to expose their Kconfig defaults as tunable knobs.

## Setup

```kconfig
CONFIG_ZMK_RUNTIME_CONFIG=y
CONFIG_ZMK_RUNTIME_CONFIG_SHELL=y        # optional but recommended
CONFIG_ZMK_RUNTIME_CONFIG_MAX_PARAMS=24  # increase if you have many modules
```

No DTS required.

## Shell commands

```
rtcfg list                  # dump all registered params with current value, default, and range
rtcfg get mymod/param1      # read a value
rtcfg set mymod/param1 42   # write and persist
rtcfg reset mymod/param1    # revert to compiled default, delete NVS entry
```

## Using it in a module

```c
#include <zmk_runtime_config/runtime_config.h>

// Register during init — call before any zrc_get
zrc_register("mymod/speed",   100,  0, 500);
zrc_register("mymod/enabled",   1,  0,   1);

// Read at call sites
int speed = zrc_get("mymod/speed");
```

If you want your module to compile cleanly whether or not `zmk-runtime-config` is present, guard the include and define the fallback yourself:

```c
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG)
#include <zmk_runtime_config/runtime_config.h>
#else
#define ZRC_GET(key, default_val) (default_val)
#endif

// Then at call sites:
int speed = ZRC_GET("mymod/speed", 100);
```

`ZRC_GET` is defined in the header as `zrc_get(key)` — the `default_val` argument is only used by your fallback macro when the module isn't compiled in.

Keys are arbitrary strings. The convention used across this firmware is `module/param` — short, lowercase, no spaces.
