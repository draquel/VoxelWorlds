// Copyright Daniel Raquel. All Rights Reserved.

#include "PCGVoxelBiomeDispatcher.h"
#include "VoxelPCGBiomeDecorationMapping.h"
#include "VoxelPCG.h"

#include "PCGCommon.h"
#include "PCGPin.h"
#include "PCGGraph.h"
#include "PCGSubgraph.h" // FPCGInputForwardingElement
#include "Data/PCGBasePointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Graph/PCGStackContext.h"

#define LOCTEXT_NAMESPACE "PCGVoxelBiomeDispatcher"

#if WITH_EDITOR
FText UPCGVoxelBiomeDispatcherSettings::GetNodeTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Routes biome-tagged surface points to per-biome decoration subgraphs via a data-driven "
		"BiomeID -> graph mapping (biomes as configuration). Biomes with no mapping entry are dropped.");
}
#endif

TArray<FPCGPinProperties> UPCGVoxelBiomeDispatcherSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	FPCGPinProperties& InPin = PinProperties.Emplace_GetRef(
		PCGVoxelBiomeDispatcherConstants::InputLabel, EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true, /*bAllowMultipleData=*/true,
		LOCTEXT("InputTooltip", "Surface points carrying a BiomeID attribute (from the Voxel Surface Sampler)."));
	InPin.SetRequiredPin();
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGVoxelBiomeDispatcherSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return PinProperties;
}

FPCGElementPtr UPCGVoxelBiomeDispatcherSettings::CreateElement() const
{
	return MakeShared<FPCGVoxelBiomeDispatcherElement>();
}

FPCGContext* FPCGVoxelBiomeDispatcherElement::Initialize(const FPCGInitializeElementParams& InParams)
{
	FPCGVoxelBiomeDispatcherContext* Context = new FPCGVoxelBiomeDispatcherContext();
	Context->InitFromParams(InParams);
	return Context;
}

bool FPCGVoxelBiomeDispatcherElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGVoxelBiomeDispatcherElement::Execute);
	FPCGVoxelBiomeDispatcherContext* Context = static_cast<FPCGVoxelBiomeDispatcherContext*>(InContext);
	check(Context);

	const UPCGVoxelBiomeDispatcherSettings* Settings = Context->GetInputSettings<UPCGVoxelBiomeDispatcherSettings>();
	check(Settings);

	// ---- Schedule phase: partition by biome and dispatch one subgraph per mapped biome ----
	if (!Context->bScheduledSubgraphs)
	{
		UVoxelPCGBiomeDecorationMapping* Mapping = Settings->BiomeMapping.LoadSynchronous();
		if (!Mapping)
		{
			PCGE_LOG(Warning, GraphAndLog, LOCTEXT("NoMapping",
				"Voxel Biome Dispatcher: no BiomeMapping assigned; no decoration generated."));
			return true;
		}

		const FPCGStack* CurrentStack = Context->GetStack();
		int32 LoopIndex = 0;

		for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(PCGVoxelBiomeDispatcherConstants::InputLabel))
		{
			const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Tagged.Data);
			if (!PointData || PointData->GetNumPoints() == 0)
			{
				continue;
			}

			// Group point indices by BiomeID.
			const UPCGMetadata* Metadata = PointData->ConstMetadata();
			const FPCGMetadataAttribute<int32>* BiomeAttr =
				Metadata ? Metadata->GetConstTypedAttribute<int32>(Settings->BiomeAttribute) : nullptr;
			const TConstPCGValueRange<int64> EntryKeys = PointData->GetConstMetadataEntryValueRange();

			const int32 NumPoints = PointData->GetNumPoints();
			TMap<int32, TArray<int32>> IndicesByBiome;
			for (int32 i = 0; i < NumPoints; ++i)
			{
				const int32 Biome = BiomeAttr ? BiomeAttr->GetValueFromItemKey(EntryKeys[i]) : 0;
				IndicesByBiome.FindOrAdd(Biome).Add(i);
			}

			for (const TPair<int32, TArray<int32>>& Pair : IndicesByBiome)
			{
				UPCGGraph* BiomeGraph = Mapping->GetGraphForBiome(static_cast<uint8>(Pair.Key));
				if (!BiomeGraph || Pair.Value.IsEmpty())
				{
					continue; // biome has no decoration graph -> drop
				}

				// Build this biome's point subset as the subgraph input.
				UPCGBasePointData* BiomeData = FPCGContext::NewPointData_AnyThread(Context);
				BiomeData->InitializeFromData(PointData);
				BiomeData->SetPointsFrom(PointData, Pair.Value);

				FPCGDataCollection BiomeInput;
				FPCGTaggedData& BiomeTagged = BiomeInput.TaggedData.Emplace_GetRef();
				BiomeTagged.Data = BiomeData;
				BiomeTagged.Pin = PCGPinConstants::DefaultInputLabel;

				// Unique invocation stack per dispatched subgraph (node frame + loop index).
				FPCGStack InvocationStack = CurrentStack ? *CurrentStack : FPCGStack();
				InvocationStack.GetStackFramesMutable().Emplace(Context->Node);
				InvocationStack.GetStackFramesMutable().Emplace(LoopIndex++);

				FPCGDataCollection EmptyUserParams;
				const FPCGTaskId TaskId = Context->ScheduleGraph(FPCGScheduleGraphParams(
					BiomeGraph,
					Context->ExecutionSource.Get(),
					MakeShared<FPCGInputForwardingElement>(EmptyUserParams),
					MakeShared<FPCGInputForwardingElement>(BiomeInput),
					/*ExternalDependencies=*/{},
					&InvocationStack,
					/*bAllowHierarchicalGeneration=*/false));

				if (TaskId != InvalidPCGTaskId)
				{
					Context->SubgraphTaskIds.Add(TaskId);
					Context->DynamicDependencies.Add(TaskId);
				}
			}
		}

		if (Context->SubgraphTaskIds.Num() > 0)
		{
			Context->bScheduledSubgraphs = true;
			Context->bIsPaused = true;
			return false; // wait for the subgraphs
		}

		// Nothing mapped -> empty output.
		return true;
	}

	// Paused waiting on the scheduled subgraphs.
	if (Context->bIsPaused)
	{
		return false;
	}

	// ---- Gather phase: merge each biome subgraph's output ----
	for (const FPCGTaskId TaskId : Context->SubgraphTaskIds)
	{
		FPCGDataCollection SubgraphOutput;
		if (Context->GetOutputData(TaskId, SubgraphOutput))
		{
			for (const FPCGTaggedData& Tagged : SubgraphOutput.TaggedData)
			{
				FPCGTaggedData& Out = Context->OutputData.TaggedData.Add_GetRef(Tagged);
				Out.Pin = PCGPinConstants::DefaultOutputLabel;
			}
			Context->ClearOutputData(TaskId);
		}
	}

	PCGE_LOG(Verbose, LogOnly, FText::Format(
		LOCTEXT("Dispatched", "Voxel Biome Dispatcher: merged {0} biome subgraph result(s)"),
		Context->SubgraphTaskIds.Num()));

	return true;
}

#undef LOCTEXT_NAMESPACE
