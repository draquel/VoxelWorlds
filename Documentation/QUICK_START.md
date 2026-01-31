# Quick Start Guide

Get up and running with VoxelWorlds plugin development.

## Prerequisites

- Unreal Engine 5.7.2
- Visual Studio 2022 or Rider
- Basic C++ knowledge
- Understanding of UE plugin architecture

## Project Setup

### 1. Create Unreal Project

```bash
# Create new project
UE5Editor.exe -NewProject "VoxelEngine" -projecttype=Blank
```

### 2. Create Plugin

In your project:
```
YourProject/
└── Plugins/
    └── VoxelWorlds/  ← Create this
```

### 3. Add Documentation

Copy all documentation files:
```
Plugins/VoxelWorlds/
├── README.md
├── .claude/
│   └── instructions.md
└── Documentation/
    ├── ARCHITECTURE.md
    ├── LOD_SYSTEM.md
    ├── DATA_STRUCTURES.md
    ├── CODING_STANDARDS.md
    ├── IMPLEMENTATION_PHASES.md
    └── ... (all other docs)
```

## First Steps

### Phase 1 Task 1: Create VoxelCore Module

**1. Create directory structure:**
```
Plugins/VoxelWorlds/Source/VoxelCore/
├── VoxelCore.Build.cs
├── Public/
│   ├── VoxelTypes.h
│   └── VoxelCoreModule.h
└── Private/
    └── VoxelCoreModule.cpp
```

**2. Create VoxelCore.Build.cs:**
```csharp
using UnrealBuildTool;

public class VoxelCore : ModuleRules
{
    public VoxelCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        
        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine"
        });
    }
}
```

**3. Create VoxelTypes.h:**
```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * FVoxelData - 4 bytes per voxel
 * 
 * Packed voxel data structure.
 * See Documentation/DATA_STRUCTURES.md for details.
 */
struct FVoxelData
{
    uint8 MaterialID;   // 0-255 material types
    uint8 Density;      // 0-255 (127 = surface)
    uint8 BiomeID;      // 0-255 biome types
    uint8 Metadata;     // AO + flags
};
```

**4. Add to VoxelWorlds.uplugin:**
```json
{
    "Modules": [
        {
            "Name": "VoxelCore",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        }
    ]
}
```

**5. Compile:**
```bash
# In Rider: Build > Build Solution
# Or command line:
UE5Editor-Cmd.exe YourProject.uproject -run=UnrealBuildTool -mode=build
```

### Phase 1 Task 2: Implement FChunkDescriptor

Add to VoxelTypes.h:
```cpp
/**
 * FChunkDescriptor
 * 
 * Metadata and contents for a single chunk.
 */
struct FChunkDescriptor
{
    FIntVector ChunkCoord;
    int32 LODLevel;
    int32 ChunkSize;
    
    TArray<FVoxelData> VoxelData;
    
    FBox Bounds;
    bool bIsDirty;
    bool bHasEdits;
    float MorphFactor;
    uint32 GenerationSeed;
};
```

## Development Workflow

### Using Claude Code in Rider

**1. Open claude.code panel**
**2. Reference documentation:**
```
"According to Documentation/LOD_SYSTEM.md, create the IVoxelLODStrategy interface"
```

**3. Iterate on implementation:**
```
"Add the GetLODMorphFactor method to IVoxelLODStrategy"
"Does this follow CODING_STANDARDS.md?"
```

### Typical Session

1. **Start**: Read relevant documentation
2. **Implement**: Create module/class/interface
3. **Test**: Compile and verify
4. **Iterate**: Refine based on standards
5. **Commit**: Update IMPLEMENTATION_PHASES.md

## Common Issues

### Linker Errors

**Problem**: Module not found
**Solution**: Check .uplugin has module listed
```json
{
    "Modules": [
        {"Name": "VoxelCore", "Type": "Runtime"}
    ]
}
```

### Include Errors

**Problem**: Cannot find header
**Solution**: Check .Build.cs dependencies
```csharp
PublicDependencyModuleNames.AddRange(new string[] {
    "Core", "CoreUObject", "Engine"
});
```

### Compilation Slow

**Problem**: Full rebuild every time
**Solution**: Use PCH correctly
```csharp
PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
```

## Next Steps

After Phase 1 foundation:
1. **Phase 2**: GPU noise generation
2. **Phase 3**: Cubic meshing
3. **Phase 4**: Custom Vertex Factory

See IMPLEMENTATION_PHASES.md for complete roadmap.

## Resources

- **Documentation**: `Plugins/VoxelWorlds/Documentation/`
- **UE Docs**: https://docs.unrealengine.com/
- **Claude Code**: Use `.claude/instructions.md` for context

## Getting Help

1. Check relevant documentation first
2. Use Claude Code for implementation help
3. Review CODING_STANDARDS.md for conventions
4. Check ARCHITECTURE.md for design decisions

