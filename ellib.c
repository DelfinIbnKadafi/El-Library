/*
 * ellib - El Language library manager.
 *
 * Works on Windows and on Linux/macOS. Everything that depends on the
 * operating system lives in the "platform layer" (first half of this file),
 * the rest of the file is the same for every system.
 */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#else
/* Needed for mkdtemp(), readlink() and lstat() while compiling with -std=c99 */
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define APP_VERSION "1.3.0"
#define MAX_FILES 128
#define MAX_PATH_LEN 4096
#define MAX_JSON_SIZE (1024 * 1024)

#ifdef _WIN32
#define PLUGIN_EXTENSION ".dll"
#elif defined(__APPLE__)
#define PLUGIN_EXTENSION ".dylib"
#else
#define PLUGIN_EXTENSION ".so"
#endif

#ifdef _WIN32
#define PATH_SEP '\\'
/* Windows has zip support built in (PowerShell), Linux always has tar */
#define ARCHIVE_KIND "zip"
#define ARCHIVE_NAME "repo.zip"
#define names_equal(a, b) (_stricmp((a), (b)) == 0)
#else
#define PATH_SEP '/'
#define ARCHIVE_KIND "tar.gz"
#define ARCHIVE_NAME "repo.tar.gz"
#define names_equal(a, b) (strcmp((a), (b)) == 0)
#endif

typedef struct {
    char name[128];
    char files[MAX_FILES][512];
    int file_count;

    /* Native plugins listed in "plugins". Only the ones for this system are
       kept in 'plugins' (with the extension added when the entry has none),
       'plugin_declared' counts every entry of the list. */
    char plugins[MAX_FILES][512];
    int plugin_count;
    int plugin_declared;
} Library;

static char exe_dir[MAX_PATH_LEN];
static char library_dir[MAX_PATH_LEN];

/* ------------------------------------------------------------------ */
/* Platform layer                                                      */
/* ------------------------------------------------------------------ */

/* Last operating system error. It is saved right after a call fails, so
   that other calls made afterwards cannot overwrite it. */
static char os_error_text[256];
static int os_error_permission;

static void save_errno(int code)
{
    snprintf(os_error_text, sizeof(os_error_text), "%s", strerror(code));

    os_error_permission = code == EACCES || code == EPERM;

#ifndef _WIN32
    if (code == EROFS)
        os_error_permission = 1;
#endif
}

#ifdef _WIN32
static void save_win_error(DWORD code)
{
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, 0, os_error_text, sizeof(os_error_text), NULL);

    /* Windows messages end with ".\r\n" */
    while (n > 0 && (os_error_text[n - 1] == '\r' ||
                     os_error_text[n - 1] == '\n' ||
                     os_error_text[n - 1] == ' '))
        os_error_text[--n] = '\0';

    if (n == 0)
        snprintf(os_error_text, sizeof(os_error_text), "unknown error");

    size_t used = strlen(os_error_text);

    snprintf(os_error_text + used, sizeof(os_error_text) - used,
             " (error %lu)", (unsigned long)code);

    os_error_permission = code == ERROR_ACCESS_DENIED ||
                          code == ERROR_WRITE_PROTECT;
}
#endif

/* Print the reason of the last saved error, and what to do about it when
   the reason is a missing permission. */
static void print_os_error(void)
{
    if (!os_error_text[0])
        return;

    printf("       Reason: %s\n", os_error_text);

    if (!os_error_permission)
        return;

#ifdef _WIN32
    puts("Hint: your user account is not allowed to change the library folder.");
    puts("      Run ellib from an Administrator terminal, or reinstall ELVM");
    puts("      with the latest installer, which makes that folder writable.");
#else
    puts("Hint: your user is not allowed to change the library folder.");
    puts("      Run ellib with sudo, or install ELVM in a folder you own.");
#endif
}

static int is_separator(char c)
{
    return c == '/' || c == '\\';
}

static int path_join(char *out, size_t size,
                     const char *a, const char *b)
{
    size_t len = strlen(a);
    int n;

    if (len > 0 && is_separator(a[len - 1]))
        n = snprintf(out, size, "%s%s", a, b);
    else
        n = snprintf(out, size, "%s%c%s", a, PATH_SEP, b);

    return n >= 0 && (size_t)n < size;
}

static int is_directory(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int path_exists(const char *path)
{
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* Create one directory. It is fine if the directory already exists. */
static int make_directory(const char *path)
{
    if (is_directory(path))
        return 1;

#ifdef _WIN32
    if (CreateDirectoryA(path, NULL))
        return 1;

    DWORD code = GetLastError();

    if (code == ERROR_ALREADY_EXISTS && is_directory(path))
        return 1;

    save_win_error(code);
    return 0;
#else
    if (mkdir(path, 0755) == 0)
        return 1;

    int code = errno;

    if (code == EEXIST && is_directory(path))
        return 1;

    save_errno(code);
    return 0;
#endif
}

#ifdef _WIN32
/* PowerShell strings written between '...' only need ' to be doubled */
static int powershell_quote(const char *in, char *out, size_t capacity)
{
    size_t i = 0;

    for (; *in; in++) {
        size_t needed = *in == '\'' ? 2 : 1;

        if (i + needed >= capacity)
            return 0;

        out[i++] = *in;

        if (*in == '\'')
            out[i++] = '\'';
    }

    out[i] = '\0';
    return 1;
}

/* Append one argument to a command line, between double quotes */
static int append_quoted(char *cmd, size_t size, size_t *len,
                         const char *arg)
{
    size_t n = strlen(arg);
    size_t trailing = 0;

    /* A double quote cannot be written safely inside an argument. None of
       the arguments used by ellib can contain one. */
    if (strchr(arg, '"'))
        return 0;

    /* Backslashes right before the closing quote must be doubled */
    while (trailing < n && arg[n - 1 - trailing] == '\\')
        trailing++;

    if (*len + n + trailing + 3 >= size)
        return 0;

    cmd[(*len)++] = '"';
    memcpy(cmd + *len, arg, n);
    *len += n;

    for (size_t i = 0; i < trailing; i++)
        cmd[(*len)++] = '\\';

    cmd[(*len)++] = '"';
    cmd[*len] = '\0';

    return 1;
}
#endif

/* Run a program and wait for it. 'argv' ends with NULL, 'cwd' may be NULL.
   Returns the exit code of the program, or -1 if it could not be started.
   No shell is involved, so no argument can be taken as a command. */
static int run_process(const char *cwd, char *const argv[])
{
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH_LEN * 3];
    size_t len = 0;

    cmd[0] = '\0';

    for (int i = 0; argv[i]; i++) {
        if (i > 0) {
            if (len + 1 >= sizeof(cmd))
                return -1;

            cmd[len++] = ' ';
            cmd[len] = '\0';
        }

        if (!append_quoted(cmd, sizeof(cmd), &len, argv[i]))
            return -1;
    }

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(
        NULL, cmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, cwd, &si, &pi))
        return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return (int)exit_code;
#else
    int status;
    pid_t pid;

    fflush(stdout);

    pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        if (cwd && chdir(cwd) != 0)
            _exit(126);

        execvp(argv[0], argv);

        /* The program was not found */
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Find the folder of the ellib executable. The libraries are stored in a
   folder named "library" next to it, the same place elvm looks in. */
static int init_paths(const char *argv0)
{
    char exe[MAX_PATH_LEN];

    exe[0] = '\0';

#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));

    if (!n || n >= sizeof(exe))
        exe[0] = '\0';

    (void)argv0;
#elif defined(__APPLE__)
    uint32_t size = sizeof(exe);

    if (_NSGetExecutablePath(exe, &size) != 0)
        exe[0] = '\0';

    (void)argv0;
#else
    ssize_t n = -1;

#ifdef __linux__
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
#endif

    if (n > 0) {
        exe[n] = '\0';
    } else if (argv0 && strchr(argv0, '/') &&
               strlen(argv0) < sizeof(exe)) {
        /* Other systems: argv[0] is only usable when it contains a path */
        strcpy(exe, argv0);
    }
#endif

    if (!exe[0] || strlen(exe) >= sizeof(exe_dir))
        return 0;

    strcpy(exe_dir, exe);

    char *slash = NULL;

    for (char *p = exe_dir; *p; p++) {
        if (is_separator(*p))
            slash = p;
    }

    if (!slash)
        return 0;

    /* A file directly in the root folder keeps the root as its folder */
    if (slash == exe_dir)
        slash++;

    *slash = '\0';

    return path_join(
        library_dir, sizeof(library_dir),
        exe_dir, "library");
}

#ifndef _WIN32
/* Delete a file, a link, or a folder with everything inside it. Links are
   removed, never followed. */
static int remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT)
            return 1;

        save_errno(errno);
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) == 0)
            return 1;

        save_errno(errno);
        return 0;
    }

    /* A folder without write permission cannot have its content deleted */
    chmod(path, st.st_mode | S_IRWXU);

    DIR *dir = opendir(path);

    if (!dir) {
        save_errno(errno);
        return 0;
    }

    int ok = 1;
    struct dirent *entry;

    while (ok && (entry = readdir(dir)) != NULL) {
        char child[MAX_PATH_LEN];

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        if (!path_join(child, sizeof(child), path, entry->d_name))
            ok = 0;
        else
            ok = remove_tree(child);
    }

    closedir(dir);

    if (!ok)
        return 0;

    if (rmdir(path) != 0) {
        save_errno(errno);
        return 0;
    }

    return 1;
}
#endif

/* Delete a folder with everything inside it */
static int remove_directory(const char *path)
{
#ifdef _WIN32
    /* PowerShell is used only with a path constructed from a validated
       package name and the known library directory. */
    char quoted[MAX_PATH_LEN * 2];
    char script[MAX_PATH_LEN * 2 + 128];

    os_error_text[0] = '\0';

    if (!powershell_quote(path, quoted, sizeof(quoted)) ||
        snprintf(script, sizeof(script),
                 "Remove-Item -LiteralPath '%s' "
                 "-Recurse -Force -ErrorAction Stop",
                 quoted) >= (int)sizeof(script))
        return 0;

    char *argv[] = { "powershell.exe", "-NoProfile", "-Command",
                     script, NULL };

    return run_process(NULL, argv) == 0;
#else
    return remove_tree(path);
#endif
}

/* Create a new, empty folder for temporary files */
static int create_temp_directory(char *out, size_t capacity)
{
#ifdef _WIN32
    char temp_path[MAX_PATH_LEN];
    char candidate[MAX_PATH_LEN];
    DWORD length = GetTempPathA(sizeof(temp_path), temp_path);

    if (!length || length >= sizeof(temp_path) ||
        !GetTempFileNameA(temp_path, "ell", 0, candidate))
        return 0;

    if (strlen(candidate) >= capacity) {
        DeleteFileA(candidate);
        return 0;
    }

    if (!DeleteFileA(candidate) ||
        (!CreateDirectoryA(candidate, NULL) && !is_directory(candidate)))
        return 0;

    strcpy(out, candidate);
    return 1;
#else
    const char *temp = getenv("TMPDIR");

    if (!temp || !*temp)
        temp = "/tmp";

    if (snprintf(out, capacity, "%s/ellibXXXXXX", temp) >= (int)capacity)
        return 0;

    if (!mkdtemp(out)) {
        save_errno(errno);
        return 0;
    }

    return 1;
#endif
}

/* Call 'callback' for every folder directly inside 'dir', until a call
   returns non-zero. Returns 1 if the search was stopped that way. */
static int for_each_subdirectory(
    const char *dir,
    int (*callback)(const char *path, void *context),
    void *context)
{
    char path[MAX_PATH_LEN];
    int stop = 0;

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH_LEN + 2];

    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >=
        (int)sizeof(pattern))
        return 0;

    HANDLE h = FindFirstFileA(pattern, &fd);

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            strcmp(fd.cFileName, ".") &&
            strcmp(fd.cFileName, "..") &&
            path_join(path, sizeof(path), dir, fd.cFileName) &&
            callback(path, context)) {
            stop = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *entry;

    if (!d)
        return 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") &&
            strcmp(entry->d_name, "..") &&
            path_join(path, sizeof(path), dir, entry->d_name) &&
            is_directory(path) &&
            callback(path, context)) {
            stop = 1;
            break;
        }
    }

    closedir(d);
#endif

    return stop;
}

static int download_repo(const char *repo,
                         const char *branch,
                         const char *archive_path)
{
    char url[1024];

    const char *slash = strchr(repo, '/');
    char owner[128], name[128];

    size_t owner_len = (size_t)(slash - repo);
    memcpy(owner, repo, owner_len);
    owner[owner_len] = '\0';
    strcpy(name, slash + 1);

    if (snprintf(url, sizeof(url),
        "https://codeload.github.com/%s/%s/" ARCHIVE_KIND "/refs/heads/%s",
        owner, name, branch) >= (int)sizeof(url))
        return 0;

#ifdef _WIN32
    char *curl[] = { "curl.exe", "-fLsS", "--retry", "2",
                     "-o", (char *)archive_path, url, NULL };

    return run_process(NULL, curl) == 0;
#else
    char *curl[] = { "curl", "-fLsS", "--retry", "2",
                     "-o", (char *)archive_path, url, NULL };

    int code = run_process(NULL, curl);

    /* curl is not installed, try wget */
    if (code == 127 || code == -1) {
        char *wget[] = { "wget", "-q", "-O", (char *)archive_path,
                         url, NULL };

        code = run_process(NULL, wget);
    }

    if (code != 0)
        remove(archive_path);

    return code == 0;
#endif
}

/* Unpack the archive that was downloaded to 'work' (see ARCHIVE_NAME) into
   the same folder 'work'. */
static int extract_archive(const char *work)
{
#ifdef _WIN32
    char archive[MAX_PATH_LEN];
    char quoted_archive[MAX_PATH_LEN * 2];
    char quoted_work[MAX_PATH_LEN * 2];
    char script[MAX_PATH_LEN * 4 + 128];

    if (!path_join(archive, sizeof(archive), work, ARCHIVE_NAME) ||
        !powershell_quote(archive, quoted_archive, sizeof(quoted_archive)) ||
        !powershell_quote(work, quoted_work, sizeof(quoted_work)) ||
        snprintf(script, sizeof(script),
                 "Expand-Archive -LiteralPath '%s' "
                 "-DestinationPath '%s' -Force",
                 quoted_archive, quoted_work) >= (int)sizeof(script))
        return 0;

    char *argv[] = { "powershell.exe", "-NoProfile", "-Command",
                     script, NULL };

    return run_process(NULL, argv) == 0;
#else
    /* Running inside 'work' with a plain file name keeps tar away from any
       special characters in the path of the temporary folder. */
    char *argv[] = { "tar", "-xzf", ARCHIVE_NAME, NULL };

    return run_process(work, argv) == 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Shared code                                                         */
/* ------------------------------------------------------------------ */

static int valid_name(const char *name)
{
    if (!name || !*name || name[0] == '.')
        return 0;

    for (const unsigned char *p =
         (const unsigned char *)name; *p; p++) {
        if (!isalnum(*p) && *p != '_' &&
            *p != '-' && *p != '.')
            return 0;
    }

    return 1;
}

static int valid_repo(const char *repo)
{
    const char *slash = strchr(repo, '/');

    if (!slash || slash == repo ||
        !slash[1] || strchr(slash + 1, '/'))
        return 0;

    char owner[128], name[128];
    size_t len = (size_t)(slash - repo);

    if (len >= sizeof(owner) ||
        strlen(slash + 1) >= sizeof(name))
        return 0;

    memcpy(owner, repo, len);
    owner[len] = '\0';
    strcpy(name, slash + 1);

    return valid_name(owner) && valid_name(name);
}

/* Create the library folder if it is missing, and make sure this user can
   really create and delete folders in it. This is checked before anything
   is downloaded, so a missing permission is reported at once, and with the
   real reason. */
static int ensure_library_directory(void)
{
    char probe[MAX_PATH_LEN];
    char name[64];

    os_error_text[0] = '\0';

    if (!make_directory(library_dir)) {
        printf("Error: cannot create the library folder:\n       %s\n",
               library_dir);
        print_os_error();
        return 0;
    }

#ifdef _WIN32
    snprintf(name, sizeof(name), ".ellib-test-%lu",
             (unsigned long)GetCurrentProcessId());
#else
    snprintf(name, sizeof(name), ".ellib-test-%lu", (unsigned long)getpid());
#endif

    if (!path_join(probe, sizeof(probe), library_dir, name))
        return 0;

    if (!make_directory(probe)) {
        printf("Error: cannot write to the library folder:\n       %s\n",
               library_dir);
        print_os_error();
        return 0;
    }

    remove_directory(probe);
    return 1;
}

/* Create every folder that leads to the file 'path' */
static int make_parent_directories(const char *path)
{
    char temp[MAX_PATH_LEN];

    if (strlen(path) >= sizeof(temp))
        return 0;

    strcpy(temp, path);

    for (char *p = temp + 1; *p; p++) {
        if (!is_separator(*p))
            continue;

        char saved = *p;
        *p = '\0';

        /* "C:" is a drive, not a folder to create */
        int is_drive = p - temp == 2 && temp[1] == ':';

        if (!is_drive && !make_directory(temp))
            return 0;

        *p = saved;
    }

    return 1;
}

/* Find the text after "key": in a JSON text (spaces skipped), or NULL.
   This is a limited reader, not a full JSON implementation. */
static const char *json_value(const char *json, const char *key)
{
    char pattern[160];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    size_t len = strlen(pattern);
    const char *p = json;

    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = p + len;

        while (isspace((unsigned char)*q))
            q++;

        /* The text may also be a value, only a key is followed by ':' */
        if (*q == ':') {
            q++;

            while (isspace((unsigned char)*q))
                q++;

            return q;
        }

        p += len;
    }

    return NULL;
}

/* Read a JSON string value for a given key. */
static int json_string(const char *json, const char *key,
                       char *out, size_t capacity)
{
    const char *p = json_value(json, key);

    if (!p || *p++ != '"')
        return 0;

    size_t i = 0;

    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;

            if (*p != '"' && *p != '\\' &&
                *p != '/')
                return 0;
        }

        if (i + 1 >= capacity)
            return 0;

        out[i++] = *p++;
    }

    if (*p != '"')
        return 0;

    out[i] = '\0';
    return 1;
}

static int safe_relative_file(const char *file)
{
    if (!file || !*file ||
        file[0] == '\\' || file[0] == '/' ||
        (strlen(file) >= 2 && file[1] == ':'))
        return 0;

    if (strstr(file, ".."))
        return 0;

    for (const unsigned char *p =
         (const unsigned char *)file; *p; p++) {
        if (!(isalnum(*p) || *p == '_' ||
              *p == '-' || *p == '.' ||
              *p == '/' || *p == '\\'))
            return 0;
    }

    return 1;
}

/* Length of the plugin extension (.dll, .so, .dylib) at the end of a name,
   or 0 if it has none. 'for_this_system' tells if it is this system's own. */
static size_t plugin_extension(const char *name, int *for_this_system)
{
    static const char *extensions[] = {".dll", ".so", ".dylib"};
    size_t length = strlen(name);

    for (int i = 0; i < 3; i++) {
        size_t ext_length = strlen(extensions[i]);

        if (length <= ext_length)
            continue;

        const char *tail = name + length - ext_length;
        int same = 1;

        for (size_t j = 0; j < ext_length; j++) {
            if (tolower((unsigned char)tail[j]) != extensions[i][j]) {
                same = 0;
                break;
            }
        }

        if (same) {
            *for_this_system =
                strcmp(extensions[i], PLUGIN_EXTENSION) == 0;
            return ext_length;
        }
    }

    return 0;
}

/* Same rule elvm uses for the "plugins" list: an entry with the extension of
   another system is skipped, an entry with no extension gets this system's
   one. Returns 1 and the file name in 'out' if the entry is for this system. */
static int plugin_for_this_system(const char *entry, char *out,
                                  size_t capacity)
{
    int for_this_system = 0;
    size_t ext_length = plugin_extension(entry, &for_this_system);

    if (ext_length > 0 && !for_this_system)
        return 0;

    size_t needed = strlen(entry) + (ext_length > 0 ?
                    0 : strlen(PLUGIN_EXTENSION)) + 1;

    if (needed > capacity)
        return 0;

    strcpy(out, entry);

    if (ext_length == 0)
        strcat(out, PLUGIN_EXTENSION);

    return 1;
}

/* Read the JSON array that starts at 'p' (the value of "key") as a list of
   relative file paths. Returns 1 if it is fine, or 0 after printing why not. */
static int read_path_array(const char *key, const char *p,
                           char (*out)[512], int *count)
{
    if (*p++ != '[') {
        printf("Error: %s must be an array.\n", key);
        return 0;
    }

    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',')
            p++;

        if (*p == ']')
            break;

        if (*p++ != '"') {
            printf("Error: invalid %s array.\n", key);
            return 0;
        }

        if (*count >= MAX_FILES) {
            printf("Error: too many entries in %s.\n", key);
            return 0;
        }

        char *dest = out[*count];
        size_t i = 0;

        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
                if (*p != '/' && *p != '\\' &&
                    *p != '"') {
                    printf("Error: invalid file escape.\n");
                    return 0;
                }
            }

            if (i + 1 >= sizeof(out[0]))
                return 0;

            dest[i++] = *p++;
        }

        if (*p != '"')
            return 0;

        dest[i] = '\0';
        p++;

        if (!safe_relative_file(dest)) {
            printf("Error: unsafe file path: %s\n", dest);
            return 0;
        }

        /* A library written on Windows may use '\' between folders, store
           it with '/' so the file also has the right name on Linux */
        for (char *c = dest; *c; c++) {
            if (*c == '\\')
                *c = '/';
        }

        (*count)++;
    }

    return 1;
}

static int read_manifest(const char *path, Library *lib)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        printf("Error: library.json not found.\n");
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }

    long size = ftell(f);

    if (size <= 0 || size > MAX_JSON_SIZE) {
        fclose(f);
        printf("Error: invalid manifest size.\n");
        return 0;
    }

    rewind(f);

    char *json = malloc((size_t)size + 1);

    if (!json) {
        fclose(f);
        return 0;
    }

    size_t bytes = fread(json, 1, (size_t)size, f);
    fclose(f);
    json[bytes] = '\0';

    memset(lib, 0, sizeof(*lib));

    if (!json_string(json, "name",
                     lib->name, sizeof(lib->name)) ||
        !valid_name(lib->name)) {
        printf("Error: invalid library name in manifest.\n");
        free(json);
        return 0;
    }

    char (*declared)[512] = malloc(sizeof(char[512]) * MAX_FILES);

    if (!declared) {
        free(json);
        return 0;
    }

    const char *plugins_json = json_value(json, "plugins");
    int declared_count = 0;

    if (plugins_json &&
        !read_path_array("plugins", plugins_json, declared,
                         &declared_count)) {
        free(declared);
        free(json);
        return 0;
    }

    lib->plugin_declared = declared_count;

    for (int i = 0; i < declared_count; i++) {
        if (plugin_for_this_system(declared[i],
                                   lib->plugins[lib->plugin_count],
                                   sizeof(lib->plugins[0])))
            lib->plugin_count++;
    }

    free(declared);

    /* A library needs files, or plugins (it can be only a plugin) */
    const char *files_json = json_value(json, "files");

    if (!files_json) {
        if (declared_count == 0) {
            printf("Error: missing files array.\n");
            free(json);
            return 0;
        }
    } else if (!read_path_array("files", files_json, lib->files,
                                &lib->file_count)) {
        free(json);
        return 0;
    }

    free(json);

    if (lib->file_count == 0 && declared_count == 0) {
        printf("Error: no files declared in manifest.\n");
        return 0;
    }

    return 1;
}

static int copy_file(const char *source, const char *target)
{
    FILE *in = fopen(source, "rb");

    if (!in) {
        save_errno(errno);
        return 0;
    }

    if (!make_parent_directories(target)) {
        fclose(in);
        return 0;
    }

    FILE *out = fopen(target, "wb");

    if (!out) {
        save_errno(errno);
        fclose(in);
        return 0;
    }

    char buffer[16384];
    size_t n;
    int ok = 1;

    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            save_errno(errno);
            ok = 0;
            break;
        }
    }

    if (ferror(in) && ok) {
        save_errno(errno);
        ok = 0;
    }

    fclose(in);

    if (fclose(out) != 0 && ok) {
        save_errno(errno);
        ok = 0;
    }

    return ok;
}

/* Plugins are native programs, so say so before installing one. A library
   from an unofficial repository also needs the user's confirmation. */
static int confirm_native_plugins(const Library *lib, int ask)
{
    printf("Note: library '%s' includes native plugin code:\n", lib->name);

    for (int i = 0; i < lib->plugin_count; i++)
        printf("  %s\n", lib->plugins[i]);

    puts("A plugin is a program that runs with your permissions.");

    if (!ask)
        return 1;

    printf("Only install it if you trust the author. Continue? [y/N] ");
    fflush(stdout);

    char answer[16];

    if (!fgets(answer, sizeof(answer), stdin) ||
        (answer[0] != 'y' && answer[0] != 'Y')) {
        puts("Cancelled.");
        return 0;
    }

    return 1;
}

static int confirm_reinstall(void)
{
    char answer[16];

    printf("This library already installed, install it again? (Y/N) : ");
    fflush(stdout);

    if (!fgets(answer, sizeof(answer), stdin) ||
        (answer[0] != 'y' && answer[0] != 'Y')) {
        puts("Cancelled.");
        return 0;
    }

    return 1;
}

static int install_library(const char *source,
                           const char *requested_name,
                           int ask_about_plugins,
                           int explicit_reinstall)
{
    char manifest[MAX_PATH_LEN];
    char target[MAX_PATH_LEN];
    char stage[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    Library lib;
    int was_installed;

    os_error_text[0] = '\0';

    if (!path_join(manifest, sizeof(manifest),
                   source, "library.json")) {
        return 0;
    }

    if (!read_manifest(manifest, &lib))
        return 0;

    if (requested_name &&
        !names_equal(requested_name, lib.name)) {
        printf("Error: requested '%s', manifest names '%s'.\n",
               requested_name, lib.name);
        return 0;
    }

    /* Validate all declared files before creating or copying anything, so a
       broken package never leaves a half installed folder behind. */
    for (int i = 0; i < lib.file_count; i++) {
        char src[MAX_PATH_LEN];

        if (!path_join(src, sizeof(src), source, lib.files[i]) ||
            !path_exists(src)) {
            printf("Error: missing package file: %s\n",
                   lib.files[i]);
            return 0;
        }
    }

    if (lib.plugin_declared > 0 && lib.plugin_count == 0) {
        printf("Error: library '%s' has no plugin for this system"
               " (it needs a '%s' file).\n", lib.name, PLUGIN_EXTENSION);
        return 0;
    }

    for (int i = 0; i < lib.plugin_count; i++) {
        char src[MAX_PATH_LEN];

        if (!path_join(src, sizeof(src), source, lib.plugins[i]) ||
            !path_exists(src)) {
            printf("Error: missing plugin file: %s\n", lib.plugins[i]);
            return 0;
        }
    }

    if (lib.plugin_count > 0 &&
        !confirm_native_plugins(&lib, ask_about_plugins))
        return 0;

    if (!make_directory(library_dir)) {
        printf("Error: cannot create library directory.\n");
        print_os_error();
        return 0;
    }

    if (!path_join(target, sizeof(target),
                   library_dir, lib.name)) {
        return 0;
    }

    was_installed = is_directory(target);

    if (explicit_reinstall && !was_installed) {
        printf("Error: library not installed: %s\n", lib.name);
        return 0;
    }

    if (was_installed && !explicit_reinstall &&
        !confirm_reinstall())
        return 0;

    int found_stage = 0;

    for (int attempt = 0; attempt < 100; attempt++) {
        char stage_name[256];
        char backup_name[256];

        snprintf(stage_name, sizeof(stage_name),
                 ".ellib-stage-%s-%d", lib.name, attempt);
        snprintf(backup_name, sizeof(backup_name),
                 ".ellib-backup-%s-%d", lib.name, attempt);

        if (!path_join(stage, sizeof(stage), library_dir, stage_name) ||
            !path_join(backup, sizeof(backup), library_dir, backup_name))
            return 0;

        if (!path_exists(stage) && !path_exists(backup)) {
            found_stage = 1;
            break;
        }
    }

    if (!found_stage) {
        printf("Error: cannot choose a temporary package directory.\n");
        return 0;
    }

    if (!make_directory(stage)) {
        printf("Error: cannot create package directory:\n       %s\n",
               stage);
        print_os_error();
        return 0;
    }

    for (int i = 0; i < lib.file_count; i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        path_join(src, sizeof(src), source, lib.files[i]);
        path_join(dst, sizeof(dst), stage, lib.files[i]);

        if (!copy_file(src, dst)) {
            printf("Error: failed copying %s\n", lib.files[i]);
            print_os_error();
            remove_directory(stage);
            return 0;
        }
    }

    for (int i = 0; i < lib.plugin_count; i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        path_join(src, sizeof(src), source, lib.plugins[i]);
        path_join(dst, sizeof(dst), stage, lib.plugins[i]);

        if (!copy_file(src, dst)) {
            printf("Error: failed copying %s\n", lib.plugins[i]);
            print_os_error();
            remove_directory(stage);
            return 0;
        }
    }

    char dst_manifest[MAX_PATH_LEN];

    if (!path_join(dst_manifest, sizeof(dst_manifest),
                   stage, "library.json") ||
        !copy_file(manifest, dst_manifest)) {
        printf("Error: failed copying library.json.\n");
        print_os_error();
        remove_directory(stage);
        return 0;
    }

    if (was_installed) {
        if (rename(target, backup) != 0) {
            save_errno(errno);
            printf("Error: could not move the existing library:\n       %s\n",
                   target);
            print_os_error();
            remove_directory(stage);
            return 0;
        }
    }

    if (rename(stage, target) != 0) {
        save_errno(errno);
        printf("Error: could not activate the new library:\n       %s\n",
               target);
        print_os_error();
        if (was_installed && rename(backup, target) != 0) {
            save_errno(errno);
            printf("Error: could not restore the previous library from:\n"
                   "       %s\n", backup);
            print_os_error();
        }
        remove_directory(stage);
        return 0;
    }

    if (was_installed && !remove_directory(backup)) {
        printf("Error: new library installed, but could not remove backup:\n"
               "       %s\n", backup);
        print_os_error();
        return 0;
    }

    printf("%s library '%s' successfully.\n",
           was_installed ? "Reinstalled" : "Installed", lib.name);
    return 1;
}

/* Used while searching a downloaded repository for the package */
typedef struct {
    const char *package_name;
    char source[MAX_PATH_LEN];
} SourceSearch;

/* 'root' is the folder inside the unpacked archive (GitHub puts everything
   in one folder named like "repository-main") */
static int check_package_root(const char *root, void *context)
{
    SourceSearch *search = context;
    char candidate[MAX_PATH_LEN];
    char manifest[MAX_PATH_LEN];

    if (search->package_name) {
        char package_root[MAX_PATH_LEN];

        if (!path_join(package_root, sizeof(package_root),
                       root, "library") ||
            !path_join(candidate, sizeof(candidate),
                       package_root, search->package_name))
            return 0;
    } else {
        if (strlen(root) >= sizeof(candidate))
            return 0;

        strcpy(candidate, root);
    }

    if (!path_join(manifest, sizeof(manifest),
                   candidate, "library.json") ||
        !path_exists(manifest))
        return 0;

    strcpy(search->source, candidate);
    return 1;
}

static int install_github(const char *repo,
                          const char *package_name,
                          int assume_yes,
                          int explicit_reinstall)
{
    char work[MAX_PATH_LEN], archive[MAX_PATH_LEN];

    /* Fail early, before downloading, if the library folder is not usable */
    if (!ensure_library_directory())
        return 0;

    if (!create_temp_directory(work, sizeof(work))) {
        printf("Error: cannot create temporary directory.\n");
        print_os_error();
        return 0;
    }

    if (!path_join(archive, sizeof(archive), work, ARCHIVE_NAME)) {
        remove_directory(work);
        return 0;
    }

    int downloaded = download_repo(repo, "main", archive);

    if (!downloaded)
        downloaded = download_repo(repo, "master", archive);

    if (!downloaded) {
        printf("Error: failed downloading GitHub repository.\n");
        puts("       Check your internet connection, and that curl is installed.");
        remove_directory(work);
        return 0;
    }

    if (!extract_archive(work)) {
        printf("Error: failed extracting repository.\n");
        remove_directory(work);
        return 0;
    }

    SourceSearch search;

    search.package_name = package_name;
    search.source[0] = '\0';

    int found = for_each_subdirectory(work, check_package_root, &search);
    int result = 0;

    if (!found) {
        if (package_name)
            printf("Error: official library not found: %s\n", package_name);
        else
            printf("Error: repository root must contain library.json.\n");
    } else {
        /* The official repository is reviewed before a library is added,
           any other repository is not, so ask before installing its plugins */
        result = install_library(search.source, package_name,
                                 package_name == NULL && !assume_yes,
                                 explicit_reinstall);
    }

    remove_directory(work);
    return result;
}

static void usage(void)
{
    puts("EL Library (ellib) " APP_VERSION);
    puts("");
    puts("Usage:");
    puts("  ellib install <library_name>");
    puts("  ellib install -g <username/repository_name> [-y]");
    puts("  ellib reinstall <library_name>");
    puts("  ellib uninstall <library_name>");
    puts("  ellib --version");
    puts("  ellib --help");
    puts("");
    puts("-y installs the native plugins of an unofficial repository without");
    puts("asking first.");
}

int main(int argc, char **argv)
{
    if (!init_paths(argv[0])) {
        printf("Error: cannot determine executable directory.\n");
        return 1;
    }

    if (argc == 2 &&
        (!strcmp(argv[1], "--version") ||
         !strcmp(argv[1], "-v"))) {
        puts("ellib " APP_VERSION);
        return 0;
    }

    if (argc == 2 &&
        (!strcmp(argv[1], "--help") ||
         !strcmp(argv[1], "-h"))) {
        usage();
        return 0;
    }

    if (argc == 3 &&
        !strcmp(argv[1], "uninstall") &&
        valid_name(argv[2])) {
        char target[MAX_PATH_LEN];

        if (!path_join(target, sizeof(target),
                       library_dir, argv[2])) {
            return 1;
        }

        if (!is_directory(target)) {
            printf("Error: library not installed: %s\n", argv[2]);
            return 1;
        }

        /* Report a missing permission with its real reason */
        if (!ensure_library_directory())
            return 1;

        if (!remove_directory(target)) {
            printf("Error: could not uninstall library.\n");
            print_os_error();
            return 1;
        }

        printf("Uninstalled '%s' successfully.\n", argv[2]);
        return 0;
    }

    if ((argc == 4 || argc == 5) &&
        !strcmp(argv[1], "install") &&
        !strcmp(argv[2], "-g") &&
        valid_repo(argv[3]) &&
        (argc == 4 || !strcmp(argv[4], "-y") ||
         !strcmp(argv[4], "--yes"))) {
        return install_github(argv[3], NULL, argc == 5, 0) ? 0 : 1;
    }

    if (argc == 3 &&
        !strcmp(argv[1], "install") &&
        valid_name(argv[2])) {
        return install_github(
            "DelfinIbnKadafi/El-Library", argv[2], 0, 0) ? 0 : 1;
    }

    if (argc == 3 &&
        !strcmp(argv[1], "reinstall") &&
        valid_name(argv[2])) {
        return install_github(
            "DelfinIbnKadafi/El-Library", argv[2], 0, 1) ? 0 : 1;
    }

    usage();
    return 1;
}
