/* The list is circular through its head: the head's next is the front
 * element, its previous the back element, and both are the head itself when
 * the list is empty. No operation has a special case for an end. */
#include "badger/list.h"

#include "badger/assert.h"
#include "exclusive.h"

#define TB_LIST_KIND "a list"

/* Joins a link in between two neighbors that are next to each other. */
static void tb_list_link_between(tb_list_link *link, tb_list_link *previous, tb_list_link *next)
{
    TB_ASSERT(link != NULL);
    TB_ASSERT_MSG(link->next == NULL && link->previous == NULL,
                  "the link must not be in a list already");
    link->previous = previous;
    link->next = next;
    previous->next = link;
    next->previous = link;
}

/* A list that was only zero-initialized has no links at all. One that was
 * initialized always has both, so one of them tells the two apart. */
static bool tb_list_is_initialized(const tb_list *list)
{
    return list->head.next != NULL;
}

/* Empties the list, leaving its check alone: a list emptied by another of
 * its functions is still inside that function's guard. */
static void tb_list_empty_links(tb_list *list)
{
    list->head.next = &list->head;
    list->head.previous = &list->head;
}

void tb_list_init(tb_list *list)
{
    const tb_exclusive_check unused = TB_EXCLUSIVE_CHECK_INIT;

    TB_ASSERT(list != NULL);
    tb_list_empty_links(list);
    list->check = unused;
}

/* ---------------------------------------------------------------------------
 * Adding and removing.
 * ------------------------------------------------------------------------- */

static void tb_list_push_front_inside(tb_list *list, tb_list_link *link)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    tb_list_link_between(link, &list->head, list->head.next);
}

void tb_list_push_front(tb_list *list, tb_list_link *link)
{
    TB_ASSERT(list != NULL);
    TB_EXCLUSIVE_ENTER(&list->check, TB_LIST_KIND);
    tb_list_push_front_inside(list, link);
    TB_EXCLUSIVE_LEAVE(&list->check);
}

static void tb_list_push_back_inside(tb_list *list, tb_list_link *link)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    tb_list_link_between(link, list->head.previous, &list->head);
}

void tb_list_push_back(tb_list *list, tb_list_link *link)
{
    TB_ASSERT(list != NULL);
    TB_EXCLUSIVE_ENTER(&list->check, TB_LIST_KIND);
    tb_list_push_back_inside(list, link);
    TB_EXCLUSIVE_LEAVE(&list->check);
}

void tb_list_insert_before(tb_list_link *position, tb_list_link *link)
{
    TB_ASSERT(position != NULL);
    TB_ASSERT_MSG(tb_list_link_is_linked(position), "the position must be in a list");
    tb_list_link_between(link, position->previous, position);
}

void tb_list_insert_after(tb_list_link *position, tb_list_link *link)
{
    TB_ASSERT(position != NULL);
    TB_ASSERT_MSG(tb_list_link_is_linked(position), "the position must be in a list");
    tb_list_link_between(link, position, position->next);
}

void tb_list_remove(tb_list_link *link)
{
    TB_ASSERT(link != NULL);
    TB_ASSERT_MSG(tb_list_link_is_linked(link), "the link must be in a list");
    link->previous->next = link->next;
    link->next->previous = link->previous;
    link->next = NULL;
    link->previous = NULL;
}

static tb_list_link *tb_list_pop_front_inside(tb_list *list)
{
    tb_list_link *const link = tb_list_first(list);

    if (link != NULL)
    {
        tb_list_remove(link);
    }
    return link;
}

tb_list_link *tb_list_pop_front(tb_list *list)
{
    tb_list_link *result = NULL;
    TB_ASSERT(list != NULL);
    TB_EXCLUSIVE_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_pop_front_inside(list);
    TB_EXCLUSIVE_LEAVE(&list->check);
    return result;
}

static tb_list_link *tb_list_pop_back_inside(tb_list *list)
{
    tb_list_link *const link = tb_list_last(list);

    if (link != NULL)
    {
        tb_list_remove(link);
    }
    return link;
}

tb_list_link *tb_list_pop_back(tb_list *list)
{
    tb_list_link *result = NULL;
    TB_ASSERT(list != NULL);
    TB_EXCLUSIVE_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_pop_back_inside(list);
    TB_EXCLUSIVE_LEAVE(&list->check);
    return result;
}

bool tb_list_link_is_linked(const tb_list_link *link)
{
    TB_ASSERT(link != NULL);
    return link->next != NULL;
}

/* ---------------------------------------------------------------------------
 * Walking a list.
 * ------------------------------------------------------------------------- */

static bool tb_list_is_empty_reading(const tb_list *list)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    return list->head.next == &list->head;
}

bool tb_list_is_empty(const tb_list *list)
{
    bool result = false;
    TB_ASSERT(list != NULL);
    const size_t list_generation = TB_EXCLUSIVE_READ_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_is_empty_reading(list);
    TB_EXCLUSIVE_READ_LEAVE(&list->check, list_generation, TB_LIST_KIND);
    return result;
}

/* A neighbor of a link as an element: NULL when it is the head, which is not
 * an element. */
static tb_list_link *tb_list_element(const tb_list *list, tb_list_link *neighbor)
{
    if (neighbor == &list->head)
    {
        return NULL;
    }
    return neighbor;
}

static tb_list_link *tb_list_first_reading(const tb_list *list)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    return tb_list_element(list, list->head.next);
}

tb_list_link *tb_list_first(const tb_list *list)
{
    tb_list_link *result = NULL;
    TB_ASSERT(list != NULL);
    const size_t list_generation = TB_EXCLUSIVE_READ_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_first_reading(list);
    TB_EXCLUSIVE_READ_LEAVE(&list->check, list_generation, TB_LIST_KIND);
    return result;
}

static tb_list_link *tb_list_last_reading(const tb_list *list)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    return tb_list_element(list, list->head.previous);
}

tb_list_link *tb_list_last(const tb_list *list)
{
    tb_list_link *result = NULL;
    TB_ASSERT(list != NULL);
    const size_t list_generation = TB_EXCLUSIVE_READ_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_last_reading(list);
    TB_EXCLUSIVE_READ_LEAVE(&list->check, list_generation, TB_LIST_KIND);
    return result;
}

static tb_list_link *tb_list_next_reading(const tb_list *list, const tb_list_link *link)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT(link != NULL);
    TB_ASSERT_MSG(tb_list_link_is_linked(link), "the link must be in a list");
    return tb_list_element(list, link->next);
}

tb_list_link *tb_list_next(const tb_list *list, const tb_list_link *link)
{
    tb_list_link *result = NULL;
    TB_ASSERT(list != NULL);
    const size_t list_generation = TB_EXCLUSIVE_READ_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_next_reading(list, link);
    TB_EXCLUSIVE_READ_LEAVE(&list->check, list_generation, TB_LIST_KIND);
    return result;
}

static tb_list_link *tb_list_previous_reading(const tb_list *list, const tb_list_link *link)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT(link != NULL);
    TB_ASSERT_MSG(tb_list_link_is_linked(link), "the link must be in a list");
    return tb_list_element(list, link->previous);
}

tb_list_link *tb_list_previous(const tb_list *list, const tb_list_link *link)
{
    tb_list_link *result = NULL;
    TB_ASSERT(list != NULL);
    const size_t list_generation = TB_EXCLUSIVE_READ_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_previous_reading(list, link);
    TB_EXCLUSIVE_READ_LEAVE(&list->check, list_generation, TB_LIST_KIND);
    return result;
}

size_t tb_list_length(const tb_list *list)
{
    return tb_list_to_array(list, NULL, 0);
}

static size_t tb_list_to_array_reading(const tb_list *list, tb_list_link **links, size_t capacity)
{
    size_t count = {0};

    TB_ASSERT(links != NULL || capacity == 0);
    for (tb_list_link *link = tb_list_first_reading(list); link != NULL;
         link = tb_list_next_reading(list, link))
    {
        if (count < capacity)
        {
            links[count] = link;
        }
        count++;
    }
    return count;
}

size_t tb_list_to_array(const tb_list *list, tb_list_link **links, size_t capacity)
{
    size_t result = 0;
    TB_ASSERT(list != NULL);
    const size_t list_generation = TB_EXCLUSIVE_READ_ENTER(&list->check, TB_LIST_KIND);
    result = tb_list_to_array_reading(list, links, capacity);
    TB_EXCLUSIVE_READ_LEAVE(&list->check, list_generation, TB_LIST_KIND);
    return result;
}

/* ---------------------------------------------------------------------------
 * Whole lists.
 * ------------------------------------------------------------------------- */

static void tb_list_append_inside(tb_list *list, tb_list *other)
{
    tb_list_link *first = NULL;
    tb_list_link *last = NULL;

    TB_ASSERT(list != NULL);
    TB_ASSERT(other != NULL);
    TB_ASSERT_MSG(list != other, "a list cannot be appended to itself");
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    if (tb_list_is_empty(other))
    {
        return;
    }
    first = other->head.next;
    last = other->head.previous;
    first->previous = list->head.previous;
    list->head.previous->next = first;
    last->next = &list->head;
    list->head.previous = last;
    tb_list_empty_links(other);
}

void tb_list_append(tb_list *list, tb_list *other)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT(other != NULL);
    TB_EXCLUSIVE_ENTER(&list->check, TB_LIST_KIND);
    TB_EXCLUSIVE_ENTER(&other->check, TB_LIST_KIND);
    tb_list_append_inside(list, other);
    TB_EXCLUSIVE_LEAVE(&other->check);
    TB_EXCLUSIVE_LEAVE(&list->check);
}

static void tb_list_reverse_inside(tb_list *list)
{
    tb_list_link *link = NULL;

    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(tb_list_is_initialized(list), "the list must be initialized");
    /* Swapping the two pointers of every link, the head included, reverses
     * the direction of the whole circle. */
    link = &list->head;
    do
    {
        tb_list_link *const next = link->next;

        link->next = link->previous;
        link->previous = next;
        link = next;
    } while (link != &list->head);
}

void tb_list_reverse(tb_list *list)
{
    TB_ASSERT(list != NULL);
    TB_EXCLUSIVE_ENTER(&list->check, TB_LIST_KIND);
    tb_list_reverse_inside(list);
    TB_EXCLUSIVE_LEAVE(&list->check);
}
