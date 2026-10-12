
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <direct.h>

#define APP_VERSION "1.0.0"
#define MAX_FILES 128
#define MAX_PATH_LEN 4096
#define MAX_JSON_SIZE (1024 * 1024)

typedef struct {
    char name[128];
    char files[MAX_FILES][512];
    int file_count;
} Library;

static char exe_dir[MAX_PATH_LEN];
static char library_dir[MAX_PATH_LEN];

static int path_join(char *out, size_t size,
                     const char *a, const char *b)
{
    int n = snprintf(out, size, "%s\\%s", a, b);
    return n >= 0 && (size_t)n < size;
}

static int is_directory(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int valid_name(const char *name)
{
    if (!name || !*name ||
        !strcmp(name, ".") || !strcmp(name, ".."))
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

static int init_paths(void)
{
    DWORD n = GetModuleFileNameA(
        NULL, exe_dir, sizeof(exe_dir));

    if (!n || n >= sizeof(exe_dir))
        return 0;

    char *slash = strrchr(exe_dir, '\\');

    if (!slash)
        return 0;

    *slash = '\0';

    return path_join(
        library_dir, sizeof(library_dir),
        exe_dir, "library");
}

static int make_directory(const char *path)
{
    if (is_directory(path))
        return 1;

    return _mkdir(path) == 0 || is_directory(path);
}

static int create_temp_directory(char *out, size_t capacity)
{
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
}

static int make_parent_directories(const char *path)
{
    char temp[MAX_PATH_LEN];

    if (strlen(path) >= sizeof(temp))
        return 0;

    strcpy(temp, path);

    char *last = strrchr(temp, '\\');

    if (last) {
        *last = '\0';

        for (char *p = temp + 3; *p; p++) {
            if (*p == '\\') {
                *p = '\0';
                if (*temp && !is_directory(temp))
                    _mkdir(temp);
                *p = '\\';
            }
        }

        if (*temp && !is_directory(temp))
            _mkdir(temp);
    }

    return 1;
}

static int run_process(const char *command)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    char cmd[MAX_PATH_LEN * 2];

    if (strlen(command) >= sizeof(cmd))
        return 0;

    strcpy(cmd, command);

    if (!CreateProcessA(
        NULL, cmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return exit_code == 0;
}

static int download_repo(const char *repo,
                         const char *branch,
                         const char *zip_path)
{
    char url[1024];
    char command[MAX_PATH_LEN * 2];

    const char *slash = strchr(repo, '/');
    char owner[128], name[128];

    size_t owner_len = (size_t)(slash - repo);
    memcpy(owner, repo, owner_len);
    owner[owner_len] = '\0';
    strcpy(name, slash + 1);

    if (snprintf(url, sizeof(url),
        "https://codeload.github.com/%s/%s/zip/refs/heads/%s",
        owner, name, branch) >= (int)sizeof(url))
        return 0;

    if (snprintf(command, sizeof(command),
        "curl.exe -fLsS --retry 2 -o \"%s\" \"%s\"",
        zip_path, url) >= (int)sizeof(command))
        return 0;

    return run_process(command);
}

static int extract_zip(const char *zip,
                       const char *destination)
{
    char command[MAX_PATH_LEN * 3];

    if (snprintf(command, sizeof(command),
        "powershell.exe -NoProfile -Command "
        "\"Expand-Archive -LiteralPath '%s' "
        "-DestinationPath '%s' -Force\"",
        zip, destination) >= (int)sizeof(command))
        return 0;

    return run_process(command);
}

/* Read a JSON string value for a given key.
   This is a limited parser, not a full JSON implementation. */
static int json_string(const char *json, const char *key,
                       char *out, size_t capacity)
{
    char pattern[160];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);

    if (!p)
        return 0;

    p += strlen(pattern);

    while (isspace((unsigned char)*p))
        p++;

    if (*p++ != ':')
        return 0;

    while (isspace((unsigned char)*p))
        p++;

    if (*p++ != '"')
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

    const char *p = strstr(json, "\"files\"");

    if (!p || !(p = strchr(p, ':'))) {
        printf("Error: missing files array.\n");
        free(json);
        return 0;
    }

    p++;

    while (isspace((unsigned char)*p))
        p++;

    if (*p++ != '[') {
        printf("Error: files must be an array.\n");
        free(json);
        return 0;
    }

    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',')
            p++;

        if (*p == ']')
            break;

        if (*p++ != '"') {
            printf("Error: invalid files array.\n");
            free(json);
            return 0;
        }

        if (lib->file_count >= MAX_FILES) {
            printf("Error: too many files in manifest.\n");
            free(json);
            return 0;
        }

        char *out = lib->files[lib->file_count];
        size_t i = 0;

        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
                if (*p != '/' && *p != '\\' &&
                    *p != '"') {
                    printf("Error: invalid file escape.\n");
                    free(json);
                    return 0;
                }
            }

            if (i + 1 >= sizeof(lib->files[0])) {
                free(json);
                return 0;
            }

            out[i++] = *p++;
        }

        if (*p != '"') {
            free(json);
            return 0;
        }

        out[i] = '\0';
        p++;

        if (!safe_relative_file(out)) {
            printf("Error: unsafe file path: %s\n", out);
            free(json);
            return 0;
        }

        lib->file_count++;
    }

    free(json);

    if (lib->file_count == 0) {
        printf("Error: no files declared in manifest.\n");
        return 0;
    }

    return 1;
}

static int copy_file(const char *source, const char *target)
{
    FILE *in = fopen(source, "rb");

    if (!in)
        return 0;

    make_parent_directories(target);

    FILE *out = fopen(target, "wb");

    if (!out) {
        fclose(in);
        return 0;
    }

    char buffer[16384];
    size_t n;
    int ok = 1;

    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }

    if (ferror(in))
        ok = 0;

    fclose(in);
    fclose(out);

    return ok;
}

static int install_library(const char *source,
                           const char *requested_name)
{
    char manifest[MAX_PATH_LEN];
    char target[MAX_PATH_LEN];
    Library lib;

    if (!path_join(manifest, sizeof(manifest),
                   source, "library.json")) {
        return 0;
    }

    if (!read_manifest(manifest, &lib))
        return 0;

    if (requested_name &&
        _stricmp(requested_name, lib.name) != 0) {
        printf("Error: requested '%s', manifest names '%s'.\n",
               requested_name, lib.name);
        return 0;
    }

    if (!make_directory(library_dir)) {
        printf("Error: cannot create library directory.\n");
        return 0;
    }

    if (!path_join(target, sizeof(target),
                   library_dir, lib.name)) {
        return 0;
    }

    if (is_directory(target)) {
        printf("Error: library already installed: %s\n", lib.name);
        return 0;
    }

    if (!make_directory(target)) {
        printf("Error: cannot create package directory.\n");
        return 0;
    }

    /* Validate all declared files before copying any of them. */
    for (int i = 0; i < lib.file_count; i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        if (!path_join(src, sizeof(src), source, lib.files[i]) ||
            !path_join(dst, sizeof(dst), target, lib.files[i]) ||
            GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
            printf("Error: missing package file: %s\n",
                   lib.files[i]);
            return 0;
        }
    }

    for (int i = 0; i < lib.file_count; i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        path_join(src, sizeof(src), source, lib.files[i]);
        path_join(dst, sizeof(dst), target, lib.files[i]);

        if (!copy_file(src, dst)) {
            printf("Error: failed copying %s\n", lib.files[i]);
            return 0;
        }
    }

    char dst_manifest[MAX_PATH_LEN];

    if (!path_join(dst_manifest, sizeof(dst_manifest),
                   target, "library.json") ||
        !copy_file(manifest, dst_manifest)) {
        printf("Error: failed copying library.json.\n");
        return 0;
    }

    printf("Installed library '%s' successfully.\n", lib.name);
    return 1;
}

static int remove_directory(const char *path)
{
    /* PowerShell is used only with a path constructed from a
       validated package name and the known library directory. */
    char command[MAX_PATH_LEN * 2];

    if (snprintf(command, sizeof(command),
        "powershell.exe -NoProfile -Command "
        "\"Remove-Item -LiteralPath '%s' "
        "-Recurse -Force -ErrorAction Stop\"",
        path) >= (int)sizeof(command))
        return 0;

    return run_process(command);
}

static int install_github(const char *repo,
                          const char *package_name)
{
    char work[MAX_PATH_LEN], zip[MAX_PATH_LEN];
    char source[MAX_PATH_LEN];

    if (!create_temp_directory(work, sizeof(work))) {
        printf("Error: cannot create temporary directory.\n");
        return 0;
    }

    if (!path_join(zip, sizeof(zip), work, "repo.zip")) {
        remove_directory(work);
        return 0;
    }

    int downloaded = download_repo(repo, "main", zip);

    if (!downloaded)
        downloaded = download_repo(repo, "master", zip);

    if (!downloaded) {
        printf("Error: failed downloading GitHub repository.\n");
        remove_directory(work);
        return 0;
    }

    if (!extract_zip(zip, work)) {
        printf("Error: failed extracting repository.\n");
        remove_directory(work);
        return 0;
    }

    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH_LEN + 2];
    char package_root[MAX_PATH_LEN];

    snprintf(pattern, sizeof(pattern), "%s\\*", work);

    HANDLE h = FindFirstFileA(pattern, &fd);
    int found = 0;

    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                strcmp(fd.cFileName, ".") &&
                strcmp(fd.cFileName, "..")) {
                if (path_join(source, sizeof(source),
                              work, fd.cFileName)) {
                    char manifest[MAX_PATH_LEN];

                    int has_manifest = 0;

                    if (package_name) {
                        has_manifest =
                            path_join(package_root, sizeof(package_root),
                                      source, "library") &&
                            path_join(source, sizeof(source),
                                      package_root, package_name) &&
                            path_join(manifest, sizeof(manifest),
                                      source, "library.json") &&
                            GetFileAttributesA(manifest) != INVALID_FILE_ATTRIBUTES;
                    } else {
                        has_manifest =
                            path_join(manifest, sizeof(manifest),
                                      source, "library.json") &&
                            GetFileAttributesA(manifest) != INVALID_FILE_ATTRIBUTES;
                    }

                    if (has_manifest) {
                        found = 1;
                        break;
                    }
                }
            }
        } while (FindNextFileA(h, &fd));

        FindClose(h);
    }

    int result = 0;

    if (!found) {
        if (package_name)
            printf("Error: official library not found: %s\n", package_name);
        else
            printf("Error: repository root must contain library.json.\n");
    } else {
        result = install_library(source, package_name);
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
    puts("  ellib install -g <username/repository_name>");
    puts("  ellib uninstall <library_name>");
    puts("  ellib --version");
    puts("  ellib --help");
}

int main(int argc, char **argv)
{
    if (!init_paths()) {
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

        if (!remove_directory(target)) {
            printf("Error: could not uninstall library.\n");
            return 1;
        }

        printf("Uninstalled '%s' successfully.\n", argv[2]);
        return 0;
    }

    if (argc == 4 &&
        !strcmp(argv[1], "install") &&
        !strcmp(argv[2], "-g") &&
        valid_repo(argv[3])) {
        return install_github(argv[3], NULL) ? 0 : 1;
    }

    if (argc == 3 &&
        !strcmp(argv[1], "install") &&
        valid_name(argv[2])) {
        return install_github(
            "DelfinIbnKadafi/El-Library", argv[2]) ? 0 : 1;
    }

    usage();
    return 1;
}
