#ifndef VALUESET_H
#define VALUESET_H

#include "../hash_baseline/dftl_types.h"

value_set *inf_get_valueset(PTR in_v, uint32_t length);
void inf_free_valueset(value_set **in);

#endif //VALUESET_H