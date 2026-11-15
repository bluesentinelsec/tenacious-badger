/** @file list.h
 * @brief An intrusive doubly linked list: the links live inside the elements.
 *
 * An element joins a list by embedding a tb_list_link in its own structure.
 * The list allocates nothing and owns nothing: adding and removing an element
 * rewrites a few pointers, wherever the element is and whoever allocated it.
 * Removal needs only the element, not a search. That is what engine code wants
 * from a list: objects that move between "active" and "free", entities
 * waiting on an event, and other sets whose members come and go.
 *
 * @code
 * typedef struct projectile
 * {
 *     vector position;
 *     tb_list_link link; // membership of the active or the free list
 * } projectile;
 *
 * tb_list active = TB_LIST_INIT(active);
 *
 * tb_list_push_back(&active, &shot->link);
 * for (tb_list_link *link = tb_list_first(&active); link != NULL;
 *      link = tb_list_next(&active, link))
 * {
 *     projectile *const each = TB_LIST_ENTRY(link, projectile, link);
 *     ...
 * }
 * tb_list_remove(&shot->link); // constant time, from wherever it is
 * @endcode
 *
 * **One link, one list.** A link is in at most one list at a time. An element
 * that must be in several lists at once embeds one link for each.
 *
 * **Initialize before use.** A list is made empty with #TB_LIST_INIT or
 * tb_list_init(). A link starts zero-initialized, which means "in no list";
 * tb_list_remove() returns it to that state. Builds with assertions use this
 * to catch a link added twice or removed twice.
 *
 * **Lifetimes.** The list borrows its elements. An element must be removed
 * before its memory is released or reused, and a list must not be moved or
 * copied while it has elements, because its elements point back at it.
 *
 * **When not to use it.** A list visits its elements by following pointers
 * through memory, which is slow for anything iterated every frame. For values
 * that are stored and scanned, use a dynamic array (array.h).
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same list or the links in it.
 */
#ifndef TB_LIST_H
#define TB_LIST_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief The part of an element that makes it a member of a list.
 *
 * Embed one in a structure and zero-initialize it. The members are private to
 * the list.
 */
typedef struct tb_list_link
{
    struct tb_list_link *next;     /**< Private; NULL while in no list. */
    struct tb_list_link *previous; /**< Private; NULL while in no list. */
} tb_list_link;

/**
 * @brief A list. Declare one, initialize it with #TB_LIST_INIT or
 * tb_list_init(), and keep it at a fixed address while it has elements.
 */
typedef struct tb_list
{
    tb_list_link head; /**< Private: joins the last element to the first. */
} tb_list;

/**
 * @brief Initializer for an empty list: `tb_list active = TB_LIST_INIT(active);`.
 * @param list The list variable being initialized.
 */
#define TB_LIST_INIT(list)                                                                         \
    {                                                                                              \
        {                                                                                          \
            &(list).head, &(list).head                                                             \
        }                                                                                          \
    }

/**
 * @brief Get the element that contains a link:
 * `TB_LIST_ENTRY(link, projectile, link)`.
 * @param link_pointer Pointer to a tb_list_link; required non-null.
 * @param type Type of the structure that embeds the link.
 * @param member Name of the tb_list_link member in that structure.
 * @return `type *` to the structure.
 */
#define TB_LIST_ENTRY(link_pointer, type, member)                                                  \
    ((type *)(void *)((char *)(link_pointer) - offsetof(type, member)))

/**
 * @brief Make a list empty. Use it for a list that #TB_LIST_INIT cannot
 * initialize, such as one inside an allocated structure.
 * @param list Required non-null. It must not have elements, which would be
 * left pointing at a list that no longer knows them.
 */
TB_API void tb_list_init(tb_list *list);

/* -------------------------------------------------------------------------
 * Adding and removing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Add an element at the front of a list.
 * @param list Required non-null and initialized.
 * @param link The link of the element; required non-null and in no list.
 */
TB_API void tb_list_push_front(tb_list *list, tb_list_link *link);

/**
 * @brief Add an element at the back of a list.
 * @param list Required non-null and initialized.
 * @param link The link of the element; required non-null and in no list.
 */
TB_API void tb_list_push_back(tb_list *list, tb_list_link *link);

/**
 * @brief Add an element just before another.
 * @param position A link that is in a list; required non-null.
 * @param link The link of the element to add; required non-null and in no list.
 */
TB_API void tb_list_insert_before(tb_list_link *position, tb_list_link *link);

/**
 * @brief Add an element just after another.
 * @param position A link that is in a list; required non-null.
 * @param link The link of the element to add; required non-null and in no list.
 */
TB_API void tb_list_insert_after(tb_list_link *position, tb_list_link *link);

/**
 * @brief Remove an element from the list it is in, in constant time.
 * @param link The link of the element; required non-null and in a list.
 * Afterwards it is in no list and may be added to one again.
 */
TB_API void tb_list_remove(tb_list_link *link);

/**
 * @brief Remove and return the front element.
 * @param list Required non-null and initialized.
 * @return The link of the element, now in no list, or NULL when the list is
 * empty.
 */
TB_API tb_list_link *tb_list_pop_front(tb_list *list);

/**
 * @brief Remove and return the back element.
 * @param list Required non-null and initialized.
 * @return The link of the element, now in no list, or NULL when the list is
 * empty.
 */
TB_API tb_list_link *tb_list_pop_back(tb_list *list);

/**
 * @brief Whether a link is in a list.
 * @param link Required non-null.
 */
TB_API bool tb_list_link_is_linked(const tb_list_link *link);

/* -------------------------------------------------------------------------
 * Walking a list.
 * ------------------------------------------------------------------------- */

/** @brief Whether a list has no elements. @param list Required non-null and initialized. */
TB_API bool tb_list_is_empty(const tb_list *list);

/**
 * @brief Number of elements in a list, found by counting them.
 * @param list Required non-null and initialized.
 * @return The count. The cost is in proportion to it; a caller that needs
 * the length often keeps its own count.
 */
TB_API size_t tb_list_length(const tb_list *list);

/**
 * @brief The front element.
 * @param list Required non-null and initialized.
 * @return Its link, or NULL when the list is empty.
 */
TB_API tb_list_link *tb_list_first(const tb_list *list);

/**
 * @brief The back element.
 * @param list Required non-null and initialized.
 * @return Its link, or NULL when the list is empty.
 */
TB_API tb_list_link *tb_list_last(const tb_list *list);

/**
 * @brief The element after another.
 *
 * To remove elements while walking, take the next link before removing the
 * current one.
 *
 * @param list The list that `link` is in; required non-null.
 * @param link A link in that list; required non-null.
 * @return The next link, or NULL when `link` is the back element.
 */
TB_API tb_list_link *tb_list_next(const tb_list *list, const tb_list_link *link);

/**
 * @brief The element before another.
 * @param list The list that `link` is in; required non-null.
 * @param link A link in that list; required non-null.
 * @return The previous link, or NULL when `link` is the front element.
 */
TB_API tb_list_link *tb_list_previous(const tb_list *list, const tb_list_link *link);

/**
 * @brief Copy the links of a list into an array, front to back.
 * @param list Required non-null and initialized.
 * @param[out] links Receives up to `capacity` links; may be NULL only when
 * `capacity` is 0.
 * @param capacity Number of links the array can hold.
 * @return The number of elements in the list, which is more than `capacity`
 * when the array was too small to hold them all.
 */
TB_API size_t tb_list_to_array(const tb_list *list, tb_list_link **links, size_t capacity);

/* -------------------------------------------------------------------------
 * Whole lists.
 * ------------------------------------------------------------------------- */

/**
 * @brief Move every element of one list to the back of another, in constant
 * time and keeping their order.
 * @param list Receives the elements; required non-null and initialized.
 * @param other Gives them up and is left empty; required non-null and
 * initialized, and not `list`.
 */
TB_API void tb_list_append(tb_list *list, tb_list *other);

/**
 * @brief Reverse the order of the elements.
 * @param list Required non-null and initialized.
 */
TB_API void tb_list_reverse(tb_list *list);

#endif
