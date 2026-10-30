# mysql: MySQL and MariaDB for El

A native plugin library that lets El programs talk to a MySQL or MariaDB server. It follows the style of the Pawn MySQL plugins: connections and results are plain `int` handles, every function starts with `mysql_`, and a failed connect or query gives `NONE` and the reason is read with `mysql_error`.

```el
library "mysql";

var int db = mysql_connect("127.0.0.1", "root", "password", "shop", 3306);

if(db == NONE) =
  print format("Cannot connect: %s", mysql_last_error());
  exit;

var int res = mysql_query(db, "SELECT id, name FROM products");

while(mysql_fetch_row(res)) =
  print format("%d: %s", mysql_field_int(res, "id"), mysql_field(res, "name"));

mysql_free_result(res);
mysql_close(db);
```

Requires El Language **0.8.2 or newer** (the version that has plugins).

## Contents

- [How it works](#how-it-works)
- [Installing](#installing)
- [Building the plugin](#building-the-plugin)
- [Finding the client library](#finding-the-client-library)
- [Quick start](#quick-start)
- [Function reference](#function-reference)
- [Handles](#handles)
- [NONE, errors and failures](#none-errors-and-failures)
- [Types and limits](#types-and-limits)
- [Security: SQL injection](#security-sql-injection)
- [Coming from Pawn](#coming-from-pawn)
- [Troubleshooting](#troubleshooting)
- [Examples](#examples)
- [What was tested](#what-was-tested)

## How it works

`mysql.so` / `mysql.dll` is the plugin. It does **not** contain a MySQL client. The first time a program calls `mysql_connect`, the plugin loads the MySQL client library that is installed on the machine (`libmysqlclient` or `libmariadb`) and uses it to talk to the server.

This has three effects:

- Building the plugin needs only a C compiler, no MySQL headers or libraries.
- One plugin file works with MySQL and with MariaDB.
- `library "mysql";` never fails because the client library is missing. Only `mysql_connect` does, with a message that says what to install.

Every result set is copied into the plugin as soon as `mysql_query` returns. A result stays valid while you run other queries, can be read in any order, and no longer depends on the connection.

## Installing

With `ellib` (when the library is in a repository):

```
ellib install mysql
```

Or by hand: copy this folder to the `library` folder next to `elvm`, so that these files exist:

```
library/mysql/library.json
library/mysql/mysql.so        (Linux)
library/mysql/mysql.dll       (Windows)
```

The `src`, `examples` and `README.md` files are not needed to run it.

Then install the MySQL client library on the machine, see [Finding the client library](#finding-the-client-library).

## Building the plugin

You need a C compiler (`gcc`). Nothing else: `elplugin.h`, the plugin header, is in `src/`.

### Linux

```bash
cd src
make
```

Or without `make`:

```bash
cd src
gcc -std=c99 -O2 -fPIC -shared -I. mysql.c -o ../mysql.so -ldl
```

### Windows (MinGW-w64)

Use a 64-bit `gcc` if `elvm.exe` is 64-bit (the plugin must be built for the same CPU type as `elvm`).

```bat
cd src
gcc -std=c99 -O2 -shared -I. mysql.c -o ..\mysql.dll
```

or `mingw32-make` in the `src` folder.

### Windows (Visual Studio)

From a "x64 Native Tools Command Prompt":

```bat
cd src
cl /O2 /LD /I. mysql.c /Fe:..\mysql.dll
```

### macOS

```bash
cd src
make
```

This builds `mysql.dylib`. Add `"mysql.dylib"` to `plugins` in `library.json`.

### After building

`library.json` lists the plugin file of each system:

```json
"plugins": ["mysql.dll", "mysql.so"]
```

`ellib` and `elvm` use only the file for the system they run on, but every listed file that is for the current system must exist. Build the plugin for each system you support, and list only the ones you built.

## Finding the client library

On the first `mysql_connect`, the plugin looks for the client library in this order, and uses the first one that works:

1. The path set with `mysql_set_library("path")`, if the program called it.
2. The path in the environment variable `EL_MYSQL_LIB`.
3. The folder of the plugin itself (so you can put the library next to `mysql.dll`).
4. The system's normal places, with these names:

| System | Names tried |
|---|---|
| Windows | `libmariadb.dll`, `libmysql.dll`, `libmysqlclient.dll` |
| Linux | `libmariadb.so.3`, `libmysqlclient.so.21`, `.so.24`, `.so.20`, `.so.18`, `libmariadbclient.so.18`, `libmysqlclient.so`, `libmariadb.so` |
| macOS | `libmysqlclient.dylib`, `libmariadb.dylib`, `libmysqlclient.24.dylib`, `libmysqlclient.21.dylib`, `libmariadb.3.dylib` |

Where to get it:

| System | Package |
|---|---|
| Debian / Ubuntu | `sudo apt install libmariadb3` (or `libmysqlclient21`) |
| Fedora | `sudo dnf install mariadb-connector-c` |
| Arch | `sudo pacman -S mariadb-libs` |
| macOS (Homebrew) | `brew install mysql-client` or `brew install mariadb-connector-c` |
| Windows | `libmariadb.dll` from MariaDB Connector/C, or `libmysql.dll` from MySQL. Put it next to `mysql.dll`, or in a folder on `PATH`. |

The client library must be built for the same CPU type as `elvm` (a 64-bit `elvm` needs a 64-bit client library).

If the library is somewhere else, point to it:

```bash
export EL_MYSQL_LIB=/opt/mysql/lib/libmysqlclient.so
```

```el
mysql_set_library("C:\\mysql\\lib\\libmysql.dll");
```

`mysql_client_library()` tells which library was loaded.

## Quick start

```el
library "mysql";

// 1. Connect. NONE means it failed.
var int db = mysql_connect("127.0.0.1", "root", "password", "shop", 3306);

if(db == NONE) =
  print format("Cannot connect: %s", mysql_last_error());
  exit;

// 2. A statement that changes data: mysql_exec gives the number of rows changed
var int changed = mysql_exec(db, "UPDATE products SET price = price * 2");
print format("%d rows changed", changed);

// 3. A query that gives rows: mysql_query gives a result handle
var int res = mysql_query(db, "SELECT id, name, price FROM products");

if(res == NONE) =
  print format("Query failed: %s", mysql_error(db));
  mysql_close(db);
  exit;

// 4. Walk the rows and read the columns by name
while(mysql_fetch_row(res)) =
  print format("%d | %s | %f", mysql_field_int(res, "id"), mysql_field(res, "name"), mysql_field_float(res, "price"));

// 5. Clean up
mysql_free_result(res);
mysql_close(db);
```

## Function reference

Argument types are `int`, `float`, `bool` and `str`. `db` is a connection handle and `res` a result handle (both `int`).

### Connections

| Function | Gives | Description |
|---|---|---|
| `mysql_connect(host, user, password, database, port)` | `int` | Connects. Gives a connection handle, or `NONE` if it failed (the reason is in `mysql_last_error`). An empty `host`, `user`, `password` or `database` means "not given", port `0` means 3306. Sets the character set to `utf8mb4` (or `utf8` on old servers). |
| `mysql_close(db)` | nothing | Closes the connection. The handle must not be used again. |
| `mysql_ping(db)` | `bool` | `true` if the connection is alive. |
| `mysql_set_charset(db, charset)` | `bool` | Changes the character set of the connection, for example `"latin1"`. `true` on success. |
| `mysql_set_timeout(seconds)` | nothing | Connect timeout for the connections made after this call. 1 to 3600, default 10. |
| `mysql_error(db)` | `str` | Text of the last error on this connection (empty if none). |
| `mysql_errno(db)` | `int` | Number of the last error on this connection (0 if none). Like `1045` (access denied), `1064` (syntax error), `1062` (duplicate key). |
| `mysql_last_error()` | `str` | Text of the last error that has no connection, like a failed `mysql_connect`. |
| `mysql_last_errno()` | `int` | Number of that error. |
| `mysql_set_library(path)` | `bool` | Loads the client library from `path` now. `true` if it worked. Call it before `mysql_connect`. |
| `mysql_client_library()` | `str` | The client library in use, or `NONE` if none is loaded yet. |

### Running SQL

| Function | Gives | Description |
|---|---|---|
| `mysql_query(db, sql)` | `int` | Runs `sql` and gives a **result handle**, or `NONE` if it failed (see `mysql_error(db)`). A statement that gives no rows (`INSERT`, `UPDATE`, ...) gives an empty result. Free the handle with `mysql_free_result`. |
| `mysql_exec(db, sql)` | `int` | Runs `sql` and gives the number of rows it changed, or `NONE` if it failed. Nothing to free. Use it for statements where you do not need rows back. |
| `mysql_affected_rows(db)` | `int` | Rows changed by the last statement. |
| `mysql_insert_id(db)` | `int` | The `AUTO_INCREMENT` id created by the last `INSERT` (0 if none). |
| `mysql_escape(db, text)` | `str` | `text` made safe to put between single quotes in SQL. See [Security](#security-sql-injection). |

### Reading a result, row by row (cursor)

| Function | Gives | Description |
|---|---|---|
| `mysql_fetch_row(res)` | `bool` | Moves to the next row. `false` when there are no more rows. A new result starts before the first row. |
| `mysql_rewind(res)` | nothing | Goes back to before the first row. |
| `mysql_field(res, name)` | `str` | Value of column `name` in the current row. |
| `mysql_field_int(res, name)` | `int` | Same, as an `int`. |
| `mysql_field_float(res, name)` | `float` | Same, as a `float`. |

Column names are matched exactly first, then ignoring upper and lower case. Calling these before the first `mysql_fetch_row`, after it gave `false`, or with a name the result does not have, stops the program with an error that says why.

### Reading a result, by row and column number

Row and column numbers start at 0.

| Function | Gives | Description |
|---|---|---|
| `mysql_num_rows(res)` | `int` | Number of rows. |
| `mysql_num_fields(res)` | `int` | Number of columns. |
| `mysql_field_name(res, column)` | `str` | Name of a column. |
| `mysql_field_index(res, name)` | `int` | Number of the column called `name`, or `NONE` if there is none. |
| `mysql_get(res, row, column)` | `str` | Value at that place. |
| `mysql_get_int(res, row, column)` | `int` | Same, as an `int`. |
| `mysql_get_float(res, row, column)` | `float` | Same, as a `float`. |
| `mysql_free_result(res)` | nothing | Frees the result. The handle must not be used again. |

An SQL `NULL` is read as `NONE` by all of the value functions above.

## Handles

A handle is a small `int`. The plugin keeps the real connection or result, and finds it from the number.

- `NONE` means a connect or query failed. Passing `NONE` to a function stops the program with `the connection handle is NONE (did mysql_connect fail?)`, so a missing check is found right away instead of causing wrong results.
- A handle that was closed or freed, or never existed, stops the program with `invalid connection handle` / `invalid result handle`.
- The number of a closed handle is reused by the next one, so do not keep a handle after closing it.
- Handles left open when the program ends are closed by the plugin.
- Free results you no longer need: a result holds all of its rows in memory.

## NONE, errors and failures

| Situation | What happens |
|---|---|
| `mysql_connect` fails (wrong password, no server, unknown database) | gives `NONE`. Read `mysql_last_error()` and `mysql_last_errno()`. |
| `mysql_query` / `mysql_exec` fails (bad SQL, no such table) | gives `NONE`. Read `mysql_error(db)` and `mysql_errno(db)`. |
| The client library is not installed | `mysql_connect` stops the program with a message that lists what it tried. |
| A handle is `NONE`, closed or invalid | stops the program with an error. |
| Row or column out of range, wrong column name | stops the program with an error. |
| A value is not a number or does not fit | stops the program with an error (see below). |
| An SQL `NULL` | read as `NONE`. |

To check for `NONE`, put the value in a variable first. `NONE` can be compared against a variable, not against a function call:

```el
var int age = mysql_field_int(res, "age");

if(age == NONE) =
  print "age is NULL";
```

(`print` of a function call that gives `NONE` shows `0`; printing the variable shows `NONE`.)

## Types and limits

- **An El `int` is a 32 bit number** (up to 2147483647). A `BIGINT` or `UNSIGNED INT` value above that cannot be read with `mysql_get_int` / `mysql_field_int` and stops the program with an error that tells you to read it with `mysql_get` / `mysql_field` as text instead. The same goes for `mysql_insert_id`, `mysql_affected_rows` and `mysql_num_rows` above 2147483647.
- `mysql_get_int` and `mysql_get_float` need the value to be a whole number or a number. `12.50` read with `mysql_get_int` is an error, read it with `mysql_get_float`.
- Dates, times, `DECIMAL` and everything else are read as text with `mysql_get` / `mysql_field`.
- Text with a zero byte inside is cut at that byte (El strings are C strings). `BLOB` data is not supported.
- A result is read completely into memory. Use `LIMIT` for queries that can give a very large number of rows.
- Variables that hold SQL must be big enough: the default `str` holds 255 characters. Write `var str[2000] sql = ...` for longer text.
- One statement per call. Several statements in one string (`a; b`) are not allowed.
- Prepared statements and transactions have no special functions: run `START TRANSACTION`, `COMMIT` and `ROLLBACK` with `mysql_exec`.
- Calls wait until the server answers. There is no background mode, and no callbacks into El.

## Security: SQL injection

If SQL is made from text a user typed, the user can change what the SQL does. This is called SQL injection. Always pass such text through `mysql_escape` and put the result between **single quotes**:

```el
var str[100] name = input "Name: ";
var str[220] safe = mysql_escape(db, name);

var int res = mysql_query(db, format("SELECT * FROM users WHERE name = '%s'", safe));
```

Rules:

- Always use single quotes around the escaped text, and `%s` for it. Escaping protects text inside quotes only.
- For a number from the user, read it into an `int` variable and put it in with `%d`. That cannot be abused.
- Make the escaped variable twice as long as the input plus one.
- Never build a table or column name from user input. Check it against a list of allowed names instead.
- Use a database user that has only the rights the program needs, never `root`, for real programs.
- Do not write passwords in `.ell` files you share. Read them with `input` or keep the file private.

## Coming from Pawn

The plugin keeps the idea of the Pawn MySQL plugins (a handle for a connection, a result you read column by column), with names that fit El.

| Pawn plugin | This plugin |
|---|---|
| `mysql_connect(host, user, db, password)` | `mysql_connect(host, user, password, database, port)`. Note the order: password comes before the database. |
| gives `0` on failure | gives `NONE` on failure |
| `mysql_query(handle, query)` | `mysql_query(db, sql)` gives a result handle. `mysql_exec` is the same without a result to free. |
| `mysql_store_result()` | not needed, the result is stored by `mysql_query` |
| `mysql_num_rows()` | `mysql_num_rows(res)` |
| `mysql_fetch_row(...)`, `mysql_fetch_field_row(dest, "field")` | `mysql_fetch_row(res)`, `mysql_field(res, "field")` |
| `cache_get_value_name(row, "field", dest)` | `mysql_get(res, row, mysql_field_index(res, "field"))` |
| `mysql_escape_string(...)` | `mysql_escape(db, text)` |
| `mysql_free_result()` | `mysql_free_result(res)` |
| `mysql_errno()`, `mysql_error()` | `mysql_errno(db)`, `mysql_error(db)` |
| `mysql_close()` | `mysql_close(db)` |
| output goes into a destination array | values are returned (El functions return strings) |

## Troubleshooting

**`cannot load the MySQL client library`**
The client library is not installed or not found. See [Finding the client library](#finding-the-client-library). Run `mysql_client_library()` after a successful connect to see which one was used.

**`Cannot load plugin ... (...)` when the program starts**
The system refused to load `mysql.dll` / `mysql.so`. Common causes: the plugin was built for another CPU type than `elvm` (32 bit and 64 bit do not mix), or the file is not there.

**`Library "mysql" has no plugin for this system`**
`library.json` has no file for this system. Build the plugin (see above) and list it in `plugins`.

**Connect gives `NONE` and `mysql_last_error()` says `Access denied`**
Wrong user or password, or the user may not connect from this host. `Unknown database` means the database name is wrong. `Can't connect to MySQL server` means the server is not running or the host and port are wrong. With MySQL 8 and an old client library you can get `Authentication plugin 'caching_sha2_password' cannot be loaded`: use a newer client library, or give the user the `mysql_native_password` login.

**`mysql_get_int: the value ... does not fit in an EL int`**
The number is bigger than 2147483647. Read it as text with `mysql_get`.

**Text with accents or emoji looks wrong**
The connection uses `utf8mb4` by default. The table and columns should use `utf8mb4` too. Use `mysql_set_charset` if you need another one.

**`the connection handle is NONE`**
A `mysql_connect` earlier gave `NONE` and the check for it is missing. Check `if(db == NONE)` right after connecting.

## Examples

In the `examples` folder:

| File | Shows |
|---|---|
| `basic.ell` | connect, create a table, insert, read with `mysql_fetch_row`, update, delete |
| `errors.ell` | a failed connect, a failed query, `mysql_ping`, reading error numbers and texts |
| `safe-input.ell` | taking text from the user and putting it in SQL safely with `mysql_escape` |
| `random-access.ell` | reading by row and column number, column names, `NULL` |

Change the host, user, password and database at the top to match your server. The examples create their own `el_demo_*` tables and remove them again.

Run one with:

```
elvm examples/basic.ell
```

## What was tested

Be aware of how this was checked:

- The plugin was built and run on **Linux x86-64**.
- No MySQL or MariaDB server was available while writing it. The tests ran against a **stand-in client library** that implements the same C API on top of SQLite, covering connect (success, wrong password, unknown host and database), queries, results with several rows, `NULL`, decimals, big numbers, empty results, 5000 rows, unicode text, escaping, handle checks, all error messages, the order the client library is searched, and leftover connections at exit. Memory was checked with AddressSanitizer (no errors and no leaks in the plugin).
- That proves the plugin's own logic. It does **not** prove behaviour against a real server: character set handling, authentication methods, exact error numbers, and the behaviour of `mysql_escape` come from the real client library. Run `examples/basic.ell` against your server first.
- The **Windows** code path (`LoadLibrary`, finding the plugin folder) was written but **not compiled or run**.
- The functions of the client library are declared inside `mysql.c` from the documented MySQL C API. They are the same in MySQL 5.7, MySQL 8 and MariaDB Connector/C, with the 64-bit calling convention. 32-bit Windows was not considered.

## Files

```
mysql/
  library.json          the library description (lists the plugin files)
  README.md             this file
  mysql.so              the plugin built for Linux x86-64
  src/mysql.c           the plugin source
  src/elplugin.h        the plugin API header (a copy of El-Language/src/elplugin.h)
  src/Makefile          builds the plugin for the current system
  examples/*.ell        example programs
```
