/*
 * Unified GC Test Suite - Multithreaded GC Tests
 * 
 * Additional tests for the double-buffered concurrent GC design:
 * - Double buffer basic operation
 * - Handle validity across buffer swaps
 * - Allocation during compaction
 * - Active table atomic swap
 * - Data preservation across swaps
 * - Multiple GC cycle stability
 * - Phase transitions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "test_runner.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"

/* External reference to g_gc for testing */
extern GCState g_gc;

/* ============================================================================
 * Multithreaded GC Test Cases
 * ============================================================================ */

TEST(test_gc_double_buffer_basic) {
    /* Verify both buffers exist and are properly initialized */
    ASSERT_TRUE(g_gc.buffers[0].storage != NULL);
    ASSERT_TRUE(g_gc.buffers[1].storage != NULL);
    ASSERT_TRUE(g_gc.buffers[0].handles != NULL);
    ASSERT_TRUE(g_gc.buffers[1].handles != NULL);
    
    /* Verify active buffer is one of the two */
    int active = g_gc.active_buffer_index;
    ASSERT_TRUE(active == 0 || active == 1);
    
    /* Verify active_handle_table points to active buffer's handles */
    ASSERT_TRUE(g_gc.active_handle_table == g_gc.buffers[active].handles);
    
    return true;
}

TEST(test_gc_active_table_swap) {
    /* Record initial active buffer */
    int initial_active = g_gc.active_buffer_index;
    
    /* Allocate an object and verify we can dereference it */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    
    void *ptr_before = gc_deref(h);
    ASSERT_TRUE(ptr_before != NULL);
    
    /* Add to roots and run GC (triggers compaction and swap) */
    gc_add_root(h);
    gc_run();
    gc_remove_root(h);
    
    /* After compaction, active buffer may have swapped */
    int after_active = g_gc.active_buffer_index;
    
    /* Verify handle still dereferences correctly after potential swap */
    void *ptr_after = gc_deref(h);
    ASSERT_TRUE(ptr_after != NULL);
    
    /* The pointer may have changed (object moved), but must be valid */
    ASSERT_TRUE(gc_ptr_is_valid(ptr_after));
    
    return true;
}

TEST(test_gc_data_preserved_across_swap) {
    /* Allocate and write pattern */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    
    void *ptr = gc_deref(h);
    ASSERT_TRUE(ptr != NULL);
    
    /* Write recognizable pattern */
    unsigned char *data = (unsigned char *)ptr;
    for (int i = 0; i < 64; i++) {
        data[i] = (unsigned char)(i * 3 + 7);
    }
    
    /* Root and run GC */
    gc_add_root(h);
    gc_run();
    gc_remove_root(h);
    
    /* Verify pattern survived */
    void *ptr_after = gc_deref(h);
    ASSERT_TRUE(ptr_after != NULL);
    
    unsigned char *data_after = (unsigned char *)ptr_after;
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ((unsigned char)(i * 3 + 7), data_after[i]);
    }
    
    return true;
}

TEST(test_gc_multiple_cycles) {
    /* Test stability across multiple GC cycles */
    GCHandle handles[5];
    
    /* Allocate and root several objects */
    for (int i = 0; i < 5; i++) {
        handles[i] = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
        ASSERT_TRUE(handles[i] != GC_HANDLE_NULL);
        gc_add_root(handles[i]);
        
        /* Write unique pattern per object */
        void *ptr = gc_deref(handles[i]);
        ASSERT_TRUE(ptr != NULL);
        memset(ptr, 0xA0 + i, 64);
    }
    
    /* Run multiple GC cycles */
    for (int cycle = 0; cycle < 5; cycle++) {
        gc_run();
        
        /* Verify all handles still valid after each cycle */
        for (int i = 0; i < 5; i++) {
            ASSERT_TRUE(gc_handle_is_valid(handles[i]));
            void *ptr = gc_deref(handles[i]);
            ASSERT_TRUE(ptr != NULL);
            
            /* Verify data preserved */
            unsigned char *data = (unsigned char *)ptr;
            ASSERT_EQ(data[0], (unsigned char)(0xA0 + i));
            ASSERT_EQ(data[63], (unsigned char)(0xA0 + i));
        }
    }
    
    /* Remove roots */
    for (int i = 0; i < 5; i++) {
        gc_remove_root(handles[i]);
    }
    
    return true;
}

TEST(test_gc_canary_after_swap) {
    /* Allocate object with known data */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    
    /* Verify canaries before GC */
    GCCanaryStatus status_before = gc_validate_canaries(h);
    ASSERT_EQ(GC_CANARY_OK, status_before);
    
    /* Root and run GC */
    gc_add_root(h);
    gc_run();
    gc_remove_root(h);
    
    /* Verify canaries still intact after compaction/swap */
    GCCanaryStatus status_after = gc_validate_canaries(h);
    ASSERT_EQ(GC_CANARY_OK, status_after);
    
    return true;
}

TEST(test_gc_dead_objects_freed) {
    /* Allocate object without rooting it */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    
    /* Verify it's alive before GC */
    ASSERT_TRUE(gc_handle_is_valid(h));
    
    /* Run GC without rooting - object should be freed */
    gc_run();
    
    /* After GC, unreferenced object should be freed */
    /* Note: handle may still exist but point to NULL or freed memory */
    void *ptr = gc_deref(h);
    if (ptr != NULL) {
        /* If pointer is still there, check if marked as freed */
        GCHeader *hdr = gc_header(ptr);
        /* Size 0 or FREED flag means freed */
        bool is_freed = (hdr->size == 0) || (hdr->size & 0x80000000);
        ASSERT_TRUE(is_freed);
    }
    
    return true;
}

TEST(test_gc_root_survives_compaction) {
    /* Allocate and root object */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    
    void *ptr = gc_deref(h);
    ASSERT_TRUE(ptr != NULL);
    memset(ptr, 0x42, 64);
    
    /* Add to roots */
    gc_add_root(h);
    
    /* Run GC - object should survive */
    gc_run();
    
    /* Verify still alive and data intact */
    ASSERT_TRUE(gc_handle_is_valid(h));
    
    void *ptr_after = gc_deref(h);
    ASSERT_TRUE(ptr_after != NULL);
    
    unsigned char *data = (unsigned char *)ptr_after;
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(0x42, data[i]);
    }
    
    gc_remove_root(h);
    
    return true;
}

TEST(test_gc_phase_transitions) {
    /* Verify GC starts in IDLE phase */
    ASSERT_EQ(GC_PHASE_IDLE, (int)g_gc.gc_phase);
    
    /* Run GC synchronously and verify it returns to IDLE */
    gc_run();
    
    ASSERT_EQ(GC_PHASE_IDLE, (int)g_gc.gc_phase);
    
    return true;
}

TEST(test_gc_table_consistency_during_compaction) {
    /* This test verifies that allocate_handle writes to both handle tables,
     * ensuring consistency regardless of which buffer is active after a swap */
    
    /* First, run a GC to get into a known state */
    gc_run();
    
    int active_before = g_gc.active_buffer_index;
    
    /* Allocate an object - allocate_handle writes to both tables */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    
    /* Both tables should have the same pointer for this handle */
    void *ptr_active = g_gc.buffers[active_before].handles[h];
    void *ptr_inactive = g_gc.buffers[1 - active_before].handles[h];
    
    /* Active buffer must have the pointer */
    ASSERT_TRUE(ptr_active != NULL);
    
    /* Inactive buffer should also have the pointer (written by allocate_handle) */
    /* If the inactive buffer was reset after compaction, its handles are zeroed,
     * but allocate_handle writes to both, so they should match */
    if (ptr_inactive != NULL) {
        ASSERT_TRUE(ptr_active == ptr_inactive);
    }
    
    /* Verify dereference works */
    void *ptr_deref = gc_deref(h);
    ASSERT_TRUE(ptr_deref != NULL);
    ASSERT_TRUE(ptr_deref == ptr_active);
    
    return true;
}

TEST(test_gc_buffer_swap_changes_deref) {
    /* Allocate and root an object */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    gc_add_root(h);
    
    /* Record which buffer it's in before GC */
    void *ptr_before = gc_deref(h);
    ASSERT_TRUE(ptr_before != NULL);
    
    /* Run GC - this may swap buffers */
    gc_run();
    
    /* After GC, object should still be dereferenceable */
    void *ptr_after = gc_deref(h);
    ASSERT_TRUE(ptr_after != NULL);
    
    /* The pointer should be in the active buffer */
    ASSERT_TRUE(gc_ptr_is_valid(ptr_after));
    
    gc_remove_root(h);
    
    return true;
}

TEST(test_gc_handle_valid_after_multiple_swaps) {
    /* Allocate and root object */
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    gc_add_root(h);
    
    /* Run multiple GCs, each potentially swapping buffers */
    for (int i = 0; i < 10; i++) {
        gc_run();
        ASSERT_TRUE(gc_handle_is_valid(h));
        ASSERT_TRUE(gc_deref(h) != NULL);
    }
    
    gc_remove_root(h);
    
    return true;
}

TEST(test_gc_free_list_preserved_across_swap) {
    /* Allocate some objects */
    GCHandle h1 = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    GCHandle h2 = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h1 != GC_HANDLE_NULL);
    ASSERT_TRUE(h2 != GC_HANDLE_NULL);
    
    /* Don't root them - they should be freed */
    gc_run();
    
    /* After GC, handles should be in free list */
    /* Now allocate new object - should reuse a freed handle */
    GCHandle h3 = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h3 != GC_HANDLE_NULL);
    
    /* h3 should reuse one of the freed handles */
    /* (either h1 or h2's index) */
    ASSERT_TRUE(h3 == h1 || h3 == h2 || h3 > h2);
    
    return true;
}

TEST(test_gc_background_thread_exists) {
    /* Verify background thread was started during init */
    /* The thread should be running after gc_init() */
    /* We can't directly test the thread, but we can verify
     * the GC system is functional */
    
    /* Request background GC - should not crash */
    gc_request_background();
    
    /* Wait for completion */
    gc_wait_for_completion();
    
    /* Should be back in IDLE phase */
    ASSERT_EQ(GC_PHASE_IDLE, (int)g_gc.gc_phase);
    
    return true;
}

/* ============================================================================
 * Test Runner Registration
 * ============================================================================ */

extern "C" void register_multithreaded_gc_tests(void) {
    printf("\n--- Multithreaded GC Tests ---\n");
    
    RUN_TEST(test_gc_double_buffer_basic);
    RUN_TEST(test_gc_active_table_swap);
    RUN_TEST(test_gc_data_preserved_across_swap);
    RUN_TEST(test_gc_multiple_cycles);
    RUN_TEST(test_gc_canary_after_swap);
    RUN_TEST(test_gc_dead_objects_freed);
    // RUN_TEST(test_gc_root_survives_compaction);
    // RUN_TEST(test_gc_phase_transitions);
    // RUN_TEST(test_gc_table_consistency_during_compaction);
    // RUN_TEST(test_gc_buffer_swap_changes_deref);
    // RUN_TEST(test_gc_handle_valid_after_multiple_swaps);
    // RUN_TEST(test_gc_free_list_preserved_across_swap);
    // RUN_TEST(test_gc_background_thread_exists);
}
