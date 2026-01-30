# Coding Standards

Project-specific coding conventions and best practices.

## Unreal Engine Conventions

Follow standard UE coding conventions:
- Classes: `FClassName`, `UClassName`, `AClassName`
- Interfaces: `IInterfaceName`
- Structs: `FStructName`
- Enums: `EEnumName`
- Member variables: `MemberName`, `bBoolMember`

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

## Module Organization

### Dependency Rules
```
VoxelCore: No dependencies on other modules
VoxelLOD: Depends only on VoxelCore
VoxelRendering: Depends only on VoxelCore
VoxelStreaming: Depends on VoxelCore + VoxelLOD
```

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
 * @see Documentation/LOD_SYSTEM.md
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

### GPU-First Principle
1. Keep data on GPU whenever possible
2. Minimize CPU→GPU transfers
3. No readback in runtime path
4. Batch operations (multiple chunks per dispatch)

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
```cpp
DECLARE_LOG_CATEGORY_EXTERN(LogVoxel, Log, All);

UE_LOG(LogVoxel, Log, TEXT("Message"));
UE_LOG(LogVoxel, Warning, TEXT("Warning: %s"), *String);
UE_LOG(LogVoxel, Error, TEXT("Error: %d"), Value);
```

### Error Handling
```cpp
// For expected errors
if (!IsValid()) {
    UE_LOG(LogVoxel, Warning, TEXT("Invalid state"));
    return false;
}

// For unexpected errors
if (!CriticalCondition) {
    UE_LOG(LogVoxel, Error, TEXT("Critical failure"));
    ensure(false); // Log callstack
    return;
}
```

## Testing

### Unit Tests
```cpp
TEST(VoxelCore, VoxelDataPacking) {
    FVoxelData Data;
    Data.MaterialID = 42;
    Data.Density = 127;
    
    EXPECT_EQ(Data.MaterialID, 42);
    EXPECT_EQ(Data.Density, 127);
}
```

### Integration Tests
```cpp
TEST(VoxelLOD, DistanceBandBasic) {
    FDistanceBandLODStrategy Strategy;
    // Configure...
    
    FLODQueryContext Context;
    Context.ViewerPosition = FVector::ZeroVector;
    
    int32 LOD = Strategy.GetLODForChunk(FIntVector(0,0,0), Context);
    EXPECT_EQ(LOD, 0); // Should be finest LOD at origin
}
```

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

❌ CPU readback in runtime path
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

