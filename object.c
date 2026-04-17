// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include "object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
static int ensure_parent_dir(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp));
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    return mkdir(tmp, 0755); /* returns -1 if exists, that's fine */
}
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_ou HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    /* build header: "blob 42\0", "tree 42\0", or "commit 42\0" */
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;
    /* +1 includes the '\0' terminator that snprintf writes */

    /* combine header + data into one buffer */
    size_t total_len = (size_t)header_len + len;
    unsigned char *full = malloc(total_len);
    if (!full) return -1;
    memcpy(full, header, header_len);
    memcpy(full + header_len, data, len);

    /* compute hash and fill id_out */
    compute_hash(full, total_len, id_out);

    /* get final path and ensure shard dir exists */
    char path[512];
    object_path(id_out, path, sizeof(path));
    ensure_parent_dir(path);

    /* if object already exists, skip writing (deduplication) */
    if (access(path, F_OK) == 0) {
        free(full);
        return 0;
    }

    /* write to a temp file first, then rename (atomic) */
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(full); return -1; }

    if (write(fd, full, total_len) != (ssize_t)total_len) {
        close(fd); free(full);
        unlink(tmp_path);
        return -1;
    }

    fsync(fd);
    close(fd);
    free(full);

    /* atomic rename: tmp -> final path */
    if (rename(tmp_path, path) < 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}


// copy has
// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    /* get path from id */
    char path[512];
    object_path(id, path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /* read entire file */
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }

    unsigned char *full = malloc((size_t)st.st_size);
    if (!full) { close(fd); return -1; }

    if (read(fd, full, (size_t)st.st_size) != st.st_size) {
        close(fd); free(full); return -1;
    }
    close(fd);

    /* verify integrity: recompute hash and compare to filename */
    ObjectID actual;
    compute_hash(full, (size_t)st.st_size, &actual);
    if (memcmp(actual.hash, id->hash, HASH_SIZE) != 0) {
        free(full);
        return -1; /* corrupted object */
    }

    /* parse header: "blob 42\0..." or "tree 42\0..." or "commit 42\0..." */
    char *header = (char *)full;
    char *null_byte = memchr(full, '\0', (size_t)st.st_size);
    if (!null_byte) { free(full); return -1; }

    size_t header_len = (size_t)(null_byte - header) + 1; /* include '\0' */

    /* parse type string */
    char type_str[16];
    size_t data_len;
    if (sscanf(header, "%15s %zu", type_str, &data_len) != 2) {
        free(full); return -1;
    }

    if      (strcmp(type_str, "blob")   == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree")   == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(full); return -1; }

    /* copy just the data portion (after the '\0') into a new buffer */
    *len_out = data_len;
    *data_out = malloc(data_len);
    if (!*data_out) { free(full); return -1; }
    memcpy(*data_out, full + header_len, data_len);

    free(full);
    return 0;
}
