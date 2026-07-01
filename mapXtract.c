#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>
#include <archive.h>
#include <archive_entry.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <process.h>
#define MKDIR(path) _mkdir(path)
#define PATH_SEP '\\'
#define getpid() _getpid()
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/time.h>
#define MKDIR(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

#define MAX_PATH_LEN 256

char bsp_files[1000][256];
int bsp_count = 0;
int total_junked = 0; 
FILE* log_file = NULL;

struct pending_txt {
    char temp_path[MAX_PATH_LEN];
    char original_pathname[MAX_PATH_LEN];
};

struct pending_txt pending_txt_files[1000];
int pending_txt_count = 0;
char temp_files_created[1000][MAX_PATH_LEN];
int temp_file_count = 0;

// Track successfully processed archives for deletion
char processed_archives[1000][MAX_PATH_LEN];
int processed_archive_count = 0;

struct archive *global_archive = NULL;
FILE* global_output_file = NULL;

const char* get_filename(const char* path);
void cleanup_temp_files(void);
void log_printf(const char* format, ...);

void add_processed_archive(const char* archive_path) {
    if (processed_archive_count >= 1000) {
        log_printf("WARNING: Maximum archive limit (1000) reached, stopping archive tracking\n");
        return;
    }
    strncpy(processed_archives[processed_archive_count], archive_path, MAX_PATH_LEN - 1);
    processed_archives[processed_archive_count][MAX_PATH_LEN - 1] = '\0';
    processed_archive_count++;
}

void log_printf(const char* format, ...);

void delete_processed_archives(void) {
    for (int i = 0; i < processed_archive_count; i++) {
        if (processed_archives[i][0] != '\0') {
            if (remove(processed_archives[i]) == 0) {
                log_printf("deleted archive: %s\n", processed_archives[i]);
            } else {
                log_printf("failed to delete archive: %s - %s\n", processed_archives[i], strerror(errno));
            }
        }
    }
}

void emergency_cleanup(void) {
    cleanup_temp_files();
    
    if (global_output_file) {
        fclose(global_output_file);
        global_output_file = NULL;
    }
    
    if (global_archive) {
        archive_read_free(global_archive);
        global_archive = NULL;
    }
    
    if (log_file) {
        fprintf(log_file, "\n>>> Emergency cleanup performed\n");
        fclose(log_file);
        log_file = NULL;
    }
}

void signal_handler(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    emergency_cleanup();
    exit(sig);
}

void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef SIGPIPE
    signal(SIGPIPE, signal_handler);
#endif
#ifndef _WIN32
    signal(SIGHUP, signal_handler);
    signal(SIGQUIT, signal_handler);
#endif
}

void cleanup_temp_files(void) {
    for (int i = 0; i < temp_file_count; i++) {
        if (temp_files_created[i][0] != '\0') {
            remove(temp_files_created[i]);
            temp_files_created[i][0] = '\0';
        }
    }
    temp_file_count = 0;
}

void add_temp_file_for_cleanup(const char* path) {
    if (temp_file_count >= 1000) {
        log_printf("WARNING: Maximum temp file limit (1000) reached, stopping temp file tracking\n");
        return;
    }
    strncpy(temp_files_created[temp_file_count], path, MAX_PATH_LEN - 1);
    temp_files_created[temp_file_count][MAX_PATH_LEN - 1] = '\0';
    temp_file_count++;
}

void remove_temp_file_from_cleanup(const char* path) {
    for (int i = 0; i < temp_file_count; i++) {
        if (strcmp(temp_files_created[i], path) == 0) {
            temp_files_created[i][0] = '\0';
            break;
        }
    }
}

int generate_unique_temp_filename(const char* baseDir, const char* original_filename, char* temp_path, size_t temp_path_size) {
    static unsigned int counter = 0;
    int pid = getpid();
    
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    unsigned int timestamp = st.wMilliseconds + (st.wSecond * 1000) + (st.wMinute * 60000);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int timestamp = (tv.tv_usec / 1000) + (tv.tv_sec % 3600) * 1000;
#endif
    
    counter++;
    
    char safe_filename[128];
    const char* name_only = get_filename(original_filename);
    int name_len = strlen(name_only);
    if (name_len > 80) {
        snprintf(safe_filename, sizeof(safe_filename), "%.80s", name_only);
    } else {
        strncpy(safe_filename, name_only, sizeof(safe_filename) - 1);
        safe_filename[sizeof(safe_filename) - 1] = '\0';
    }
    
    for (int i = 0; safe_filename[i]; i++) {
        if (safe_filename[i] == ' ' || safe_filename[i] == '\t' || 
            safe_filename[i] == '<' || safe_filename[i] == '>' || 
            safe_filename[i] == ':' || safe_filename[i] == '"' || 
            safe_filename[i] == '|' || safe_filename[i] == '?' || 
            safe_filename[i] == '*') {
            safe_filename[i] = '_';
        }
    }
    
    for (int attempt = 0; attempt < 1000; attempt++) {
        int result = snprintf(temp_path, temp_path_size, "%s%ctemp_%d_%u_%u_%s", 
                             baseDir, PATH_SEP, pid, timestamp, counter + attempt, safe_filename);
        
        if (result >= temp_path_size) {
            return 0;
        }
        
        FILE* test = fopen(temp_path, "r");
        if (!test) {
            return 1;
        }
        fclose(test);
    }
    
    return 0;
}

void log_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    vprintf(format, args);
    
    if (log_file) {
        va_start(args, format);
        vfprintf(log_file, format, args);
        fflush(log_file);
    }
    
    va_end(args);
}

int validate_path_length(const char* path, const char* operation) {
    if (strlen(path) >= MAX_PATH_LEN) {
        log_printf("ERROR: Path too long for %s: %s\n", operation, path);
        return 0;
    }
    return 1;
}

int is_safe_path(const char* path) {
    if (!path) return 0;
    
    char path_copy[MAX_PATH_LEN];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    char* pos = path_copy;
    
    while (*pos) {
        if (pos[0] == '.' && pos[1] == '.' && 
            (pos[2] == '/' || pos[2] == '\\' || pos[2] == '\0')) {
            return 0;
        }
        
        if (pos == path_copy && (pos[0] == '/' || pos[0] == '\\')) {
            return 0;
        }
        
        pos++;
    }
    
    return 1;
}

void sanitize_path(const char* input_path, char* output_path, size_t output_size) {
    if (!input_path || !output_path || output_size == 0) return;
    
    const char* start = input_path;
    while (*start == '/' || *start == '\\') start++;
    
    char temp[MAX_PATH_LEN];
    strncpy(temp, start, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    char* write_pos = temp;
    char* read_pos = temp;
    
    while (*read_pos) {
        if (*read_pos == '\\') {
            *read_pos = '/';
        }
        read_pos++;
    }
    
    read_pos = temp;
    write_pos = temp;
    
    while (*read_pos) {
        if (read_pos[0] == '/' && read_pos[1] == '/') {
            read_pos++;
            continue;
        }
        
        if (read_pos[0] == '.' && read_pos[1] == '/' && read_pos == temp) {
            read_pos += 2;
            continue;
        }
        
        if (read_pos[0] == '/' && read_pos[1] == '.' && read_pos[2] == '/') {
            read_pos += 2;
            continue;
        }
        
        *write_pos++ = *read_pos++;
    }
    *write_pos = '\0';
    
    strncpy(output_path, temp, output_size - 1);
    output_path[output_size - 1] = '\0';
}

void create_dir(const char *path) {
    if (!validate_path_length(path, "directory creation")) return;
    
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (MKDIR(path) == -1 && errno != EEXIST) {
            log_printf("mkdir failed for %s: %s\n", path, strerror(errno));
            return;
        }
    }
}

void create_dirs_recursive(const char *filepath) {
    if (!validate_path_length(filepath, "recursive directory creation")) return;
    
    char path[MAX_PATH_LEN];
    strncpy(path, filepath, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    
    char *last_slash = strrchr(path, '/');
    if (!last_slash) last_slash = strrchr(path, '\\');
    
    if (last_slash) {
        *last_slash = '\0';
        
        char temp[MAX_PATH_LEN];
        temp[0] = '\0';
        
        char *token = strtok(path, "/\\");
        while (token) {
            size_t current_len = strlen(temp);
            size_t token_len = strlen(token);
            
            if (current_len > 0) {
                if (current_len + 1 >= sizeof(temp)) break;
                strncat(temp, "/", sizeof(temp) - current_len - 1);
                current_len++;
            }
            
            if (current_len + token_len >= sizeof(temp)) break;
            strncat(temp, token, sizeof(temp) - current_len - 1);
            
            if (!validate_path_length(temp, "intermediate directory")) break;
            create_dir(temp);
            token = strtok(NULL, "/\\");
        }
    }
}

void to_lowercase(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower(str[i]);
    }
}

const char* get_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";
    return dot + 1;
}

const char* get_filename(const char* path) {
    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = (slash1 > slash2) ? slash1 : slash2;
    if (!slash) return path;
    return slash + 1;
}

void add_bsp_file(const char* filename) {
    if (bsp_count >= 1000) {
        log_printf("WARNING: Maximum BSP file limit (1000) reached, stopping BSP tracking\n");
        return;
    }
    
    const char* bsp_name = get_filename(filename);
    strncpy(bsp_files[bsp_count], bsp_name, sizeof(bsp_files[bsp_count]) - 1);
    bsp_files[bsp_count][sizeof(bsp_files[bsp_count]) - 1] = '\0';
    
    char* dot = strrchr(bsp_files[bsp_count], '.');
    if (dot) *dot = '\0';
    
    bsp_count++;
}

void add_pending_txt(const char* temp_path, const char* original_pathname) {
    if (pending_txt_count >= 1000) {
        log_printf("WARNING: Maximum pending TXT file limit (1000) reached, stopping TXT tracking\n");
        return;
    }
    if (!validate_path_length(temp_path, "pending txt temp") || 
        !validate_path_length(original_pathname, "pending txt original")) return;
    
    strncpy(pending_txt_files[pending_txt_count].temp_path, temp_path, sizeof(pending_txt_files[pending_txt_count].temp_path) - 1);
    pending_txt_files[pending_txt_count].temp_path[sizeof(pending_txt_files[pending_txt_count].temp_path) - 1] = '\0';
    
    strncpy(pending_txt_files[pending_txt_count].original_pathname, original_pathname, sizeof(pending_txt_files[pending_txt_count].original_pathname) - 1);
    pending_txt_files[pending_txt_count].original_pathname[sizeof(pending_txt_files[pending_txt_count].original_pathname) - 1] = '\0';
    
    pending_txt_count++;
}

int txt_has_matching_bsp(const char* txt_filename) {
    char txt_name[256];
    strncpy(txt_name, get_filename(txt_filename), sizeof(txt_name) - 1);
    txt_name[sizeof(txt_name) - 1] = '\0';
    
    char* dot = strrchr(txt_name, '.');
    if (dot) *dot = '\0';
    
    for (int i = 0; i < bsp_count; i++) {
        if (strcmp(txt_name, bsp_files[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int is_overview_description_file(const char* filepath) {
    if (!validate_path_length(filepath, "overview check")) return 0;
    
    FILE* file = fopen(filepath, "r");
    if (!file) return 0;
    
    char line[256];
    int is_overview = 0;
    if (fgets(line, sizeof(line), file)) {
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        
        if (strncmp(trimmed, "// overview description file", 28) == 0) {
            is_overview = 1;
        }
    }
    fclose(file);
    return is_overview;
}

void get_target_directory(const char* filename, const char* original_path, char* target_dir, size_t target_dir_size, int skip_addons) {
    char ext_lower[16];
    const char* ext = get_extension(filename);
    strncpy(ext_lower, ext, sizeof(ext_lower) - 1);
    ext_lower[sizeof(ext_lower) - 1] = '\0';
    to_lowercase(ext_lower);
    
    char path_lower[MAX_PATH_LEN];
    strncpy(path_lower, original_path, sizeof(path_lower) - 1);
    path_lower[sizeof(path_lower) - 1] = '\0';
    to_lowercase(path_lower);
    
    if (strstr(path_lower, "addons/") != NULL || strstr(path_lower, "addons\\") != NULL) {
        if (skip_addons) {
            strcpy(target_dir, "SKIP");
        } else {
            strcpy(target_dir, "ADDONS_PRESERVE");
        }
        return;
    }
    
    if (strcmp(ext_lower, "bsp") == 0 || strcmp(ext_lower, "res") == 0 || strcmp(ext_lower, "nav") == 0) {
        strcpy(target_dir, "maps");
    }
    else if (strcmp(ext_lower, "txt") == 0) {
        strcpy(target_dir, "TXT_CHECK");
    }
    else if (strcmp(ext_lower, "mdl") == 0) {
        strcpy(target_dir, "models");
    }
    else if (strcmp(ext_lower, "bmp") == 0) {
        // Check if BMP file is in gfx\ or its subdirectories
        if (strstr(path_lower, "gfx/") != NULL || strstr(path_lower, "gfx\\") != NULL) {
            strcpy(target_dir, "gfx/env");
        } else {
            strcpy(target_dir, "overviews");
        }
    }
    else if (strcmp(ext_lower, "wav") == 0) {
        strcpy(target_dir, "sound");
    }
    else if (strcmp(ext_lower, "spr") == 0) {
        strcpy(target_dir, "sprites");
    }
    else if (strcmp(ext_lower, "tga") == 0) {
        strcpy(target_dir, "gfx/env");
    }
    else if (strcmp(ext_lower, "wad") == 0) {
        strcpy(target_dir, "BASEDIR");
    }
    else if (strcmp(ext_lower, "cfg") == 0 || strcmp(ext_lower, "ini") == 0) {
        strcpy(target_dir, "junk");
    }
    else {
        strcpy(target_dir, "junk");
    }
}

int path_has_correct_structure(const char* path, const char* target_dir) {
    if (target_dir[0] == '\0') return 1;
    
    char path_lower[MAX_PATH_LEN];
    strncpy(path_lower, path, sizeof(path_lower) - 1);
    path_lower[sizeof(path_lower) - 1] = '\0';
    to_lowercase(path_lower);
    
    char target_lower[256];
    strncpy(target_lower, target_dir, sizeof(target_lower) - 1);
    target_lower[sizeof(target_lower) - 1] = '\0';
    to_lowercase(target_lower);
    
    return strncmp(path_lower, target_lower, strlen(target_lower)) == 0 &&
           (path_lower[strlen(target_lower)] == '/' || path_lower[strlen(target_lower)] == '\\' || path_lower[strlen(target_lower)] == '\0');
}

int is_supported_archive(const char* filename) {
    char filename_lower[256];
    strncpy(filename_lower, filename, sizeof(filename_lower) - 1);
    filename_lower[sizeof(filename_lower) - 1] = '\0';
    to_lowercase(filename_lower);
    
    return (strstr(filename_lower, ".zip") != NULL ||
            strstr(filename_lower, ".rar") != NULL ||
            strstr(filename_lower, ".7z") != NULL);
}

const char* detect_archive_type(const char* filename) {
    char filename_lower[256];
    strncpy(filename_lower, filename, sizeof(filename_lower) - 1);
    filename_lower[sizeof(filename_lower) - 1] = '\0';
    to_lowercase(filename_lower);
    
    if (strstr(filename_lower, ".zip") != NULL) return "ZIP";
    if (strstr(filename_lower, ".rar") != NULL) return "RAR";
    if (strstr(filename_lower, ".7z") != NULL) return "7Z";
    return "UNKNOWN";
}

void process_pending_txt_files(const char* baseDir, int* junked, int delete_junk) {
    log_printf("processing txt files...\n");
    
    // Lazy BSP matching - if no BSPs found, junk all TXT files immediately
    if (bsp_count == 0) {
        log_printf("no BSP files found, junking all TXT files...\n");
        for (int i = 0; i < pending_txt_count; i++) {
            const char* temp_path = pending_txt_files[i].temp_path;
            const char* original_pathname = pending_txt_files[i].original_pathname;
            const char* filename = get_filename(original_pathname);
            
            if (delete_junk) {
                remove(temp_path);
                remove_temp_file_from_cleanup(temp_path);
                log_printf("txt file deleted (junk): %s\n", filename);
                (*junked)++;
            } else {
                char actualPath[MAX_PATH_LEN];
                int result = snprintf(actualPath, sizeof(actualPath), "%s/junk/%s", baseDir, filename);
                
                if (result >= sizeof(actualPath)) {
                    log_printf("ERROR: Junk path too long, skipping: %s\n", filename);
                    remove(temp_path);
                    continue;
                }
                
                char junkDir[MAX_PATH_LEN];
                int dir_result = snprintf(junkDir, sizeof(junkDir), "%s/junk", baseDir);
                if (dir_result < sizeof(junkDir)) {
                    create_dir(junkDir);
                }
                
                if (rename(temp_path, actualPath) == 0) {
                    remove_temp_file_from_cleanup(temp_path);
                    log_printf("txt file junked: %s\n", actualPath);
                    (*junked)++;
                } else {
                    remove(temp_path);
                    remove_temp_file_from_cleanup(temp_path);
                    (*junked)++;
                }
            }
        }
        return;
    }
    
    for (int i = 0; i < pending_txt_count; i++) {
        const char* temp_path = pending_txt_files[i].temp_path;
        const char* original_pathname = pending_txt_files[i].original_pathname;
        
        char actualPath[MAX_PATH_LEN];
        
        if (txt_has_matching_bsp(original_pathname) || is_overview_description_file(temp_path)) {
            if (is_overview_description_file(temp_path)) {
                const char* filename = get_filename(original_pathname);
                int result = snprintf(actualPath, sizeof(actualPath), "%s/overviews/%s", baseDir, filename);
                
                if (result >= sizeof(actualPath)) {
                    log_printf("ERROR: Overview path too long, skipping: %s\n", filename);
                    remove(temp_path);
                    continue;
                }
                
                char overviewsDir[MAX_PATH_LEN];
                int dir_result = snprintf(overviewsDir, sizeof(overviewsDir), "%s/overviews", baseDir);
                if (dir_result < sizeof(overviewsDir)) {
                    create_dir(overviewsDir);
                }
            } else {
                const char* filename = get_filename(original_pathname);
                int result = snprintf(actualPath, sizeof(actualPath), "%s/maps/%s", baseDir, filename);
                
                if (result >= sizeof(actualPath)) {
                    log_printf("ERROR: Maps path too long, skipping: %s\n", filename);
                    remove(temp_path);
                    continue;
                }
                
                char mapsDir[MAX_PATH_LEN];
                int dir_result = snprintf(mapsDir, sizeof(mapsDir), "%s/maps", baseDir);
                if (dir_result < sizeof(mapsDir)) {
                    create_dir(mapsDir);
                }
            }
            
            if (rename(temp_path, actualPath) == 0) {
                remove_temp_file_from_cleanup(temp_path);
                log_printf("valid txt file moved: %s\n", actualPath);
            } else {
                log_printf("failed to move txt file: %s, copying instead\n", temp_path);
                FILE* src = fopen(temp_path, "rb");
                FILE* dst = fopen(actualPath, "wb");
                if (src && dst) {
                    char buffer[65536];  // Bigger buffer for faster copying
                    size_t bytes;
                    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        if (fwrite(buffer, 1, bytes, dst) != bytes) {
                            log_printf("warning: write error during copy of %s\n", temp_path);
                            break;
                        }
                    }
                    if (ferror(src)) {
                        log_printf("warning: read error during copy of %s\n", temp_path);
                    }
                    fclose(src);
                    fclose(dst);
                    log_printf("copied txt file: %s\n", actualPath);
                } else {
                    log_printf("failed to open files for copying %s\n", temp_path);
                    if (src) fclose(src);
                    if (dst) fclose(dst);
                }
                remove(temp_path);
                remove_temp_file_from_cleanup(temp_path);
            }
        } else {
            const char* filename = get_filename(original_pathname);
            int result = snprintf(actualPath, sizeof(actualPath), "%s/junk/%s", baseDir, filename);
            
            if (result >= sizeof(actualPath)) {
                log_printf("ERROR: Junk path too long, skipping: %s\n", filename);
                remove(temp_path);
                continue;
            }
            
            char junkDir[MAX_PATH_LEN];
            int dir_result = snprintf(junkDir, sizeof(junkDir), "%s/junk", baseDir);
            if (dir_result < sizeof(junkDir)) {
                create_dir(junkDir);
            }
            
            if (delete_junk) {
                remove(temp_path);
                remove_temp_file_from_cleanup(temp_path);
                log_printf("txt file deleted (junk): %s\n", filename);
                (*junked)++;
            } else {
                if (rename(temp_path, actualPath) == 0) {
                    remove_temp_file_from_cleanup(temp_path);
                    log_printf("txt file junked: %s\n", actualPath);
                    (*junked)++;
                } else {
                    log_printf("failed to move txt file to junk: %s, copying instead\n", temp_path);
                    FILE* src = fopen(temp_path, "rb");
                    FILE* dst = fopen(actualPath, "wb");
                    if (src && dst) {
                        char buffer[65536];  // Bigger buffer for faster copying
                        size_t bytes;
                        while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                            fwrite(buffer, 1, bytes, dst);
                        }
                        fclose(src);
                        fclose(dst);
                        log_printf("copied txt file to junk: %s\n", actualPath);
                        (*junked)++;
                    } else {
                        if (src) fclose(src);
                        if (dst) fclose(dst);
                    }
                    remove(temp_path);
                    remove_temp_file_from_cleanup(temp_path);
                }
            }
        }
    }
}

int extract_archive(const char* archive_path, const char* baseDir, int skip_addons, int delete_junk) {
    if (!validate_path_length(archive_path, "archive path") || 
        !validate_path_length(baseDir, "base directory")) return -1;
    
    FILE* test = fopen(archive_path, "r");
    if (!test) {
        log_printf("cannot open archive file: %s\n", archive_path);
        return -1;
    }
    fclose(test);

    const char* archive_type = detect_archive_type(archive_path);
    log_printf("\n>>> processing %s (%s)\n", archive_path, archive_type);

    struct archive *a;
    struct archive_entry *entry;
    int r;

    a = archive_read_new();
    if (!a) {
        log_printf("failed to create archive object\n");
        return -1;
    }
    
    global_archive = a;
    
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    
    r = archive_read_open_filename(a, archive_path, 10240);
    if (r != ARCHIVE_OK) {
        log_printf("failed to open archive: %s\n", archive_error_string(a));
        archive_read_free(a);
        global_archive = NULL;
        return -1;
    }

    bsp_count = 0;
    pending_txt_count = 0;
    
    int count = 0;
    int extracted = 0;
    int junked = 0;
    int skipped = 0;
    log_printf("extracting files...\n");

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* pathname = archive_entry_pathname(entry);
        la_int64_t size = archive_entry_size(entry);
        
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            count++;
            continue;
        }

        if (!validate_path_length(pathname, "archive entry path")) {
            archive_read_data_skip(a);
            skipped++;
            count++;
            continue;
        }
        
        if (!is_safe_path(pathname)) {
            log_printf("SECURITY: Dangerous path blocked: %s\n", pathname);
            archive_read_data_skip(a);
            skipped++;
            count++;
            continue;
        }
        
        char safe_pathname[MAX_PATH_LEN];
        sanitize_path(pathname, safe_pathname, sizeof(safe_pathname));

        log_printf("%s\n", safe_pathname);

        char target_dir[256];
        get_target_directory(get_filename(safe_pathname), safe_pathname, target_dir, sizeof(target_dir), skip_addons);
        
        if (strcmp(target_dir, "SKIP") == 0) {
            log_printf("skipped (addons): %s\n", safe_pathname);
            archive_read_data_skip(a);
            skipped++;
            count++;
            continue;
        }
        
        char finalPath[MAX_PATH_LEN];
        
        if (strcmp(target_dir, "TXT_CHECK") == 0) {
            if (!generate_unique_temp_filename(baseDir, safe_pathname, finalPath, sizeof(finalPath))) {
                log_printf("ERROR: Could not generate unique temp filename for: %s\n", safe_pathname);
                archive_read_data_skip(a);
                skipped++;
                count++;
                continue;
            }
        }
        else if (strcmp(target_dir, "BASEDIR") == 0) {
            const char* filename = get_filename(safe_pathname);
            int result = snprintf(finalPath, sizeof(finalPath), "%s/%s", baseDir, filename);
            if (result >= sizeof(finalPath)) {
                log_printf("ERROR: Base path too long, skipping: %s\n", safe_pathname);
                archive_read_data_skip(a);
                skipped++;
                count++;
                continue;
            }
        }
        else if (strcmp(target_dir, "JUNK_PRESERVE") == 0) {
            int result = snprintf(finalPath, sizeof(finalPath), "%s/junk/%s", baseDir, safe_pathname);
            if (result >= sizeof(finalPath)) {
                log_printf("ERROR: Junk preserve path too long, skipping: %s\n", safe_pathname);
                archive_read_data_skip(a);
                skipped++;
                count++;
                continue;
            }
        }
        else if (strcmp(target_dir, "ADDONS_PRESERVE") == 0) {
            const char* addons_pos = strstr(safe_pathname, "addons/");
            if (!addons_pos) addons_pos = strstr(safe_pathname, "addons\\");
            int result;
            if (addons_pos) {
                const char* after_addons = addons_pos + 7;
                result = snprintf(finalPath, sizeof(finalPath), "%s/__addons/%s", baseDir, after_addons);
            } else {
                result = snprintf(finalPath, sizeof(finalPath), "%s/__addons/%s", baseDir, safe_pathname);
            }
            if (result >= sizeof(finalPath)) {
                log_printf("ERROR: Addons path too long, skipping: %s\n", safe_pathname);
                archive_read_data_skip(a);
                skipped++;
                count++;
                continue;
            }
        }
        else if (path_has_correct_structure(safe_pathname, target_dir)) {
            int result = snprintf(finalPath, sizeof(finalPath), "%s/%s", baseDir, safe_pathname);
            if (result >= sizeof(finalPath)) {
                log_printf("ERROR: Structured path too long, skipping: %s\n", safe_pathname);
                archive_read_data_skip(a);
                skipped++;
                count++;
                continue;
            }
        }
        else {
            const char* filename = get_filename(safe_pathname);
            int result = snprintf(finalPath, sizeof(finalPath), "%s/%s/%s", baseDir, target_dir, filename);
            if (result >= sizeof(finalPath)) {
                log_printf("ERROR: Target path too long, skipping: %s\n", safe_pathname);
                archive_read_data_skip(a);
                skipped++;
                count++;
                continue;
            }
        }
        
        if (!validate_path_length(finalPath, "final extraction path")) {
            archive_read_data_skip(a);
            skipped++;
            count++;
            continue;
        }
        
        int will_be_junked = (strcmp(target_dir, "junk") == 0);
        
        create_dirs_recursive(finalPath);
        
        FILE* output = fopen(finalPath, "wb");
        if (output == NULL) {
            log_printf("failed to create file: %s - %s\n", finalPath, strerror(errno));
            archive_read_data_skip(a);
            count++;
            continue;
        }
        
        global_output_file = output;

        const void *buff;
        size_t buff_size;
        la_int64_t offset;
        la_int64_t total_read = 0;
        int data_error = 0;
        
        while ((r = archive_read_data_block(a, &buff, &buff_size, &offset)) == ARCHIVE_OK) {
            fwrite(buff, 1, buff_size, output);
            total_read += buff_size;
        }
        
        if (r != ARCHIVE_EOF) {
            data_error = 1;
            log_printf("WARNING: Data corruption detected in %s (error: %s)\n", 
                      pathname, archive_error_string(a));
        }
        
        if (size >= 0 && total_read != size) {
            data_error = 1;
            log_printf("WARNING: Size mismatch in %s (expected: %lld, got: %lld)\n", 
                      pathname, (long long)size, (long long)total_read);
        }

        fclose(output);
        global_output_file = NULL;
        
        if (data_error) {
            log_printf("CORRUPTED FILE REMOVED: %s\n", finalPath);
            remove(finalPath);
            count++;
            continue;
        }

        if (strcmp(target_dir, "TXT_CHECK") == 0) {
            add_temp_file_for_cleanup(finalPath);
            add_pending_txt(finalPath, safe_pathname);
        } else {
            log_printf("%s\n", finalPath);
            if (will_be_junked) {
                if (delete_junk) {
                    remove(finalPath);
                    log_printf("deleted (junk): %s\n", get_filename(safe_pathname));
                    junked++;
                } else {
                    junked++;
                }
            }
        }

        const char* ext = get_extension(get_filename(safe_pathname));
        char ext_lower[16];
        strncpy(ext_lower, ext, sizeof(ext_lower) - 1);
        ext_lower[sizeof(ext_lower) - 1] = '\0';
        to_lowercase(ext_lower);
        
        if (strcmp(ext_lower, "bsp") == 0) {
            add_bsp_file(safe_pathname);
            log_printf("%s\n", get_filename(safe_pathname));
        }

        extracted++;
        count++;
    }
    
    if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
        log_printf("WARNING: Archive may be corrupted - stopped reading at entry %d (error: %s)\n", 
                  count, archive_error_string(a));
    }

    r = archive_read_free(a);
    global_archive = NULL;
    if (r != ARCHIVE_OK) {
        log_printf("warning: error closing archive: %s\n", archive_error_string(a));
    }

    process_pending_txt_files(baseDir, &junked, delete_junk);

    log_printf("archive complete: %d files processed, %d files extracted, %d files junked", count, extracted, junked);
    if (skipped > 0) {
        log_printf(", %d files skipped", skipped);
    }
    log_printf(" from %s\n", archive_path);
    total_junked += junked;  
    return extracted;
}

#ifdef _WIN32
int process_directory(const char* dir_path, const char* baseDir, int skip_addons, int delete_junk, int delete_archives) {
    if (!validate_path_length(dir_path, "directory path") || 
        !validate_path_length(baseDir, "base directory")) return -1;
    
    WIN32_FIND_DATA findFileData;
    HANDLE hFind;
    char searchPath[MAX_PATH_LEN];
    int total_extracted = 0;
    int archives_processed = 0;
    
    int result = snprintf(searchPath, sizeof(searchPath), "%s\\*", dir_path);
    if (result >= sizeof(searchPath)) {
        log_printf("ERROR: Search path too long: %s\n", dir_path);
        return -1;
    }
    
    hFind = FindFirstFile(searchPath, &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        log_printf("failed to open directory: %s\n", dir_path);
        return -1;
    }
    
    do {
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0) {
            continue;
        }
        
        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        
        if (is_supported_archive(findFileData.cFileName)) {
            char fullPath[MAX_PATH_LEN];
            int result = snprintf(fullPath, sizeof(fullPath), "%s\\%s", dir_path, findFileData.cFileName);
            
            if (result >= sizeof(fullPath)) {
                log_printf("ERROR: Archive path too long, skipping: %s\n", findFileData.cFileName);
            } else {
                int extract_result = extract_archive(fullPath, baseDir, skip_addons, delete_junk);
                if (extract_result >= 0) {
                    total_extracted += extract_result;
                    archives_processed++;
                    if (delete_archives) {
                        add_processed_archive(fullPath);
                    }
                }
            }
        } else {
            log_printf("skipping non-archive file: %s\n", findFileData.cFileName);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);
    
    FindClose(hFind);
    
    log_printf("\n>>> directory processing complete\n");
    log_printf("archives processed: %d\n", archives_processed);
    log_printf("total files extracted: %d\n", total_extracted);
    return total_extracted;
}
#else
int process_directory(const char* dir_path, const char* baseDir, int skip_addons, int delete_junk, int delete_archives) {
    if (!validate_path_length(dir_path, "directory path") || 
        !validate_path_length(baseDir, "base directory")) return -1;
    
    DIR* dir;
    struct dirent* entry;
    int total_extracted = 0;
    int archives_processed = 0;
    
    dir = opendir(dir_path);
    if (dir == NULL) {
        log_printf("failed to open directory: %s\n", dir_path);
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char fullPath[MAX_PATH_LEN];
        int result = snprintf(fullPath, sizeof(fullPath), "%s/%s", dir_path, entry->d_name);
        
        if (result >= sizeof(fullPath)) {
            log_printf("ERROR: Archive path too long, skipping: %s\n", entry->d_name);
            continue;
        }
        
        struct stat file_stat;
        if (stat(fullPath, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
            if (is_supported_archive(entry->d_name)) {
                int extract_result = extract_archive(fullPath, baseDir, skip_addons, delete_junk);
                if (extract_result >= 0) {
                    total_extracted += extract_result;
                    archives_processed++;
                    if (delete_archives) {
                        add_processed_archive(fullPath);
                    }
                }
            } else {
                log_printf("skipping non-archive file: %s\n", entry->d_name);
            }
        }
    }
    
    closedir(dir);
    
    log_printf("\n>>> directory processing complete\n");
    log_printf("archives processed: %d\n", archives_processed);
    log_printf("total files extracted: %d\n", total_extracted);
    return total_extracted;
}
#endif

int is_directory(const char* path) {
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        return 0;
    }
    return S_ISDIR(path_stat.st_mode);
}

int main(int argc, char* argv[]) {
    setup_signal_handlers();
    
    clock_t start_time = clock();  
    int enable_logging = 0;
    int delete_junk = 0;
    int skip_addons = 0;
    int extract_to_exe = 0;
    int delete_archives = 0;
    int arg_start = 1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            enable_logging = 1;
        }
        else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--junk") == 0) {
            delete_junk = 1;
        }
        else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--addons") == 0) {
            skip_addons = 1;
        }
        else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--executable") == 0) {
            extract_to_exe = 1;
        }
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delete") == 0) {
            delete_archives = 1;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("welcome to mapXtract!\n");
            printf("-l, --log       write timestamped log file\n");
            printf("-j, --junk      delete junk files (default: save to junk folder)\n");
            printf("-a, --addons    ignore addons\\ directory (default: save to __addons\\)\n");
            printf("-e, --executable extract to exe dir (default: archive dir)\n");
            printf("-d, --delete    delete archives after successful extraction\n");
            printf("-h, --help      show this message\n");
            return 0;
        }
        else {
            arg_start = i;
            break;
        }
    }
    
    if (argc < arg_start + 1) {
        printf("usage: %s [-l|--log] [-j|--junk] [-a|--addons] [-e|--executable] [-d|--delete] <archive_file(s)> OR <directory>\n", argv[0]);
        printf("supported formats: ZIP, RAR, 7Z\n");
        printf("options:\n");
        printf("  -l, --log         write timestamped log file\n");
        printf("  -j, --junk        delete junk files after extraction\n");
        printf("  -a, --addons      skip files from /addons/ directories\n");
        printf("  -e, --executable  extract to executable directory instead of archive location\n");
        printf("  -d, --delete      delete archives after successful extraction\n");
        printf("examples:\n");
        printf("  %s map1.zip map2.rar map3.7z    # extract multiple archives\n", argv[0]);
        printf("  %s /path/to/archives/           # extract all archives in directory\n", argv[0]);
        printf("  %s -l map1.zip                  # extract with logging\n", argv[0]);
        printf("  %s -j -a map1.zip               # skip addons and delete junk\n", argv[0]);
        printf("  %s -e map1.zip                  # extract to executable directory\n", argv[0]);
        printf("  %s -d map1.zip                  # extract and delete archive when done\n", argv[0]);
        printf("  %s -d -j -a map1.zip            # extract, delete junk, skip addons, delete archive\n", argv[0]);
        return -1;
    }

    char first_archive_dir[MAX_PATH_LEN];
    char full_base_dir[MAX_PATH_LEN];
    
    if (extract_to_exe) {
#ifdef _WIN32
        GetModuleFileName(NULL, first_archive_dir, sizeof(first_archive_dir));
        char* last_slash = strrchr(first_archive_dir, '\\');
        if (last_slash) *last_slash = '\0';
#else
        strncpy(first_archive_dir, argv[0], sizeof(first_archive_dir) - 1);
        first_archive_dir[sizeof(first_archive_dir) - 1] = '\0';
        char* last_slash = strrchr(first_archive_dir, '/');
        if (last_slash) *last_slash = '\0';
        else strcpy(first_archive_dir, ".");
#endif
    } else {
        if (is_directory(argv[arg_start])) {
            strncpy(first_archive_dir, argv[arg_start], sizeof(first_archive_dir) - 1);
            first_archive_dir[sizeof(first_archive_dir) - 1] = '\0';
        } else {
            strncpy(first_archive_dir, argv[arg_start], sizeof(first_archive_dir) - 1);
            first_archive_dir[sizeof(first_archive_dir) - 1] = '\0';
            
            char* last_slash = strrchr(first_archive_dir, PATH_SEP);
            if (last_slash) {
                *last_slash = '\0';
            } else {
                strcpy(first_archive_dir, ".");
            }
        }
    }
    
    snprintf(full_base_dir, sizeof(full_base_dir), "%s%ccstrike", first_archive_dir, PATH_SEP);

    if (!validate_path_length(full_base_dir, "base extraction directory")) {
        log_printf("ERROR: Base directory path too long\n");
        return -1;
    }

    if (enable_logging) {
        time_t rawtime;
        struct tm * timeinfo;
        char timestamp[80];
        char log_filename[MAX_PATH_LEN];
        
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        
        char readable_timestamp[80];
        strftime(timestamp, sizeof(timestamp), "%Y_%m%d_%H%M-%S", timeinfo);
        strftime(readable_timestamp, sizeof(readable_timestamp), "%Y.%m.%d %H:%M:%S", timeinfo);
        
        int result = snprintf(log_filename, sizeof(log_filename), "%s%cmapXtract_%s.log", first_archive_dir, PATH_SEP, timestamp);
        
        if (result >= sizeof(log_filename)) {
            printf("warning: log file path too long, logging disabled\n");
        } else {
            log_file = fopen(log_filename, "w");
            if (log_file) {
                log_printf(">>> log file created\n>>> %s\n\n", readable_timestamp);
            } else {
                printf("warning: failed to create log file %s\n", log_filename);
            }
        }
    }

    const char *baseDir = full_base_dir;
    create_dir(baseDir);

    int total_extracted = 0;
    int total_archives = 0;

    log_printf("extracting to %s%c\n", baseDir, PATH_SEP);

    for (int i = arg_start; i < argc; i++) {
        if (!validate_path_length(argv[i], "input argument")) {
            log_printf("skipping argument with path too long: %s\n", argv[i]);
            continue;
        }
        
        if (is_directory(argv[i])) {
            log_printf("\n>>> processing directory: %s\n", argv[i]);
            int result = process_directory(argv[i], baseDir, skip_addons, delete_junk, delete_archives);
            if (result >= 0) {
                total_extracted += result;
            }
        } else {
            if (is_supported_archive(argv[i])) {
                int result = extract_archive(argv[i], baseDir, skip_addons, delete_junk);
                if (result >= 0) {
                    total_extracted += result;
                    total_archives++;
                    if (delete_archives) {
                        add_processed_archive(argv[i]);
                    }
                }
            } else {
                log_printf("skipping unsupported file: %s\n", argv[i]);
            }
        }
    }

    if (delete_archives && processed_archive_count > 0) {
        log_printf("\n>>> deleting processed archives\n");
        delete_processed_archives();
    }

    log_printf("\n>>> summary\n");
    log_printf("archives: %d\n", total_archives);
    log_printf("files: %d\n", total_extracted);
    log_printf("junk: %d\n", total_junked);

    if (delete_archives && processed_archive_count > 0) {
        log_printf("deleted: %d\n", processed_archive_count);
    }

    if (delete_junk) {
        char junk_dir[MAX_PATH_LEN];
        int result = snprintf(junk_dir, sizeof(junk_dir), "%s%cjunk", baseDir, PATH_SEP);
        
        if (result < sizeof(junk_dir)) {
            struct stat st;
            if (stat(junk_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
#ifdef _WIN32
                if (RemoveDirectory(junk_dir)) {
                    log_printf("junk folder deleted\n");
                }
#else
                if (rmdir(junk_dir) == 0) {
                    log_printf("junk folder deleted\n");
                }
#endif
            }
        } else {
            log_printf("warning: junk directory path too long for cleanup\n");
        }
    }
    
    clock_t end_time = clock();
    double runtime = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    cleanup_temp_files();
    
    if (log_file) {
        log_printf("\n>>> log file completed in %.2fs", runtime);
        fclose(log_file);
        log_file = NULL;
    }

    return 0;
}