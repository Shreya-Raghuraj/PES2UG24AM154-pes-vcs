#include "object.h"
#include "tree.h"
#include <stdio.h>
#include <string.h>

int main() {
    // Manually create a fake tree buffer to force a tree object write
    char *fake_tree_data = "tree 30\0file1.txt\0hash12345678901234567890";
    ObjectID hash;
    
    // This calls your object_write function directly
    if (object_write(OBJ_TREE, fake_tree_data, 30, &hash) == 0) {
        char hex[65];
        hash_to_hex(&hash, hex);
        printf("SUCCESS! Tree object created with hash: %s\n", hex);
    } else {
        printf("FAILED! Check if .pes/objects folder exists.\n");
    }
    return 0;
}
