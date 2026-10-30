/*
 * mysql - MySQL / MariaDB plugin for EL.
 *
 * The MySQL client library is NOT linked at build time. The plugin loads it
 * the first time a program connects (dlopen / LoadLibrary) and declares the
 * few C API functions it needs itself, so building needs no mysql.h, and the
 * same plugin works with libmysqlclient (MySQL) and libmariadb (MariaDB).
 *
 * Style: like the Pawn MySQL plugins. Connections and results are plain int
 * handles, functions have a mysql_ prefix, a failed connect or query gives
 * NONE and the reason is read with mysql_error / mysql_last_error.
 *
 * Every result set is copied into plugin memory right away, so a result stays
 * valid after more queries and can be read in any order.
 *
 * See README.md for the full list of functions.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include "elplugin.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static const ELApi *api;

/* ------------------------------------------------------------------ */
/* Client library functions (declared here, loaded at run time)        */
/* ------------------------------------------------------------------ */

typedef void *MyConn;
typedef void *MyRes;

typedef MyConn (*fn_mysql_init)(MyConn);
typedef MyConn (*fn_mysql_real_connect)(MyConn, const char *, const char *,
    const char *, const char *, unsigned int, const char *, unsigned long);
typedef int (*fn_mysql_options)(MyConn, int, const void *);
typedef int (*fn_mysql_real_query)(MyConn, const char *, unsigned long);
typedef MyRes (*fn_mysql_store_result)(MyConn);
typedef unsigned int (*fn_mysql_field_count)(MyConn);
typedef uint64_t (*fn_mysql_num_rows)(MyRes);
typedef unsigned int (*fn_mysql_num_fields)(MyRes);
typedef char **(*fn_mysql_fetch_row)(MyRes);
/* The result is a MYSQL_FIELD*. Its first member is 'char *name' in both
   MySQL and MariaDB, which is the only thing read from it. */
typedef void *(*fn_mysql_fetch_field_direct)(MyRes, unsigned int);
typedef void (*fn_mysql_free_result)(MyRes);
typedef uint64_t (*fn_mysql_affected_rows)(MyConn);
typedef uint64_t (*fn_mysql_insert_id)(MyConn);
typedef const char *(*fn_mysql_error)(MyConn);
typedef unsigned int (*fn_mysql_errno)(MyConn);
typedef unsigned long (*fn_mysql_real_escape_string)(MyConn, char *,
    const char *, unsigned long);
typedef int (*fn_mysql_ping)(MyConn);
typedef int (*fn_mysql_set_character_set)(MyConn, const char *);
typedef void (*fn_mysql_close)(MyConn);

#define CLIENT_SYMBOLS(X) \
    X(mysql_init) X(mysql_real_connect) X(mysql_options) \
    X(mysql_real_query) X(mysql_store_result) X(mysql_field_count) \
    X(mysql_num_rows) X(mysql_num_fields) X(mysql_fetch_row) \
    X(mysql_fetch_field_direct) X(mysql_free_result) \
    X(mysql_affected_rows) X(mysql_insert_id) X(mysql_error) \
    X(mysql_errno) X(mysql_real_escape_string) X(mysql_ping) \
    X(mysql_set_character_set) X(mysql_close)

static struct {
#define DECLARE_SYMBOL(name) fn_##name name;
    CLIENT_SYMBOLS(DECLARE_SYMBOL)
#undef DECLARE_SYMBOL
} my;

/* MYSQL_OPT_CONNECT_TIMEOUT is the first value of enum mysql_option in both
   MySQL and MariaDB */
#define MY_OPT_CONNECT_TIMEOUT 0

#ifdef _WIN32
typedef HMODULE LibHandle;
#else
typedef void *LibHandle;
#endif

static LibHandle client = NULL;
static char client_name[1024] = "";
static char forced_path[1024] = "";
static unsigned int connect_timeout = 10;

/* Last error that is not tied to one connection (a failed connect, ...) */
static char last_error[512] = "";
static unsigned int last_errno = 0;

static void set_last_error(unsigned int number, const char *text)
{
    last_errno = number;
    snprintf(last_error, sizeof(last_error), "%s", text ? text : "");
}

static LibHandle open_library(const char *path)
{
#ifdef _WIN32
    if (strchr(path, '\\') || strchr(path, '/') || strchr(path, ':')) {
        char fixed[1024];
        size_t i;

        snprintf(fixed, sizeof(fixed), "%s", path);

        for (i = 0; fixed[i]; i++) {
            if (fixed[i] == '/')
                fixed[i] = '\\';
        }

        /* Also look for the DLLs the client library needs in its folder */
        return LoadLibraryExA(fixed, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *find_symbol(LibHandle lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress(lib, name);
#else
    return dlsym(lib, name);
#endif
}

static void close_library(LibHandle lib)
{
#ifdef _WIN32
    FreeLibrary(lib);
#else
    dlclose(lib);
#endif
}

/* Folder this plugin file is in, so the client library can sit next to it */
static void plugin_directory(char *out, size_t capacity)
{
    out[0] = '\0';

#ifdef _WIN32
    HMODULE module = NULL;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&plugin_directory, &module) &&
        GetModuleFileNameA(module, out, (DWORD)capacity) > 0) {
        char *slash = strrchr(out, '\\');

        if (slash)
            *slash = '\0';
        else
            out[0] = '\0';
    }
#else
    Dl_info info;

    if (dladdr((const void *)&plugin_directory, &info) && info.dli_fname) {
        char *slash;

        snprintf(out, capacity, "%s", info.dli_fname);
        slash = strrchr(out, '/');

        if (slash)
            *slash = '\0';
        else
            out[0] = '\0';
    }
#endif
}

/* Names the client library usually has */
static const char *const candidates[] = {
#ifdef _WIN32
    "libmariadb.dll", "libmysql.dll", "libmysqlclient.dll",
#elif defined(__APPLE__)
    "libmysqlclient.dylib", "libmariadb.dylib", "libmysqlclient.24.dylib",
    "libmysqlclient.21.dylib", "libmariadb.3.dylib",
#else
    "libmariadb.so.3", "libmysqlclient.so.21", "libmysqlclient.so.24",
    "libmysqlclient.so.20", "libmysqlclient.so.18", "libmariadbclient.so.18",
    "libmysqlclient.so", "libmariadb.so",
#endif
    NULL
};

/* Take all the functions out of an opened client library */
static int load_symbols(LibHandle lib, char *missing, size_t capacity)
{
#define LOAD_SYMBOL(name) { \
        void *symbol = find_symbol(lib, #name); \
        if (!symbol) { \
            snprintf(missing, capacity, "%s", #name); \
            return 0; \
        } \
        memcpy(&my.name, &symbol, sizeof(symbol)); \
    }
    CLIENT_SYMBOLS(LOAD_SYMBOL)
#undef LOAD_SYMBOL
    return 1;
}

/* Try one path or name. Returns 1 when it is a usable client library.
   'tried' collects what went wrong: a file that is not a MySQL client is
   always noted, a file that is not there only if 'note_missing' (the guesses
   for common names are not, there are too many of them). */
static int try_library(const char *path, char *tried, size_t tried_size,
                       int note_missing)
{
    LibHandle lib = open_library(path);
    char missing[64];

    if (!lib) {
        size_t used = strlen(tried);

        if (note_missing && used + strlen(path) + 3 < tried_size)
            snprintf(tried + used, tried_size - used, "%s%s",
                     used ? ", " : "", path);

        return 0;
    }

    if (!load_symbols(lib, missing, sizeof(missing))) {
        size_t used = strlen(tried);

        if (used + strlen(path) + strlen(missing) + 24 < tried_size)
            snprintf(tried + used, tried_size - used,
                     "%s%s (no %s)", used ? ", " : "", path, missing);

        close_library(lib);
        return 0;
    }

    client = lib;
    snprintf(client_name, sizeof(client_name), "%s", path);
    return 1;
}

/* Load the client library if it is not loaded yet. Returns 1 when it is
   ready, or 0 with the reason in 'error'. */
static int ensure_client(char *error, size_t error_size)
{
    char tried[768];
    char directory[512];
    const char *environment;
    int i;

    if (client)
        return 1;

    tried[0] = '\0';

    if (forced_path[0] && try_library(forced_path, tried, sizeof(tried), 1))
        return 1;

    environment = getenv("EL_MYSQL_LIB");

    if (environment && environment[0] &&
        try_library(environment, tried, sizeof(tried), 1))
        return 1;

    plugin_directory(directory, sizeof(directory));

    for (i = 0; candidates[i]; i++) {
        if (directory[0]) {
            char full[1024];

#ifdef _WIN32
            snprintf(full, sizeof(full), "%s\\%s", directory, candidates[i]);
#else
            snprintf(full, sizeof(full), "%s/%s", directory, candidates[i]);
#endif
            if (try_library(full, tried, sizeof(tried), 0))
                return 1;
        }
    }

    for (i = 0; candidates[i]; i++) {
        if (try_library(candidates[i], tried, sizeof(tried), 0))
            return 1;
    }

    {
        char names[320];
        size_t used = 0;

        names[0] = '\0';

        for (i = 0; candidates[i]; i++) {
            size_t length = strlen(candidates[i]);

            if (used + length + 3 >= sizeof(names))
                break;

            used += (size_t)snprintf(names + used, sizeof(names) - used,
                                     "%s%s", i ? ", " : "", candidates[i]);
        }

        snprintf(error, error_size,
            "cannot load the MySQL client library. %s%s%sLooked in EL_MYSQL_LIB, "
            "the plugin folder and the system for: %s. Install libmysqlclient "
            "or libmariadb, or set EL_MYSQL_LIB to its full path",
            tried[0] ? "Problem with: " : "", tried[0] ? tried : "",
            tried[0] ? ". " : "", names);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    MyConn handle;
} Connection;

typedef struct {
    int rows;
    int fields;
    char **names;
    char **cells;   /* rows * fields values, NULL for an SQL NULL */
    int cursor;     /* row mysql_fetch_row is on, -1 before the first */
} Result;

static Connection **connections = NULL;
static int connection_capacity = 0;

static Result **results = NULL;
static int result_capacity = 0;

/* Handles are slot number + 1, and a freed slot is used again */
static int add_connection(Connection *connection)
{
    int i;

    for (i = 0; i < connection_capacity; i++) {
        if (!connections[i]) {
            connections[i] = connection;
            return i + 1;
        }
    }

    {
        int new_capacity = connection_capacity ? connection_capacity * 2 : 8;
        Connection **grown = realloc(connections,
            (size_t)new_capacity * sizeof(Connection *));

        if (!grown)
            return 0;

        for (i = connection_capacity; i < new_capacity; i++)
            grown[i] = NULL;

        connections = grown;
        i = connection_capacity;
        connection_capacity = new_capacity;
        connections[i] = connection;
        return i + 1;
    }
}

static int add_result(Result *result)
{
    int i;

    for (i = 0; i < result_capacity; i++) {
        if (!results[i]) {
            results[i] = result;
            return i + 1;
        }
    }

    {
        int new_capacity = result_capacity ? result_capacity * 2 : 8;
        Result **grown = realloc(results,
            (size_t)new_capacity * sizeof(Result *));

        if (!grown)
            return 0;

        for (i = result_capacity; i < new_capacity; i++)
            grown[i] = NULL;

        results = grown;
        i = result_capacity;
        result_capacity = new_capacity;
        results[i] = result;
        return i + 1;
    }
}

static void free_result_data(Result *result)
{
    int i;

    if (!result)
        return;

    if (result->names) {
        for (i = 0; i < result->fields; i++)
            free(result->names[i]);
    }

    if (result->cells) {
        long total = (long)result->rows * result->fields;

        for (i = 0; i < total; i++)
            free(result->cells[i]);
    }

    free(result->names);
    free(result->cells);
    free(result);
}

static char *copy_text(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1);

    if (copy)
        memcpy(copy, text, length + 1);

    return copy;
}

/* Connection argument 'index' of a native, or NULL after reporting why not */
static Connection *get_connection(ELCall *call, const char *function, int index)
{
    long long handle;

    if (api->IsNone(call, index)) {
        api->Error(call, "%s: the connection handle is NONE (did mysql_connect fail?)", function);
        return NULL;
    }

    handle = api->GetInt(call, index);

    if (handle < 1 || handle > connection_capacity || !connections[handle - 1]) {
        api->Error(call, "%s: invalid connection handle %lld", function, handle);
        return NULL;
    }

    return connections[handle - 1];
}

static Result *get_result(ELCall *call, const char *function, int index)
{
    long long handle;

    if (api->IsNone(call, index)) {
        api->Error(call, "%s: the result handle is NONE (did mysql_query fail?)", function);
        return NULL;
    }

    handle = api->GetInt(call, index);

    if (handle < 1 || handle > result_capacity || !results[handle - 1]) {
        api->Error(call, "%s: invalid result handle %lld", function, handle);
        return NULL;
    }

    return results[handle - 1];
}

/* Give a count from the server as an EL int (32 bit) */
static void return_count(ELCall *call, const char *function, uint64_t value)
{
    if (value > (uint64_t)INT_MAX) {
        api->Error(call, "%s: the value %llu does not fit in an EL int (32 bit)",
                   function, (unsigned long long)value);
        return;
    }

    api->ReturnInt(call, (long long)value);
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

/* mysql_connect(host, user, password, database, port):int */
static void native_connect(ELCall *call)
{
    const char *host = api->GetStr(call, 0);
    const char *user = api->GetStr(call, 1);
    const char *password = api->GetStr(call, 2);
    const char *database = api->GetStr(call, 3);
    long long port = api->GetInt(call, 4);
    char error[1024];
    MyConn handle;
    Connection *connection;
    int id;

    if (port < 0 || port > 65535) {
        api->Error(call, "mysql_connect: invalid port %lld", port);
        return;
    }

    if (!ensure_client(error, sizeof(error))) {
        api->Error(call, "mysql_connect: %s", error);
        return;
    }

    handle = my.mysql_init(NULL);

    if (!handle) {
        set_last_error(0, "mysql_init failed (out of memory)");
        api->ReturnNone(call);
        return;
    }

    my.mysql_options(handle, MY_OPT_CONNECT_TIMEOUT, &connect_timeout);

    if (!my.mysql_real_connect(handle, host[0] ? host : NULL,
                               user[0] ? user : NULL,
                               password[0] ? password : NULL,
                               database[0] ? database : NULL,
                               (unsigned int)port, NULL, 0)) {
        set_last_error(my.mysql_errno(handle), my.mysql_error(handle));
        my.mysql_close(handle);
        api->ReturnNone(call);
        return;
    }

    /* Full Unicode if the server can do it, else the older utf8 */
    if (my.mysql_set_character_set(handle, "utf8mb4") != 0)
        my.mysql_set_character_set(handle, "utf8");

    connection = malloc(sizeof(Connection));
    id = connection ? add_connection(connection) : 0;

    if (!id) {
        free(connection);
        my.mysql_close(handle);
        set_last_error(0, "out of memory");
        api->ReturnNone(call);
        return;
    }

    connection->handle = handle;
    set_last_error(0, "");
    api->ReturnInt(call, id);
}

/* mysql_close(connection) */
static void native_close(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_close", 0);

    if (!connection)
        return;

    my.mysql_close(connection->handle);
    connections[api->GetInt(call, 0) - 1] = NULL;
    free(connection);
}

/* mysql_ping(connection):bool */
static void native_ping(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_ping", 0);

    if (connection)
        api->ReturnBool(call, my.mysql_ping(connection->handle) == 0);
}

/* mysql_set_charset(connection, charset):bool */
static void native_set_charset(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_set_charset", 0);

    if (connection)
        api->ReturnBool(call,
            my.mysql_set_character_set(connection->handle, api->GetStr(call, 1)) == 0);
}

/* mysql_error(connection):str */
static void native_error(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_error", 0);

    if (connection)
        api->ReturnStr(call, my.mysql_error(connection->handle));
}

/* mysql_errno(connection):int */
static void native_errno(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_errno", 0);

    if (connection)
        api->ReturnInt(call, (long long)my.mysql_errno(connection->handle));
}

/* mysql_last_error():str */
static void native_last_error(ELCall *call)
{
    api->ReturnStr(call, last_error);
}

/* mysql_last_errno():int */
static void native_last_errno(ELCall *call)
{
    api->ReturnInt(call, (long long)last_errno);
}

/* mysql_set_timeout(seconds) */
static void native_set_timeout(ELCall *call)
{
    long long seconds = api->GetInt(call, 0);

    if (seconds < 1 || seconds > 3600) {
        api->Error(call, "mysql_set_timeout: seconds must be 1 to 3600, got %lld", seconds);
        return;
    }

    connect_timeout = (unsigned int)seconds;
}

/* mysql_set_library(path):bool */
static void native_set_library(ELCall *call)
{
    const char *path = api->GetStr(call, 0);
    char tried[768];

    if (client) {
        api->ReturnBool(call, 1);
        return;
    }

    snprintf(forced_path, sizeof(forced_path), "%s", path);
    tried[0] = '\0';

    if (try_library(path, tried, sizeof(tried), 1)) {
        api->ReturnBool(call, 1);
        return;
    }

    set_last_error(0, tried);
    api->ReturnBool(call, 0);
}

/* mysql_client_library():str, the library in use, or NONE if not loaded yet */
static void native_client_library(ELCall *call)
{
    if (client)
        api->ReturnStr(call, client_name);
    else
        api->ReturnNone(call);
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* Run a query. Returns 1 on success. On failure the reason is already in
   the connection (mysql_error). */
static int run_query(Connection *connection, const char *sql)
{
    return my.mysql_real_query(connection->handle, sql, (unsigned long)strlen(sql)) == 0;
}

/* Copy a stored result set into plugin memory. NULL if out of memory. */
static Result *copy_result(MyRes raw)
{
    Result *result = calloc(1, sizeof(Result));
    uint64_t rows = my.mysql_num_rows(raw);
    unsigned int fields = my.mysql_num_fields(raw);
    int r, f;

    if (!result)
        return NULL;

    if (rows > (uint64_t)INT_MAX || (uint64_t)fields > (uint64_t)INT_MAX ||
        (fields > 0 && rows > (uint64_t)INT_MAX / fields)) {
        free(result);
        return NULL;
    }

    result->rows = (int)rows;
    result->fields = (int)fields;
    result->cursor = -1;

    {
        size_t total = (size_t)rows * (size_t)fields;

        result->names = calloc(fields ? fields : 1, sizeof(char *));
        result->cells = calloc(total ? total : 1, sizeof(char *));
    }

    if (!result->names || !result->cells)
        goto failed;

    for (f = 0; f < result->fields; f++) {
        void *field = my.mysql_fetch_field_direct(raw, (unsigned int)f);
        const char *name = field ? *(char **)field : NULL;

        result->names[f] = copy_text(name ? name : "");

        if (!result->names[f])
            goto failed;
    }

    for (r = 0; r < result->rows; r++) {
        char **row = my.mysql_fetch_row(raw);

        if (!row) {
            result->rows = r;
            break;
        }

        for (f = 0; f < result->fields; f++) {
            if (row[f]) {
                result->cells[(size_t)r * result->fields + f] = copy_text(row[f]);

                if (!result->cells[(size_t)r * result->fields + f])
                    goto failed;
            }
        }
    }

    return result;

failed:
    free_result_data(result);
    return NULL;
}

/* mysql_query(connection, sql):int
   Runs the query and gives a result handle, or NONE if it failed. A statement
   that gives no rows (INSERT, UPDATE, ...) gives an empty result. */
static void native_query(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_query", 0);
    MyRes raw;
    Result *result;
    int id;

    if (!connection)
        return;

    if (!run_query(connection, api->GetStr(call, 1))) {
        api->ReturnNone(call);
        return;
    }

    raw = my.mysql_store_result(connection->handle);

    if (!raw) {
        if (my.mysql_field_count(connection->handle) != 0) {
            /* A result was expected but could not be read: an error */
            api->ReturnNone(call);
            return;
        }

        result = calloc(1, sizeof(Result));

        if (result) {
            result->cursor = -1;
            result->names = calloc(1, sizeof(char *));
            result->cells = calloc(1, sizeof(char *));
        }
    } else {
        result = copy_result(raw);
        my.mysql_free_result(raw);
    }

    id = result ? add_result(result) : 0;

    if (!id) {
        free_result_data(result);
        api->Error(call, "mysql_query: out of memory while reading the result");
        return;
    }

    api->ReturnInt(call, id);
}

/* mysql_exec(connection, sql):int
   Runs a query and gives the number of rows it changed, or NONE if it
   failed. No result handle to free: for statements that do not need one. */
static void native_exec(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_exec", 0);
    uint64_t affected;
    MyRes raw;

    if (!connection)
        return;

    if (!run_query(connection, api->GetStr(call, 1))) {
        api->ReturnNone(call);
        return;
    }

    affected = my.mysql_affected_rows(connection->handle);
    raw = my.mysql_store_result(connection->handle);

    if (raw)
        my.mysql_free_result(raw);
    else if (my.mysql_field_count(connection->handle) != 0) {
        api->ReturnNone(call);
        return;
    }

    if (affected == (uint64_t)-1)
        affected = 0;

    return_count(call, "mysql_exec", affected);
}

/* mysql_affected_rows(connection):int */
static void native_affected_rows(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_affected_rows", 0);
    uint64_t affected;

    if (!connection)
        return;

    affected = my.mysql_affected_rows(connection->handle);

    if (affected == (uint64_t)-1)
        api->ReturnNone(call);
    else
        return_count(call, "mysql_affected_rows", affected);
}

/* mysql_insert_id(connection):int, the AUTO_INCREMENT id of the last INSERT */
static void native_insert_id(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_insert_id", 0);

    if (connection)
        return_count(call, "mysql_insert_id", my.mysql_insert_id(connection->handle));
}

/* mysql_escape(connection, text):str, text made safe to put between quotes */
static void native_escape(ELCall *call)
{
    Connection *connection = get_connection(call, "mysql_escape", 0);
    const char *text;
    size_t length;
    char *escaped;
    unsigned long written;

    if (!connection)
        return;

    text = api->GetStr(call, 1);
    length = strlen(text);
    escaped = malloc(length * 2 + 1);

    if (!escaped) {
        api->Error(call, "mysql_escape: out of memory");
        return;
    }

    written = my.mysql_real_escape_string(connection->handle, escaped, text,
                                          (unsigned long)length);
    escaped[written] = '\0';

    api->ReturnStr(call, escaped);
    free(escaped);
}

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

/* mysql_free_result(result) */
static void native_free_result(ELCall *call)
{
    Result *result = get_result(call, "mysql_free_result", 0);

    if (!result)
        return;

    results[api->GetInt(call, 0) - 1] = NULL;
    free_result_data(result);
}

static void native_num_rows(ELCall *call)
{
    Result *result = get_result(call, "mysql_num_rows", 0);

    if (result)
        api->ReturnInt(call, result->rows);
}

static void native_num_fields(ELCall *call)
{
    Result *result = get_result(call, "mysql_num_fields", 0);

    if (result)
        api->ReturnInt(call, result->fields);
}

/* True if the two column names are the same, ignoring upper/lower case */
static int same_name(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;

        a++;
        b++;
    }

    return *a == *b;
}

/* Column number of 'name', or -1. An exact match wins over a match that
   ignores case. */
static int find_field(const Result *result, const char *name)
{
    int i;

    for (i = 0; i < result->fields; i++) {
        if (strcmp(result->names[i], name) == 0)
            return i;
    }

    for (i = 0; i < result->fields; i++) {
        if (same_name(result->names[i], name))
            return i;
    }

    return -1;
}

/* mysql_field_name(result, column):str */
static void native_field_name(ELCall *call)
{
    Result *result = get_result(call, "mysql_field_name", 0);
    long long column;

    if (!result)
        return;

    column = api->GetInt(call, 1);

    if (column < 0 || column >= result->fields) {
        api->Error(call, "mysql_field_name: column %lld is out of range (the result has %d column(s))",
                   column, result->fields);
        return;
    }

    api->ReturnStr(call, result->names[column]);
}

/* mysql_field_index(result, name):int, NONE if there is no such column */
static void native_field_index(ELCall *call)
{
    Result *result = get_result(call, "mysql_field_index", 0);
    int column;

    if (!result)
        return;

    column = find_field(result, api->GetStr(call, 1));

    if (column < 0)
        api->ReturnNone(call);
    else
        api->ReturnInt(call, column);
}

/* The value (or NULL for an SQL NULL) at row/column. Reports and gives
   *ok = 0 when row or column does not exist. */
static const char *cell_at(ELCall *call, const char *function, Result *result,
                           long long row, long long column, int *ok)
{
    *ok = 0;

    if (row < 0 || row >= result->rows) {
        api->Error(call, "%s: row %lld is out of range (the result has %d row(s))",
                   function, row, result->rows);
        return NULL;
    }

    if (column < 0 || column >= result->fields) {
        api->Error(call, "%s: column %lld is out of range (the result has %d column(s))",
                   function, column, result->fields);
        return NULL;
    }

    *ok = 1;
    return result->cells[(size_t)row * result->fields + column];
}

/* Read a whole text as an int. Returns 1 if it is one, else reports. */
static int parse_int(ELCall *call, const char *function, const char *text, long long *value)
{
    char *end;
    long long parsed;

    parsed = strtoll(text, &end, 10);

    while (*end == ' ' || *end == '\t')
        end++;

    if (text[0] == '\0' || *end != '\0') {
        api->Error(call, "%s: the value '%s' is not an integer", function, text);
        return 0;
    }

    if (parsed < INT_MIN || parsed > INT_MAX) {
        api->Error(call, "%s: the value %s does not fit in an EL int (32 bit), read it with the text function instead", function, text);
        return 0;
    }

    *value = parsed;
    return 1;
}

static int parse_float(ELCall *call, const char *function, const char *text, double *value)
{
    char *end;
    double parsed = strtod(text, &end);

    while (*end == ' ' || *end == '\t')
        end++;

    if (text[0] == '\0' || *end != '\0') {
        api->Error(call, "%s: the value '%s' is not a number", function, text);
        return 0;
    }

    *value = parsed;
    return 1;
}

/* mysql_get(result, row, column):str */
static void native_get(ELCall *call)
{
    Result *result = get_result(call, "mysql_get", 0);
    const char *value;
    int ok;

    if (!result)
        return;

    value = cell_at(call, "mysql_get", result, api->GetInt(call, 1), api->GetInt(call, 2), &ok);

    if (!ok)
        return;

    if (value)
        api->ReturnStr(call, value);
    else
        api->ReturnNone(call);
}

/* mysql_get_int(result, row, column):int */
static void native_get_int(ELCall *call)
{
    Result *result = get_result(call, "mysql_get_int", 0);
    const char *value;
    long long number;
    int ok;

    if (!result)
        return;

    value = cell_at(call, "mysql_get_int", result, api->GetInt(call, 1), api->GetInt(call, 2), &ok);

    if (!ok)
        return;

    if (!value)
        api->ReturnNone(call);
    else if (parse_int(call, "mysql_get_int", value, &number))
        api->ReturnInt(call, number);
}

/* mysql_get_float(result, row, column):float */
static void native_get_float(ELCall *call)
{
    Result *result = get_result(call, "mysql_get_float", 0);
    const char *value;
    double number;
    int ok;

    if (!result)
        return;

    value = cell_at(call, "mysql_get_float", result, api->GetInt(call, 1), api->GetInt(call, 2), &ok);

    if (!ok)
        return;

    if (!value)
        api->ReturnNone(call);
    else if (parse_float(call, "mysql_get_float", value, &number))
        api->ReturnFloat(call, number);
}

/* mysql_fetch_row(result):bool
   Moves to the next row. False when there are no more rows. */
static void native_fetch_row(ELCall *call)
{
    Result *result = get_result(call, "mysql_fetch_row", 0);

    if (!result)
        return;

    if (result->cursor < result->rows)
        result->cursor++;

    api->ReturnBool(call, result->cursor < result->rows);
}

/* mysql_rewind(result), go back to before the first row */
static void native_rewind(ELCall *call)
{
    Result *result = get_result(call, "mysql_rewind", 0);

    if (result)
        result->cursor = -1;
}

/* The value of column 'name' in the row mysql_fetch_row is on */
static const char *current_value(ELCall *call, const char *function, int *ok)
{
    Result *result = get_result(call, function, 0);
    const char *name;
    int column;

    *ok = 0;

    if (!result)
        return NULL;

    if (result->cursor < 0 || result->cursor >= result->rows) {
        api->Error(call, "%s: there is no current row (call mysql_fetch_row first, and stop when it gives false)", function);
        return NULL;
    }

    name = api->GetStr(call, 1);
    column = find_field(result, name);

    if (column < 0) {
        api->Error(call, "%s: the result has no column '%s'", function, name);
        return NULL;
    }

    *ok = 1;
    return result->cells[(size_t)result->cursor * result->fields + column];
}

/* mysql_field(result, name):str */
static void native_field(ELCall *call)
{
    int ok;
    const char *value = current_value(call, "mysql_field", &ok);

    if (!ok)
        return;

    if (value)
        api->ReturnStr(call, value);
    else
        api->ReturnNone(call);
}

/* mysql_field_int(result, name):int */
static void native_field_int(ELCall *call)
{
    int ok;
    long long number;
    const char *value = current_value(call, "mysql_field_int", &ok);

    if (!ok)
        return;

    if (!value)
        api->ReturnNone(call);
    else if (parse_int(call, "mysql_field_int", value, &number))
        api->ReturnInt(call, number);
}

/* mysql_field_float(result, name):float */
static void native_field_float(ELCall *call)
{
    int ok;
    double number;
    const char *value = current_value(call, "mysql_field_float", &ok);

    if (!ok)
        return;

    if (!value)
        api->ReturnNone(call);
    else if (parse_float(call, "mysql_field_float", value, &number))
        api->ReturnFloat(call, number);
}

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */

EL_EXPORT int elp_Supports(void)
{
    return EL_PLUGIN_API_VERSION;
}

EL_EXPORT int elp_Load(const ELApi *a)
{
    static const struct { const char *signature; ELNative function; } natives[] = {
        {"mysql_connect(str, str, str, str, int):int", native_connect},
        {"mysql_close(int)", native_close},
        {"mysql_ping(int):bool", native_ping},
        {"mysql_set_charset(int, str):bool", native_set_charset},
        {"mysql_error(int):str", native_error},
        {"mysql_errno(int):int", native_errno},
        {"mysql_last_error():str", native_last_error},
        {"mysql_last_errno():int", native_last_errno},
        {"mysql_set_timeout(int)", native_set_timeout},
        {"mysql_set_library(str):bool", native_set_library},
        {"mysql_client_library():str", native_client_library},
        {"mysql_query(int, str):int", native_query},
        {"mysql_exec(int, str):int", native_exec},
        {"mysql_affected_rows(int):int", native_affected_rows},
        {"mysql_insert_id(int):int", native_insert_id},
        {"mysql_escape(int, str):str", native_escape},
        {"mysql_free_result(int)", native_free_result},
        {"mysql_num_rows(int):int", native_num_rows},
        {"mysql_num_fields(int):int", native_num_fields},
        {"mysql_field_name(int, int):str", native_field_name},
        {"mysql_field_index(int, str):int", native_field_index},
        {"mysql_get(int, int, int):str", native_get},
        {"mysql_get_int(int, int, int):int", native_get_int},
        {"mysql_get_float(int, int, int):float", native_get_float},
        {"mysql_fetch_row(int):bool", native_fetch_row},
        {"mysql_rewind(int)", native_rewind},
        {"mysql_field(int, str):str", native_field},
        {"mysql_field_int(int, str):int", native_field_int},
        {"mysql_field_float(int, str):float", native_field_float}
    };
    size_t i;

    api = a;

    for (i = 0; i < sizeof(natives) / sizeof(natives[0]); i++) {
        if (!api->Register(natives[i].signature, natives[i].function))
            return 1;
    }

    return 0;
}

/* Close what the program left open */
EL_EXPORT void elp_Unload(void)
{
    int i;

    for (i = 0; i < connection_capacity; i++) {
        if (connections[i]) {
            if (client)
                my.mysql_close(connections[i]->handle);

            free(connections[i]);
            connections[i] = NULL;
        }
    }

    for (i = 0; i < result_capacity; i++) {
        free_result_data(results[i]);
        results[i] = NULL;
    }

    free(connections);
    free(results);
    connections = NULL;
    results = NULL;
    connection_capacity = 0;
    result_capacity = 0;
}
