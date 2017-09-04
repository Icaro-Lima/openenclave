#include <stddef.h>
#include <stdlib.h>
#include <openenclave.h>

void *malloc(size_t size)
{
    return OE_Malloc(size);
}

void free(void *ptr)
{
    OE_Free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
    return OE_Calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size)
{
    return OE_Realloc(ptr, size);
}

void *memalign(size_t alignment, size_t size)
{
    return OE_Memalign(alignment, size);
}