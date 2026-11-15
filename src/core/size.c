#include "badger/size.h"

#include "badger/assert.h"

#include <limits.h>
#include <stdint.h>

/* Half the width of size_t, in bits. Two factors that both fit in half the
 * width have a product that fits in the whole of it. */
#define TB_SIZE_HALF_WIDTH (sizeof(size_t) * CHAR_BIT / 2)

/* Each capacity increase multiplies the capacity by this. */
#define TB_SIZE_GROWTH_FACTOR ((size_t)2)

bool tb_size_add(size_t left, size_t right, size_t *result)
{
    TB_ASSERT(result != NULL);
    if (left > SIZE_MAX - right)
    {
        return false;
    }
    *result = left + right;
    return true;
}

bool tb_size_multiply(size_t left, size_t right, size_t *result)
{
    /* Most products are of small factors. Checking for that first keeps the
     * division off the common path. */
    const bool is_factor_wide = ((left | right) >> TB_SIZE_HALF_WIDTH) != 0;

    TB_ASSERT(result != NULL);
    if (is_factor_wide && left != 0 && right > SIZE_MAX / left)
    {
        return false;
    }
    *result = left * right;
    return true;
}

bool tb_size_grow(size_t capacity, size_t required, size_t element_size, size_t *result)
{
    size_t largest_capacity = {0};
    size_t grown = {0};

    TB_ASSERT(result != NULL);
    if (element_size == 0)
    {
        element_size = 1;
    }
    largest_capacity = SIZE_MAX / element_size;
    TB_ASSERT_MSG(capacity <= largest_capacity, "capacity must be that of a buffer that can exist");
    if (required > largest_capacity)
    {
        return false;
    }
    if (required <= capacity)
    {
        *result = capacity;
        return true;
    }
    grown = largest_capacity;
    if (capacity <= largest_capacity / TB_SIZE_GROWTH_FACTOR)
    {
        grown = capacity * TB_SIZE_GROWTH_FACTOR;
    }
    if (grown < required)
    {
        grown = required;
    }
    *result = grown;
    return true;
}
