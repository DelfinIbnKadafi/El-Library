# dotenv

Load values from a `.env` file into an El program. The plugin reads `.env` from
the program's working directory when it loads.

```env
APP_NAME=MyApplication
DEBUG=true
PORT=8080
```

```el
library "dotenv";

var str name = env_str("APP_NAME");
var bool debug = env_bool("DEBUG");
var int port = env_int("PORT");
```

## Functions

| Function | Returns |
| --- | --- |
| `env_str(key)` | String value |
| `env_int(key)` | Integer value |
| `env_float(key)` | Floating-point value |
| `env_bool(key)` | Boolean value (`true`/`false` or `1`/`0`) |

Each function returns `None` when the key is missing. Invalid numeric or
boolean values produce an error. Blank lines and lines beginning with `#` are
ignored; surrounding whitespace and matching single or double quotes are
removed.

## Build

Run `make` in this directory with GCC and Make installed. This builds
`dotenv.dll` on Windows or `dotenv.so` on Linux. The repository also includes
prebuilt plugins.
