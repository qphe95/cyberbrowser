/*
 * Unit tests for PreOrderCompactionArray
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "preorder_compaction_array.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Payload used by tests. PreOrderCompactionArrayNode must be the first member. */
typedef struct TestNode {
    PreOrderCompactionArrayNode array_node;
    int value;
    char name[32];
} TestNode;

TEST(test_init_free) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 4));
    ASSERT_EQ((size_t)0, po_array_count(&array));
    ASSERT_EQ((size_t)0, po_array_active_count(&array));
    po_array_free(&array);
    return true;
}

TEST(test_add_single_root) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 4));

    int root = po_array_add(&array, -1);
    ASSERT_EQ(0, root);
    ASSERT_TRUE(po_array_is_active(&array, root));
    ASSERT_EQ(-1, po_array_parent(&array, root));
    ASSERT_EQ(-1, po_array_first_child(&array, root));
    ASSERT_EQ(-1, po_array_next_sibling(&array, root));

    TestNode *node = (TestNode*)po_array_payload(&array, root);
    ASSERT_TRUE(node != NULL);
    node->value = 42;

    po_array_free(&array);
    return true;
}

TEST(test_add_children_and_siblings) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 4));

    int root = po_array_add(&array, -1);
    int c1 = po_array_add(&array, root);
    int c2 = po_array_add(&array, root);
    int c3 = po_array_add(&array, root);

    ASSERT_EQ(0, root);
    ASSERT_EQ(1, c1);
    ASSERT_EQ(2, c2);
    ASSERT_EQ(3, c3);

    ASSERT_EQ(root, po_array_parent(&array, c1));
    ASSERT_EQ(root, po_array_parent(&array, c2));
    ASSERT_EQ(root, po_array_parent(&array, c3));

    ASSERT_EQ(c1, po_array_first_child(&array, root));
    ASSERT_EQ(c3, po_array_last_child(&array, root));

    ASSERT_EQ(c2, po_array_next_sibling(&array, c1));
    ASSERT_EQ(c3, po_array_next_sibling(&array, c2));
    ASSERT_EQ(-1, po_array_next_sibling(&array, c3));

    ASSERT_EQ(c1, po_array_prev_sibling(&array, c2));
    ASSERT_EQ(c2, po_array_prev_sibling(&array, c3));

    po_array_free(&array);
    return true;
}

TEST(test_preorder_layout_after_compaction) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 8));

    /* Build tree: root -> [a -> [a1, a2], b -> [b1]] */
    int root = po_array_add(&array, -1);
    int a = po_array_add(&array, root);
    int a1 = po_array_add(&array, a);
    int a2 = po_array_add(&array, a);
    int b = po_array_add(&array, root);
    int b1 = po_array_add(&array, b);

    ((TestNode*)po_array_payload(&array, root))->value = 0;
    ((TestNode*)po_array_payload(&array, a))->value    = 1;
    ((TestNode*)po_array_payload(&array, a1))->value   = 2;
    ((TestNode*)po_array_payload(&array, a2))->value   = 3;
    ((TestNode*)po_array_payload(&array, b))->value    = 4;
    ((TestNode*)po_array_payload(&array, b1))->value   = 5;

    size_t active = po_array_compact(&array);
    ASSERT_EQ((size_t)6, active);
    ASSERT_EQ((size_t)6, po_array_count(&array));

    /* After compaction the order should be root, a, a1, a2, b, b1. */
    int expected[] = {0, 1, 2, 3, 4, 5};
    for (size_t i = 0; i < 6; i++) {
        TestNode *n = (TestNode*)po_array_payload(&array, (int)i);
        ASSERT_TRUE(n != NULL);
        ASSERT_EQ(expected[i], n->value);
    }

    /* Verify tree structure survived remapping. */
    ASSERT_EQ(0, po_array_parent(&array, 1));   /* a parent root */
    ASSERT_EQ(1, po_array_parent(&array, 2));   /* a1 parent a */
    ASSERT_EQ(1, po_array_parent(&array, 3));   /* a2 parent a */
    ASSERT_EQ(0, po_array_parent(&array, 4));   /* b parent root */
    ASSERT_EQ(4, po_array_parent(&array, 5));   /* b1 parent b */

    ASSERT_EQ(1, po_array_first_child(&array, 0));
    ASSERT_EQ(4, po_array_last_child(&array, 0));

    po_array_free(&array);
    return true;
}

TEST(test_delete_subtree_and_compact) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 8));

    /* root -> [a -> [a1, a2], b -> [b1]] */
    int root = po_array_add(&array, -1);
    int a = po_array_add(&array, root);
    int a1 = po_array_add(&array, a);
    int a2 = po_array_add(&array, a);
    int b = po_array_add(&array, root);
    int b1 = po_array_add(&array, b);

    ((TestNode*)po_array_payload(&array, root))->value = 0;
    ((TestNode*)po_array_payload(&array, a))->value    = 1;
    ((TestNode*)po_array_payload(&array, a1))->value   = 2;
    ((TestNode*)po_array_payload(&array, a2))->value   = 3;
    ((TestNode*)po_array_payload(&array, b))->value    = 4;
    ((TestNode*)po_array_payload(&array, b1))->value   = 5;

    /* Delete the 'a' subtree. */
    po_array_delete(&array, a);
    ASSERT_EQ((size_t)3, po_array_active_count(&array));
    ASSERT_FALSE(po_array_is_active(&array, a));
    ASSERT_FALSE(po_array_is_active(&array, a1));
    ASSERT_FALSE(po_array_is_active(&array, a2));
    ASSERT_TRUE(po_array_is_active(&array, root));
    ASSERT_TRUE(po_array_is_active(&array, b));
    ASSERT_TRUE(po_array_is_active(&array, b1));

    size_t active = po_array_compact(&array);
    ASSERT_EQ((size_t)3, active);

    /* Remaining order: root, b, b1. */
    int expected[] = {0, 4, 5};
    for (size_t i = 0; i < 3; i++) {
        TestNode *n = (TestNode*)po_array_payload(&array, (int)i);
        ASSERT_TRUE(n != NULL);
        ASSERT_EQ(expected[i], n->value);
    }

    ASSERT_EQ(1, po_array_first_child(&array, root));
    ASSERT_EQ(1, po_array_last_child(&array, root));
    ASSERT_EQ(2, po_array_first_child(&array, 1));

    po_array_free(&array);
    return true;
}

TEST(test_multiple_roots) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 8));

    int r1 = po_array_add(&array, -1);
    int r2 = po_array_add(&array, -1);
    int r3 = po_array_add(&array, -1);

    ASSERT_EQ(-1, po_array_parent(&array, r1));
    ASSERT_EQ(-1, po_array_parent(&array, r2));
    ASSERT_EQ(-1, po_array_parent(&array, r3));

    ASSERT_EQ(r2, po_array_next_sibling(&array, r1));
    ASSERT_EQ(r3, po_array_next_sibling(&array, r2));
    ASSERT_EQ(-1, po_array_next_sibling(&array, r3));

    po_array_free(&array);
    return true;
}

TEST(test_payload_to_index_roundtrip) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 4));

    int a = po_array_add(&array, -1);
    int b = po_array_add(&array, a);

    TestNode *pa = (TestNode*)po_array_payload(&array, a);
    TestNode *pb = (TestNode*)po_array_payload(&array, b);

    ASSERT_EQ(a, po_array_index_from_payload(&array, pa));
    ASSERT_EQ(b, po_array_index_from_payload(&array, pb));
    ASSERT_EQ(-1, po_array_index_from_payload(&array, NULL));

    po_array_free(&array);
    return true;
}

TEST(test_growth_beyond_initial_capacity) {
    PreOrderCompactionArray array;
    ASSERT_TRUE(po_array_init(&array, sizeof(TestNode), 2));

    for (int i = 0; i < 100; i++) {
        int idx = po_array_add(&array, -1);
        ASSERT_EQ(i, idx);
        TestNode *p = (TestNode*)po_array_payload(&array, idx);
        p->value = i;
    }

    ASSERT_EQ((size_t)100, po_array_count(&array));
    ASSERT_EQ((size_t)100, po_array_active_count(&array));

    for (int i = 0; i < 100; i++) {
        TestNode *p = (TestNode*)po_array_payload(&array, i);
        ASSERT_EQ(i, p->value);
    }

    po_array_free(&array);
    return true;
}

void run_preorder_compaction_array_tests(void) {
    printf("\n--- PreOrderCompactionArray Tests ---\n");
    RUN_TEST(test_init_free);
    RUN_TEST(test_add_single_root);
    RUN_TEST(test_add_children_and_siblings);
    RUN_TEST(test_preorder_layout_after_compaction);
    RUN_TEST(test_delete_subtree_and_compact);
    RUN_TEST(test_multiple_roots);
    RUN_TEST(test_payload_to_index_roundtrip);
    RUN_TEST(test_growth_beyond_initial_capacity);
}

#ifdef __cplusplus
}
#endif
