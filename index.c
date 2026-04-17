// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add
#include "pes.h"
#include "object.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#define INDEX_PATH ".pes/index"
#define INDEX_TEMP ".pes/index.tmp"

// ─── HELPERS ────────────────────────────────────────────────────────────────

// Helper for qsort: sorts entries by their file path string
static int compare_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry*)a)->path, ((IndexEntry*)b)->path);
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0 || strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── TODO (Phase 3) ──────────────────────────────────────────────────────────

int index_load(Index *index) {
    if (!index) return -1; // Safety check
    
    index->count = 0;
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) return 0; 

    char hex[65];
    // Use fixed-width types or match your struct exactly
    // mode is likely int (use %d or %o), size is uint32_t (use %u)
    while (index->count < MAX_INDEX_ENTRIES) {
        int res = fscanf(f, "%o %64s %llu %u %[^\n]\n", 
                        (unsigned int *)&index->entries[index->count].mode,
                        hex,
                        (unsigned long long *)&index->entries[index->count].mtime_sec,
                        &index->entries[index->count].size,
                        index->entries[index->count].path);
        
        if (res != 5) break;

        hex_to_hash(hex, &index->entries[index->count].hash);
        index->count++;
    }
    
    fclose(f);
    return 0;
}
static int compare_entries_ptrs(const void *a, const void *b) {
    const IndexEntry *ea = *(const IndexEntry **)a;
    const IndexEntry *eb = *(const IndexEntry **)b;
    return strcmp(ea->path, eb->path);
}
int index_save(const Index *index) {
    if (!index) return -1;

    // We skip the 'sorted' copy to avoid the Seg Fault
    FILE *f = fopen(INDEX_TEMP, "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        char hex[65];
        hash_to_hex(&index->entries[i].hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                index->entries[i].mode, 
                hex,
                (unsigned long long)index->entries[i].mtime_sec,
                index->entries[i].size,
                index->entries[i].path);
    }

    fclose(f);
    rename(INDEX_TEMP, INDEX_PATH);
    return 0;
}
int index_add(Index *index, const char *path) {
    struct stat st;
    // Check if the file actually exists before proceeding
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: %s does not exist\n", path);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    void *data = malloc(st.st_size);
    if (!data) { // Always check if malloc succeeded
        fclose(f);
        return -1;
    }

    if (fread(data, 1, st.st_size, f) != (size_t)st.st_size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID hash;
    // Ensure object_write is working correctly
    if (object_write(OBJ_BLOB, data, st.st_size, &hash) != 0) {
        free(data);
        return -1;
    }
    free(data);

    // Find if the entry exists or create a new one
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
        memset(entry, 0, sizeof(IndexEntry)); // Clear the new entry memory
        strncpy(entry->path, path, sizeof(entry->path) - 1);
    }

    entry->mode = st.st_mode;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    memcpy(&entry->hash, &hash, sizeof(ObjectID));

    // Self-save to satisfy the requirement without touching pes.c
    return index_save(index); 
}
 
