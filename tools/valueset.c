#include "valueset.h"
#include <stdlib.h>
#include <string.h>

value_set *inf_get_valueset(PTR in_v, uint32_t length)
{
    value_set *res = (value_set *)malloc(sizeof(value_set));
    uint32_t length_in_bytes = length;
    res->length_in_bytes = length_in_bytes;
    length = (length / PIECE + (length % PIECE ? 1 : 0)) * PIECE;
    res->value = (PTR)malloc(length_in_bytes);
    res->length = length;

    if (in_v)
    {
        memcpy(res->value, in_v, length_in_bytes < length ? length_in_bytes : length);
    }
    else
    {
        memset(res->value, 0, length_in_bytes < length ? length_in_bytes : length);
    }
    return res;
}

void inf_free_valueset(value_set **in)
{
    free((*in)->value);
    free((*in));
    *in = NULL;
}