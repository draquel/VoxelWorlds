# Coding Standards

> State as of 2026-11-02 (code review CR5).

Project-specific coding conventions and best practices. The names below (`FClassName`, `IVoxel*`, ...) are patterns,
not types. The plugin's rules of architecture (seam ownership, world modes, console names) are in
`.claude/instructions.md`.

## Unreal Engine Conventions

Follow standard UE coding conventions:
- Classes: `FClassName`, `UClassName`, `AClassName`
- Interfaces: `IInterfaceName`
- Structs: `FStructName`
- Enums: `EEnumName`
- Member variables: `MemberName`, `bBoolMember`

## Blueprint Type Compatibility

**IMPORTANT**: When exposing properties to Blueprints (`BlueprintReadWrite`), use only Blueprint-compatible types:

| C++ Type | Blueprint Support | Alternative |
|----------|-------------------|-------------|
| `uint8`  | ✅ Yes (as "Byte") | - |
| `int8`   | ❌ No | `int32` |
| `uint16` | ❌ No | `int32` |
| `int16`  | ❌ No | `int32` |
| `uint32` | ❌ No | `int32` or `int64` |
| `int32`  | ✅ Yes | - |
| `uint64` | ❌ No | `int64` |
| `int64`  | ✅ Yes | - |

**Guidelines**:
- For counters/seeds where full uint32 range isn't needed: use `int32`
- For frame numbers or timestamps: use `int64`
- For byte-sized data (0-255): `uint8` is fine
- Internal-only fields (no `BlueprintReadWrite`): any type is acceptable
- Add comment noting "for Blueprint compatibility" when using signed alternatives

## UE5 API Deprecations

Track deprecated APIs to maintain forward compatibility with future engine versions.

### TArray::Pop / TArray::RemoveAt Shrinking Parameter

**Deprecated (UE 5.5+)**: `TArray::Pop(bool bAllowShrinking)` and similar methods with boolean shrinking parameter.

**New API**: Use `EAllowShrinking` enum instead.

```cpp
// ❌ Deprecated
TArray<int32> Arr;
int32 Value = Arr.Pop(false);           // Warning C4996
Arr.RemoveAt(0, 1, false);              // Warning C4996

// ✅ Correct
int32 Value = Arr.Pop(EAllowShrinking::No);
Arr.RemoveAt(0, 1, EAllowShrinking::No);

// Or allow shrinking:
int32 Value = Arr.Pop(EAllowShrinking::Yes);
Arr.Pop();  // Default behavior (allows shrinking)
```

## Naming Conventions

### Files
- Headers: `ClassName.h`
- Implementation: `ClassName.cpp`
- Shaders: `ShaderName.usf`

### Voxel-Specific Prefixes
- `FVoxel*` - Voxel-specific structs
- `UVoxel*` - Voxel UObject classes
- `AVoxel*` - Voxel actors
- `IVoxel*` - Voxel interfaces

A type, function or cvar that works in every world mode carries no mode in its name (`FVoxelFarField`, not
`FVoxelPlanetFarField`); a rename of a reflected name adds a `[CoreRedirects]` entry in `Config/DefaultVoxelWorlds.ini`.
Console names are `voxel.<System>.<Name>`, grouped by the system that reads the value.

## Module Organization

### Dependency Rules
- `VoxelCore` depends on no other voxel module; `VoxelMeshing` and `VoxelRendering` only on `VoxelCore`.
- `VoxelStreaming` is the hub; `VoxelMap`, `VoxelNavigation` and `VoxelPCG` are its clients, and nothing in the plugin
  depends on them.
- No voxel module depends on a game plugin. [ARCHITECTURE.md](../ARCHITECTURE.md) has the full table.
- A module dependency added only to link (no include) says so in a comment in `Build.cs`.

### File Structure
```cpp
// 1. Copyright/License
// 2. #pragma once or header guard
// 3. Engine includes
// 4. Plugin includes (relative)
// 5. Forward declarations
// 6. Class/Struct definitions
// 7. Inline implementations (if any)
```

## Documentation

### Required Documentation
All public interfaces must have Doxygen comments:

```cpp
/**
 * Brief description.
 * 
 * Detailed description with usage examples.
 * 
 * Performance: O(n), Memory: X bytes
 * Thread Safety: Thread-safe/Not thread-safe
 * 
 * @param ParamName Parameter description
 * @return Return value description
 * @see RelatedClass
 */
```

### Example
```cpp
/**
 * LOD strategy interface for voxel terrain.
 * 
 * Implementations determine which chunks load at what LOD.
 * All methods must be fast (< 1μs per call).
 * 
 * Thread Safety: Read-only methods are thread-safe.
 * 
 * @see FDistanceBandLODStrategy
 * @see Documentation/Features/LOD_SYSTEM.md
 */
class IVoxelLODStrategy {
    /**
     * Get LOD level for chunk.
     * 
     * Called frequently (per chunk per frame).
     * Must be fast and thread-safe.
     * 
     * @param ChunkCoord Chunk position
     * @param Context Query context
     * @return LOD level (0 = finest)
     */
    virtual int32 GetLODForChunk(
        FIntVector ChunkCoord,
        const FLODQueryContext& Context
    ) const = 0;
};
```

## Performance Guidelines

### Threads
1. Generation runs on the GPU (one readback per chunk) or on a worker; meshing, seams, collision cooks, scatter
   extraction and far-compression encodes run on workers
2. The game thread schedules, installs results and answers queries; it never meshes, decodes or encodes a chunk
3. Workers read voxels from immutable snapshots, never from a descriptor's array
4. Budget per-tick work (caps per tick, time slices) rather than letting a queue drain in one frame

### Memory Guidelines
1. Use packed data structures
2. Pool memory allocations
3. Cache-friendly layouts (contiguous memory)
4. Clear unused memory promptly

### Profiling
- Profile before optimizing
- Use Unreal Insights for GPU profiling
- Measure, don't guess
- Document performance characteristics

## Code Quality

### Assertions
```cpp
check(Condition);              // Always enabled
checkSlow(Condition);          // Development only
ensure(Condition);             // Logs but continues
verify(Expression);            // Check + execute
```

### Logging
One category per module (`LogVoxelCore`, `LogVoxelStreaming`, `LogVoxelMeshing`, ...):
```cpp
UE_LOG(LogVoxelStreaming, Log, TEXT("Message"));
UE_LOG(LogVoxelStreaming, Warning, TEXT("Warning: %s"), *String);
UE_LOG(LogVoxelStreaming, Error, TEXT("Error: %d"), Value);
```

### Error Handling
```cpp
// For expected errors
if (!IsValid()) {
    UE_LOG(LogVoxelCore, Warning, TEXT("Invalid state"));
    return false;
}

// For unexpected errors
if (!CriticalCondition) {
    UE_LOG(LogVoxelCore, Error, TEXT("Critical failure"));
    ensure(false); // Log callstack
    return;
}
```

## Testing

Tests are UE automation tests in each module's `Tests/` folder, named `VoxelWorlds.<System>.<Case>`:
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDataPackingTest, "VoxelWorlds.Core.VoxelDataPacking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelDataPackingTest::RunTest(const FString& Parameters)
{
	const FVoxelData Data(42, 127);
	TestEqual(TEXT("MaterialID survives a pack"), (int32)FVoxelData::Unpack(Data.Pack()).MaterialID, 42);
	return true;
}
```
A test that needs a real RHI (GPU generation, a scene proxy) skips itself under `-nullrhi` and must be run in the
editor; [TESTING_STRATEGY.md](TESTING_STRATEGY.md) has the suites and the gate.

## Git Workflow

### Commit Messages
```
[Module] Brief description

Detailed explanation of changes.

- Bullet point details
- References to issues/docs

Closes #123
```

### Branch Naming
```
feature/lod-system
bugfix/collision-crash
refactor/chunk-manager
```

## Common Patterns

### Interface Implementation
```cpp
// 1. Define interface
class IVoxelSystem {
    virtual void DoWork() = 0;
};

// 2. Implement
class FVoxelSystemImpl : public IVoxelSystem {
public:
    virtual void DoWork() override { }
};

// 3. Wrap in UObject (if needed)
UCLASS()
class UVoxelSystemWrapper : public UObject {
    TUniquePtr<IVoxelSystem> Impl;
};
```

### Render Commands
```cpp
ENQUEUE_RENDER_COMMAND(CommandName)([
    Param1,
    Param2
](FRHICommandListImmediate& RHICmdList) {
    // Render thread code
});
```

### Async Tasks
```cpp
AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []() {
    // Worker thread code
    
    AsyncTask(ENamedThreads::GameThread, []() {
        // Back to game thread
    });
});
```

## Anti-Patterns (Avoid)

❌ Meshing, decoding or encoding a chunk on the game thread
❌ Synchronous GPU waits
❌ Memory allocations per frame
❌ Deep recursion
❌ Virtual calls in tight loops (if avoidable)
❌ String operations in performance-critical code

## Checklist

Before submitting code:
- [ ] Follows naming conventions
- [ ] Public APIs documented
- [ ] No compiler warnings
- [ ] Tested locally
- [ ] Performance considered
- [ ] Memory leaks checked
- [ ] Thread safety considered

