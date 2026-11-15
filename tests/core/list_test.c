/* An intrusive list is nothing but pointers, and one wrong pointer corrupts
 * every element after it. Each operation is therefore checked by walking the
 * whole list in both directions and comparing the order with a plain array,
 * and the mistakes a caller can make with links are checked to be caught. */
#include "badger/list.h"

#include "badger/assert.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

enum
{
    ITEM_COUNT = 64
};

/* An element. The link is deliberately not its first member, so that getting
 * from a link back to its element is really exercised. */
typedef struct item
{
    int id;
    tb_list_link link;
    tb_list_link other_link; /* Membership of a second list at the same time. */
    int owner;               /* For the model: 0 in no list, else 1 or 2. */
} item;

static item items[ITEM_COUNT] = {0};

/* Puts every item back in no list, with its index as its id. */
static void reset_items(void)
{
    memset(items, 0, sizeof(items));
    for (int index = 0; index < ITEM_COUNT; index++)
    {
        items[index].id = index;
    }
}

/* The reference: the ids of a list's elements, front to back. */
typedef struct list_model
{
    int ids[ITEM_COUNT];
    size_t length;
} list_model;

static void model_insert(list_model *model, size_t index, int id)
{
    memmove(&model->ids[index + 1], &model->ids[index], (model->length - index) * sizeof(int));
    model->ids[index] = id;
    model->length++;
}

static void model_remove(list_model *model, size_t index)
{
    model->length--;
    memmove(&model->ids[index], &model->ids[index + 1], (model->length - index) * sizeof(int));
}

static size_t model_find(const list_model *model, int id)
{
    for (size_t index = 0; index < model->length; index++)
    {
        if (model->ids[index] == id)
        {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Walks a list forward and backward and counts every way it disagrees with
 * the model: a wrong element, a wrong length, or an end that is not an end. */
static size_t count_differences(const tb_list *list, const list_model *model)
{
    size_t differences = {0};
    size_t index = {0};

    for (tb_list_link *link = tb_list_first(list); link != NULL; link = tb_list_next(list, link))
    {
        if (index >= model->length || TB_LIST_ENTRY(link, item, link)->id != model->ids[index])
        {
            differences++;
        }
        index++;
    }
    if (index != model->length)
    {
        differences++;
    }
    index = model->length;
    for (tb_list_link *link = tb_list_last(list); link != NULL; link = tb_list_previous(list, link))
    {
        if (index == 0 || TB_LIST_ENTRY(link, item, link)->id != model->ids[index - 1])
        {
            differences++;
            break;
        }
        index--;
    }
    if (index != 0)
    {
        differences++;
    }
    if (tb_list_length(list) != model->length || tb_list_is_empty(list) != (model->length == 0))
    {
        differences++;
    }
    return differences;
}

/* Builds a list of the first `length` items, in order, and the model of it. */
static void fill_list(tb_list *list, list_model *model, size_t length)
{
    reset_items();
    tb_list_init(list);
    model->length = 0;
    for (size_t index = 0; index < length; index++)
    {
        tb_list_push_back(list, &items[index].link);
        model_insert(model, index, (int)index);
    }
}

/* --- empty lists -------------------------------------------------------------- */

TB_TEST(core_list, a_new_list_is_empty_however_it_is_initialized)
{
    tb_list by_initializer = TB_LIST_INIT(by_initializer);
    tb_list by_function = {0};
    tb_list *const lists[] = {&by_initializer, &by_function};

    tb_list_init(&by_function);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(lists); index++)
    {
        TB_TEST_CONTEXT("list %zu", index);
        TB_TEST_EXPECT(tb_list_is_empty(lists[index]));
        TB_TEST_EXPECT_UINT(tb_list_length(lists[index]), ==, 0);
        TB_TEST_EXPECT_POINTER(tb_list_first(lists[index]), ==, NULL);
        TB_TEST_EXPECT_POINTER(tb_list_last(lists[index]), ==, NULL);
        TB_TEST_EXPECT_POINTER(tb_list_pop_front(lists[index]), ==, NULL);
        TB_TEST_EXPECT_POINTER(tb_list_pop_back(lists[index]), ==, NULL);
        TB_TEST_EXPECT_UINT(tb_list_to_array(lists[index], NULL, 0), ==, 0);
        tb_list_reverse(lists[index]);
        TB_TEST_EXPECT(tb_list_is_empty(lists[index]));
    }
}

/* --- adding and removing ------------------------------------------------------ */

TB_TEST(core_list, push_back_and_push_front_add_at_their_ends)
{
    tb_list list = TB_LIST_INIT(list);
    list_model model = {0};

    reset_items();
    for (int index = 0; index < ITEM_COUNT; index++)
    {
        TB_TEST_EXPECT(!tb_list_link_is_linked(&items[index].link));
        if (index % 2 == 0)
        {
            tb_list_push_back(&list, &items[index].link);
            model_insert(&model, model.length, index);
            TB_TEST_EXPECT_POINTER(tb_list_last(&list), ==, &items[index].link);
        }
        else
        {
            tb_list_push_front(&list, &items[index].link);
            model_insert(&model, 0, index);
            TB_TEST_EXPECT_POINTER(tb_list_first(&list), ==, &items[index].link);
        }
        TB_TEST_EXPECT(tb_list_link_is_linked(&items[index].link));
        TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
    }
    TB_TEST_EXPECT_UINT(tb_list_length(&list), ==, ITEM_COUNT);
}

TB_TEST(core_list, an_entry_is_found_from_its_link)
{
    reset_items();
    for (int index = 0; index < ITEM_COUNT; index++)
    {
        TB_TEST_EXPECT_POINTER(TB_LIST_ENTRY(&items[index].link, item, link), ==, &items[index]);
        TB_TEST_EXPECT_POINTER(TB_LIST_ENTRY(&items[index].other_link, item, other_link), ==,
                               &items[index]);
        TB_TEST_EXPECT_INT(TB_LIST_ENTRY(&items[index].link, item, link)->id, ==, index);
    }
}

TB_TEST(core_list, insert_before_and_after_place_an_element_next_to_another)
{
    enum
    {
        LENGTH = 6
    };

    for (size_t position = 0; position < LENGTH; position++)
    {
        for (size_t side = 0; side < 2; side++)
        {
            tb_list list = {0};
            list_model model = {0};
            item *const added = &items[LENGTH];

            fill_list(&list, &model, LENGTH);
            TB_TEST_CONTEXT("position %zu, side %zu", position, side);
            if (side == 0)
            {
                tb_list_insert_before(&items[position].link, &added->link);
                model_insert(&model, position, added->id);
            }
            else
            {
                tb_list_insert_after(&items[position].link, &added->link);
                model_insert(&model, position + 1, added->id);
            }
            TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
        }
    }
}

TB_TEST(core_list, remove_takes_out_any_element_and_frees_its_link)
{
    enum
    {
        LENGTH = 6
    };

    for (size_t position = 0; position < LENGTH; position++)
    {
        tb_list list = {0};
        list_model model = {0};

        fill_list(&list, &model, LENGTH);
        TB_TEST_CONTEXT("position %zu", position);
        tb_list_remove(&items[position].link);
        model_remove(&model, position);
        TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
        TB_TEST_EXPECT(!tb_list_link_is_linked(&items[position].link));
        /* A removed element can join a list again. */
        tb_list_push_front(&list, &items[position].link);
        model_insert(&model, 0, (int)position);
        TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
    }
}

TB_TEST(core_list, the_only_element_can_be_removed_and_leaves_an_empty_list)
{
    tb_list list = {0};
    list_model model = {0};

    fill_list(&list, &model, 1);
    TB_TEST_EXPECT_POINTER(tb_list_first(&list), ==, tb_list_last(&list));
    TB_TEST_EXPECT_POINTER(tb_list_next(&list, &items[0].link), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_list_previous(&list, &items[0].link), ==, NULL);
    tb_list_remove(&items[0].link);
    model.length = 0;
    TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
}

TB_TEST(core_list, pop_removes_from_either_end_in_order)
{
    tb_list list = {0};
    list_model model = {0};

    fill_list(&list, &model, ITEM_COUNT);
    for (int expected = 0; expected < ITEM_COUNT / 2; expected++)
    {
        tb_list_link *const front = tb_list_pop_front(&list);
        tb_list_link *const back = tb_list_pop_back(&list);

        TB_TEST_REQUIRE_POINTER(front, !=, NULL);
        TB_TEST_REQUIRE_POINTER(back, !=, NULL);
        TB_TEST_EXPECT_INT(TB_LIST_ENTRY(front, item, link)->id, ==, expected);
        TB_TEST_EXPECT_INT(TB_LIST_ENTRY(back, item, link)->id, ==, ITEM_COUNT - 1 - expected);
        TB_TEST_EXPECT(!tb_list_link_is_linked(front));
        TB_TEST_EXPECT(!tb_list_link_is_linked(back));
    }
    TB_TEST_EXPECT(tb_list_is_empty(&list));
    TB_TEST_EXPECT_POINTER(tb_list_pop_front(&list), ==, NULL);
}

TB_TEST(core_list, elements_can_be_removed_while_the_list_is_walked)
{
    tb_list list = {0};
    list_model model = {0};
    tb_list_link *link = NULL;

    fill_list(&list, &model, ITEM_COUNT);
    /* The documented pattern: take the next link, then remove the current. */
    link = tb_list_first(&list);
    while (link != NULL)
    {
        tb_list_link *const next = tb_list_next(&list, link);
        const item *const entry = TB_LIST_ENTRY(link, item, link);

        if (entry->id % 3 != 0)
        {
            tb_list_remove(link);
            model_remove(&model, model_find(&model, entry->id));
        }
        link = next;
    }
    TB_TEST_EXPECT_UINT(tb_list_length(&list), ==, (ITEM_COUNT + 2) / 3);
    TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
}

TB_TEST(core_list, an_element_with_two_links_is_in_two_lists_at_once)
{
    tb_list forward = TB_LIST_INIT(forward);
    tb_list backward = TB_LIST_INIT(backward);
    int expected = ITEM_COUNT - 1;

    reset_items();
    for (int index = 0; index < ITEM_COUNT; index++)
    {
        tb_list_push_back(&forward, &items[index].link);
        tb_list_push_front(&backward, &items[index].other_link);
    }
    for (tb_list_link *link = tb_list_first(&backward); link != NULL;
         link = tb_list_next(&backward, link))
    {
        TB_TEST_EXPECT_INT(TB_LIST_ENTRY(link, item, other_link)->id, ==, expected);
        expected--;
    }
    /* Leaving one list does not disturb the other. */
    tb_list_remove(&items[10].link);
    TB_TEST_EXPECT_UINT(tb_list_length(&forward), ==, ITEM_COUNT - 1);
    TB_TEST_EXPECT_UINT(tb_list_length(&backward), ==, ITEM_COUNT);
    TB_TEST_EXPECT(tb_list_link_is_linked(&items[10].other_link));
}

/* --- whole lists -------------------------------------------------------------- */

TB_TEST(core_list, to_array_copies_the_links_that_fit_and_reports_the_length)
{
    static const size_t capacities[] = {0, 1, 5, 9, 10, 11, 20};
    tb_list list = {0};
    list_model model = {0};

    fill_list(&list, &model, 10);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(capacities); row++)
    {
        tb_list_link *links[21] = {0};
        const size_t capacity = capacities[row];
        const size_t copied = capacity < 10 ? capacity : 10;

        TB_TEST_CONTEXT("capacity %zu", capacity);
        TB_TEST_EXPECT_UINT(tb_list_to_array(&list, links, capacity), ==, 10);
        for (size_t index = 0; index < copied; index++)
        {
            TB_TEST_EXPECT_POINTER(links[index], ==, &items[index].link);
        }
        /* Nothing is written beyond the capacity or beyond the elements. */
        TB_TEST_EXPECT_POINTER(links[copied], ==, NULL);
    }
}

TB_TEST(core_list, append_moves_every_element_and_empties_the_other_list)
{
    static const size_t lengths[] = {0, 1, 2, 7};

    for (size_t first_row = 0; first_row < TB_TEST_COUNT_OF(lengths); first_row++)
    {
        for (size_t second_row = 0; second_row < TB_TEST_COUNT_OF(lengths); second_row++)
        {
            const size_t first_length = lengths[first_row];
            const size_t second_length = lengths[second_row];
            tb_list first = TB_LIST_INIT(first);
            tb_list second = TB_LIST_INIT(second);
            list_model model = {0};
            list_model empty = {0};

            reset_items();
            for (size_t index = 0; index < first_length + second_length; index++)
            {
                tb_list_push_back(index < first_length ? &first : &second, &items[index].link);
                model_insert(&model, index, (int)index);
            }
            TB_TEST_CONTEXT("%zu elements and %zu elements", first_length, second_length);
            tb_list_append(&first, &second);
            TB_TEST_EXPECT_UINT(count_differences(&first, &model), ==, 0);
            TB_TEST_EXPECT_UINT(count_differences(&second, &empty), ==, 0);
            /* The emptied list is still usable. */
            tb_list_push_back(&second, &items[ITEM_COUNT - 1].link);
            TB_TEST_EXPECT_UINT(tb_list_length(&second), ==, 1);
        }
    }
}

TB_TEST(core_list, reverse_turns_the_order_around)
{
    static const size_t lengths[] = {0, 1, 2, 3, 10, ITEM_COUNT};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        tb_list list = {0};
        list_model model = {0};
        list_model reversed = {0};

        fill_list(&list, &model, lengths[row]);
        for (size_t index = 0; index < lengths[row]; index++)
        {
            model_insert(&reversed, 0, (int)index);
        }
        TB_TEST_CONTEXT("length %zu", lengths[row]);
        tb_list_reverse(&list);
        TB_TEST_EXPECT_UINT(count_differences(&list, &reversed), ==, 0);
        tb_list_reverse(&list);
        TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
    }
}

/* --- random operations against a model ---------------------------------------- */

TB_TEST(core_list, random_operations_on_two_lists_agree_with_plain_arrays)
{
    enum
    {
        OPERATION_COUNT = 30000
    };
    tb_list lists[2] = {TB_LIST_INIT(lists[0]), TB_LIST_INIT(lists[1])};
    list_model models[2] = {0};
    size_t removal_count = {0};
    size_t append_count = {0};

    reset_items();
    for (size_t operation = 0; operation < OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(100);
        const size_t which = (size_t)tb_test_random_below(2);
        item *const picked = &items[tb_test_random_below(ITEM_COUNT)];
        tb_list *const list = &lists[which];
        list_model *const model = &models[which];

        TB_TEST_CONTEXT("operation %zu (choice %u), item %d owned by %d, lists of %zu and %zu",
                        operation, (unsigned)choice, picked->id, picked->owner, models[0].length,
                        models[1].length);
        if (picked->owner != 0 && choice < 40)
        {
            /* The item is in a list: remove it from wherever it is. */
            list_model *const owner = &models[picked->owner - 1];

            tb_list_remove(&picked->link);
            model_remove(owner, model_find(owner, picked->id));
            picked->owner = 0;
            removal_count++;
        }
        else if (picked->owner == 0 && choice < 25)
        {
            tb_list_push_back(list, &picked->link);
            model_insert(model, model->length, picked->id);
            picked->owner = (int)which + 1;
        }
        else if (picked->owner == 0 && choice < 50)
        {
            tb_list_push_front(list, &picked->link);
            model_insert(model, 0, picked->id);
            picked->owner = (int)which + 1;
        }
        else if (picked->owner == 0 && model->length > 0)
        {
            /* Next to a random element of the chosen list. */
            const size_t index = (size_t)tb_test_random_below(model->length);
            item *const neighbor = &items[model->ids[index]];

            if (choice % 2 == 0)
            {
                tb_list_insert_before(&neighbor->link, &picked->link);
                model_insert(model, index, picked->id);
            }
            else
            {
                tb_list_insert_after(&neighbor->link, &picked->link);
                model_insert(model, index + 1, picked->id);
            }
            picked->owner = (int)which + 1;
        }
        else if (choice < 60)
        {
            tb_list_link *const popped =
                choice % 2 == 0 ? tb_list_pop_front(list) : tb_list_pop_back(list);

            if (model->length == 0)
            {
                TB_TEST_EXPECT_POINTER(popped, ==, NULL);
            }
            else
            {
                const size_t index = choice % 2 == 0 ? 0 : model->length - 1;

                TB_TEST_REQUIRE_POINTER(popped, !=, NULL);
                TB_TEST_EXPECT_INT(TB_LIST_ENTRY(popped, item, link)->id, ==, model->ids[index]);
                TB_LIST_ENTRY(popped, item, link)->owner = 0;
                model_remove(model, index);
                removal_count++;
            }
        }
        else if (choice < 80)
        {
            tb_list_reverse(list);
            for (size_t index = 0; index < model->length / 2; index++)
            {
                const int swapped = model->ids[index];

                model->ids[index] = model->ids[model->length - 1 - index];
                model->ids[model->length - 1 - index] = swapped;
            }
        }
        else
        {
            /* Everything in the other list moves to the back of this one. */
            list_model *const other = &models[1 - which];

            tb_list_append(list, &lists[1 - which]);
            for (size_t index = 0; index < other->length; index++)
            {
                items[other->ids[index]].owner = (int)which + 1;
                model_insert(model, model->length, other->ids[index]);
            }
            other->length = 0;
            append_count++;
        }
        TB_TEST_REQUIRE_UINT(count_differences(&lists[0], &models[0]), ==, 0);
        TB_TEST_REQUIRE_UINT(count_differences(&lists[1], &models[1]), ==, 0);
    }
    TB_TEST_EXPECT_UINT(removal_count, >, OPERATION_COUNT / 10);
    TB_TEST_EXPECT_UINT(append_count, >, OPERATION_COUNT / 20);
}

/* --- contract violations ------------------------------------------------------ */

typedef struct list_request
{
    tb_list *list;
    tb_list *other;
    tb_list_link *link;
    tb_list_link *position;
} list_request;

static void push_front_request(void *context)
{
    const list_request *const request = context;

    tb_list_push_front(request->list, request->link);
}

static void push_back_request(void *context)
{
    const list_request *const request = context;

    tb_list_push_back(request->list, request->link);
}

static void insert_before_request(void *context)
{
    const list_request *const request = context;

    tb_list_insert_before(request->position, request->link);
}

static void insert_after_request(void *context)
{
    const list_request *const request = context;

    tb_list_insert_after(request->position, request->link);
}

static void remove_request(void *context)
{
    const list_request *const request = context;

    tb_list_remove(request->link);
}

static void next_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_next(request->list, request->link);
}

static void previous_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_previous(request->list, request->link);
}

static void append_request(void *context)
{
    const list_request *const request = context;

    tb_list_append(request->list, request->other);
}

static void is_empty_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_is_empty(request->list);
}

static void first_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_first(request->list);
}

static void last_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_last(request->list);
}

static void length_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_length(request->list);
}

static void pop_front_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_pop_front(request->list);
}

static void pop_back_request(void *context)
{
    const list_request *const request = context;

    (void)tb_list_pop_back(request->list);
}

static void reverse_request(void *context)
{
    const list_request *const request = context;

    tb_list_reverse(request->list);
}

static void to_array_without_an_array(void *context)
{
    const list_request *const request = context;

    (void)tb_list_to_array(request->list, NULL, 4);
}

static void init_null(void *context)
{
    (void)context;
    tb_list_init(NULL);
}

static void is_linked_null(void *context)
{
    (void)context;
    (void)tb_list_link_is_linked(NULL);
}

TB_TEST(core_list, a_link_used_twice_or_not_at_all_violates_the_contract)
{
    tb_list list = {0};
    tb_list other = TB_LIST_INIT(other);
    list_model model = {0};
    list_request request = {.list = &other};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    fill_list(&list, &model, 4);

    /* Adding a link that is already in a list would tangle both lists. */
    request.link = &items[1].link;
    request.position = &items[2].link;
    TB_TEST_EXPECT_TRAP(push_front_request, &request, "the link must not be in a list already");
    TB_TEST_EXPECT_TRAP(push_back_request, &request, "the link must not be in a list already");
    TB_TEST_EXPECT_TRAP(insert_before_request, &request, "the link must not be in a list already");
    TB_TEST_EXPECT_TRAP(insert_after_request, &request, "the link must not be in a list already");

    /* A link that is in no list has no neighbors and cannot be removed. */
    request.list = &list;
    request.link = &items[10].link;
    TB_TEST_EXPECT_TRAP(remove_request, &request, "the link must be in a list");
    TB_TEST_EXPECT_TRAP(next_request, &request, "the link must be in a list");
    TB_TEST_EXPECT_TRAP(previous_request, &request, "the link must be in a list");
    request.position = &items[11].link;
    TB_TEST_EXPECT_TRAP(insert_before_request, &request, "the position must be in a list");
    TB_TEST_EXPECT_TRAP(insert_after_request, &request, "the position must be in a list");

    /* Removing twice is the same mistake. */
    tb_list_remove(&items[3].link);
    model_remove(&model, 3);
    request.link = &items[3].link;
    TB_TEST_EXPECT_TRAP(remove_request, &request, "the link must be in a list");
    TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
    TB_TEST_EXPECT(tb_list_is_empty(&other));
}

TB_TEST(core_list, a_list_that_was_never_initialized_violates_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        push_front_request, push_back_request, is_empty_request, first_request,   last_request,
        length_request,     pop_front_request, pop_back_request, reverse_request, append_request,
    };
    tb_list zeroed = {0};
    tb_list other = TB_LIST_INIT(other);
    list_request request = {.list = &zeroed, .other = &other};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    reset_items();
    request.link = &items[0].link;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], &request, "the list must be initialized");
    }
    TB_TEST_EXPECT(!tb_list_link_is_linked(&items[0].link));
}

TB_TEST(core_list, missing_lists_and_links_violate_the_contract)
{
    static tb_test_trap_body *const list_bodies[] = {
        push_front_request, push_back_request, is_empty_request,  first_request,
        last_request,       length_request,    pop_front_request, pop_back_request,
        reverse_request,    append_request,    next_request,      previous_request,
    };
    tb_list list = {0};
    tb_list other = TB_LIST_INIT(other);
    list_model model = {0};
    list_request request = {.other = &other};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    fill_list(&list, &model, 3);
    request.link = &items[1].link;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(list_bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(list_bodies[index], &request, "list != NULL");
    }
    request.list = &list;
    request.link = NULL;
    request.position = NULL;
    TB_TEST_EXPECT_TRAP(push_front_request, &request, "link != NULL");
    TB_TEST_EXPECT_TRAP(push_back_request, &request, "link != NULL");
    TB_TEST_EXPECT_TRAP(remove_request, &request, "link != NULL");
    TB_TEST_EXPECT_TRAP(next_request, &request, "link != NULL");
    TB_TEST_EXPECT_TRAP(previous_request, &request, "link != NULL");
    TB_TEST_EXPECT_TRAP(insert_before_request, &request, "position != NULL");
    TB_TEST_EXPECT_TRAP(insert_after_request, &request, "position != NULL");
    TB_TEST_EXPECT_TRAP(is_linked_null, NULL, "link != NULL");
    TB_TEST_EXPECT_TRAP(init_null, NULL, "list != NULL");
    TB_TEST_EXPECT_TRAP(to_array_without_an_array, &request, "links != NULL || capacity == 0");

    /* A list cannot take its own elements, and needs a list to take from. */
    request.other = &list;
    TB_TEST_EXPECT_TRAP(append_request, &request, "a list cannot be appended to itself");
    request.other = NULL;
    TB_TEST_EXPECT_TRAP(append_request, &request, "other != NULL");
    TB_TEST_EXPECT_UINT(count_differences(&list, &model), ==, 0);
}
