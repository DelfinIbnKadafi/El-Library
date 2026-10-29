# Hashing

The `hashing` library provides four native functions for hashing strings:

| Function | Return type | Description |
| --- | --- | --- |
| `hash_djb2(str)` | `int` | djb2 hash (32-bit) |
| `hash_fnv1a(str)` | `int` | FNV-1a hash (32-bit) |
| `hash_crc32(str)` | `int` | CRC-32, compatible with zlib and PNG |
| `sha256(str)` | `str` | SHA-256 digest as 64 lowercase hexadecimal characters |

Load the library in an EL program and call the functions:

```el
library "hashing";

var int hash = hash_fnv1a("hello");
var str digest = sha256("hello");
```

The three 32-bit functions return their hash bits as an EL signed 32-bit
integer. Values whose high bit is set therefore appear negative.

Use SHA-256 when you need a cryptographic digest. djb2, FNV-1a, and CRC-32
are not cryptographic and should not be used for security-sensitive purposes
such as password storage.

## Build

The repository includes prebuilt Windows (`hashing.dll`) and Linux
(`hashing.so`) plugins. To rebuild for your current system, run `make -C src`
from this directory with GCC and Make installed. The build uses
[`src/Makefile`](src/Makefile) and writes the plugin next to `library.json`.