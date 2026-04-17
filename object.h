#ifndef OBJECT_H
#define OBJECT_H

#include <stddef.h>
#include "pes.h" // THIS MUST BE HERE

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
// ... other functions ...

#endif
