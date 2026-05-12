# Dispatcher-Handler Architecture (2026-05-12)

## Clean Separation of Concerns

```
Doorbell (MMIO 0x1020) 
  ↓
Dispatcher Registry → Per-Channel Dispatcher
  ├─ Channel0Dispatcher (ch=0, primary ring)
  └─ ComputeDispatcher (ch=1-4, compute/render)
      ↓
Switch-case Dispatch → Handler by Functional Area
  ├─ handlers/compute/exec_indirect2.c
  ├─ handlers/task/task.c
  ├─ handlers/memory/memory.c
  ├─ handlers/display/display.c
  ├─ handlers/sync/sync.c
  └─ handlers/util/util.c
```

## Logging Strategy (For Debugging)

### Dispatcher Layer - Routing Logs
- **LOG level**: "Channel0Dispatcher: routing opcode 0x20 → lagfx_compute_exec_indirect2"
- Shows which opcode was received and where it's routed
- Single source of truth for opcode flow tracking

### Handler Layer - Logic Logs  
- **TRACE level**: "handler: executing logic function"
- Minimal noise, only visible when explicitly enabled
- Confirms handler entry without duplicating opcode info

## Benefits

1. **Single Responsibility**
   - Dispatchers route commands by channel (no logic)
   - Handlers implement opcode functionality (no routing)

2. **Easy Debugging**
   - Follow call chain: `doorbell → dispatcher switch-case → handler function`
   - Dispatcher logs show "what" and "where", handlers confirm execution

3. **No Opcode Table Lookup**
   - Direct function calls from switch-case
   - No global `opcodes.c` table lookup overhead

4. **Modular Organization**
   - Each functional area in separate `.c` file
   - Easy to find/modify handler logic without dispatcher code

## File Structure

```
src/
├── dispatchers/
│   ├── channel_0_dispatcher.c    # Primary ring routing (ch=0)
│   └── compute_dispatcher.c      # Compute/render routing (ch=1-4)
│
├── handlers/
│   ├── handlers.h                # Handler declarations
│   ├── compute/exec_indirect2.c  # CmdExecIndirect2 logic
│   ├── task/task.c               # Task management logic
│   ├── memory/memory.c           # Memory mapping logic  
│   ├── display/display.c         # Display operations logic
│   ├── sync/sync.c               # Synchronization logic
│   └── util/util.c               # Utility functions (NOP, device info)
```

## Migration Status

### ✅ Completed
- Dispatcher hierarchy with per-channel routing
- Switch-case dispatch in Channel0Dispatcher for all primary ring opcodes
- Handler stubs organized by functional area
- Logging strategy: dispatcher logs routing, handlers log execution

### ⏭️ Next Steps
1. Migrate remaining compute channel opcodes (0x37/0x38/0x39) to handlers
2. Implement DisplayVchanDispatcher for channels 5+
3. Replace handler stubs with full implementations from `ops_*.c`
4. Delete legacy `opcodes.c`, `ops_cmdbuf.c`, `ops_device.c`, etc.

## Comparison: Before vs After

### Before (Legacy)
```
Doorbell → opcode table lookup → ops_*.c function call
          ↓
    Global LAGFX_OPCODES array
    Mixed routing + logic in single file
```

### After (New Architecture)
```
Doorbell → dispatcher switch-case → handler function call
           ↓
    Clean separation: routing vs logic
    Dispatcher logs "what", handlers log "how"
```

## Debugging Example

When CmdExecIndirect2 fires on channel 0:

1. **Dispatcher log**: `Channel0Dispatcher: routing opcode 0x20 (CmdExecIndirect2) → lagfx_compute_exec_indirect2`
   - Confirms opcode received and routing decision
   
2. **Handler log** (if enabled): `handler: executing logic function`
   - Confirms handler was called successfully

3. **Call chain visible**: Clear path from doorbell to final implementation

This makes it trivial to trace where commands go and identify bottlenecks or errors.

