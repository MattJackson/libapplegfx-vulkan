# Handler Test Mapping

## Handlers (src/handlers/) | Tests | Status
---------------------------|-------|-------
### Compute
`lagfx_compute_exec_indirect2` | `tests/handler-cmdexecindirect2-parse.c` | ✅ Migrated to new style
### Task
`lagfx_task_define_task2` | ❌ Missing | Need test
`lagfx_task_delete_task` | ❌ Missing | Need test
`lagfx_task_define_host_task2` | ❌ Missing | Need test
### Memory  
`lagfx_memory_map_memory2` | `tests/memory-task.c` (partial) | ⚠️ Uses old dispatch
`lagfx_memory_unmap_memory` | ❌ Missing | Need test
`lagfx_memory_define_child_fifo` | ❌ Missing | Need test
`lagfx_memory_map_memory_immediate` | ❌ Missing | Need test
### Display
`lagfx_display_ack` | ❌ Missing | Need test
`lagfx_display_cursor_show/glyph` | `tests/m5-display-vchan.c` (partial) | ✅ Migrated to new style
`lagfx_display_transaction3` | ❌ Missing | Need test
`lagfx_display_compositor_params` | ❌ Missing | Need test  
`lagfx_display_icc_profile` | ❌ Missing | Need test
### Display VChan (ch 5+)
`lagfx_display_vchan_setup_shared_state` | `tests/m5-display-vchan.c` | ✅ Migrated to new style
`lagfx_display_vchan_display_submit` | `tests/m5-display-vchan.c` | ✅ Migrated to new style
`lagfx_display_define_child_fifo` | `tests/m5-display-vchan.c` | ✅ Migrated to new style
`lagfx_display_vchan_present` | ❌ Missing | Need test
`lagfx_display_vchan_present_gamma` | ❌ Missing | Need test
### Sync
`lagfx_sync_synchronize_resources` | ❌ Missing | Need test
### Utility
`lagfx_util_nop` | `tests/header-syntax-check.c` (partial) | ⚠️ Partial coverage
`lagfx_util_device_info` | ❌ Missing | Need test

## Dispatchers (src/dispatchers/) | Tests | Status
----------------------------------|-------|-------
`channel_0_dispatcher` | `tests/m4-doorbell-drain.c` | ⚠️ Uses old dispatch
`compute_dispatcher` | ❌ Missing | Need test  
`display_vchan_dispatcher` | `tests/m5-display-vchan.c` | ✅ Migrated to new style
`channel_1-4_dispatchers` | ❌ Missing | Need tests

## Legacy Protocol (src/protocol/) | Tests | Status
------------------------------------|-------|-------
`lagfx_protocol_dispatch_one()` | `tests/fuzz-protocol-dispatch.c`, `tests/protocol-dispatch.c` | ⚠️ Deprecated - prod doesn't use this
