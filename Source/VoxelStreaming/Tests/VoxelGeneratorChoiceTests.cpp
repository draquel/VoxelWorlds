// Copyright Daniel Raquel. All Rights Reserved.

// Where a chunk manager generates (G289: a dedicated server process has no renderer and no global shader map, and its
// first GPU dispatch asserted in GetGlobalShaderMap):
// - Rules: UVoxelChunkManager::ChooseGenerator over every combination of its four inputs: the world's configuration
//   first, then -VoxelForceCPU, then voxel.Stream.GPUGeneration, then the process; the GPU only when all four allow it.
// - ThisProcess: FVoxelGPUNoiseGenerator::CanRunInThisProcess is the renderer's own question (FApp::CanEverRender) here,
//   with a reason when it says no; and a chunk manager initialized on a world configured for the GPU chooses what the
//   rules give for this process: the CPU, the process named, in the headless suite (-nullrhi, a process that cannot run
//   the GPU path, as a dedicated server process cannot), the GPU in an editor with a real RHI.

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGeneratorChoiceRulesTest, "VoxelWorlds.Streaming.GeneratorChoice.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGeneratorChoiceRulesTest::RunTest(const FString& Parameters)
{
	for (int32 Bits = 0; Bits < 16; ++Bits)
	{
		const bool bConfig = (Bits & 1) != 0;
		const bool bSwitch = (Bits & 2) != 0;
		const bool bCVar = (Bits & 4) != 0;
		const bool bProcess = (Bits & 8) != 0;
		const EVoxelGeneratorChoice Expected = !bConfig ? EVoxelGeneratorChoice::CPUConfigured
			: bSwitch ? EVoxelGeneratorChoice::CPUForcedBySwitch
			: !bCVar ? EVoxelGeneratorChoice::CPUForcedByCVar
			: !bProcess ? EVoxelGeneratorChoice::CPUProcessCannotRunGPU
			: EVoxelGeneratorChoice::GPU;
		const EVoxelGeneratorChoice Got = UVoxelChunkManager::ChooseGenerator(bConfig, bSwitch, bCVar, bProcess);
		TestEqual(*FString::Printf(TEXT("config=%d switch=%d cvar=%d process=%d: %s"), bConfig, bSwitch, bCVar, bProcess,
			UVoxelChunkManager::GeneratorChoiceToString(Expected)), static_cast<int32>(Got), static_cast<int32>(Expected));
	}
	// The case G289 is about, by name: a world on the GPU in a process that cannot run it generates on the CPU.
	TestEqual(TEXT("a dedicated server process on a GPU world generates on the CPU"),
		static_cast<int32>(UVoxelChunkManager::ChooseGenerator(true, false, true, false)),
		static_cast<int32>(EVoxelGeneratorChoice::CPUProcessCannotRunGPU));
	TestEqual(TEXT("a process that renders (the editor, PIE's dedicated server, a listen server, a client) generates on the GPU"),
		static_cast<int32>(UVoxelChunkManager::ChooseGenerator(true, false, true, true)),
		static_cast<int32>(EVoxelGeneratorChoice::GPU));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGeneratorChoiceThisProcessTest, "VoxelWorlds.Streaming.GeneratorChoice.ThisProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGeneratorChoiceThisProcessTest::RunTest(const FString& Parameters)
{
	FString WhyNot;
	const bool bCanRun = FVoxelGPUNoiseGenerator::CanRunInThisProcess(&WhyNot);
	if (!FApp::CanEverRender())
	{
		TestFalse(TEXT("a process that cannot render cannot run the GPU generator"), bCanRun);
		TestFalse(TEXT("and says why"), WhyNot.IsEmpty());
	}
	else
	{
		// An editor with a real RHI: the renderer and the global shader map are there.
		TestTrue(TEXT("a process that renders can run the GPU generator"), bCanRun);
	}
	AddInfo(FString::Printf(TEXT("this process: CanEverRender=%d, the GPU generator %s"), FApp::CanEverRender() ? 1 : 0,
		bCanRun ? TEXT("can run") : *FString::Printf(TEXT("cannot run (%s)"), *WhyNot)));

	// A chunk manager on a world configured for the GPU (only initialized: nothing is generated).
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelGeneratorChoiceTest"));
	W.ConfigureSmallPlane();
	W.Config->bUseGPUGeneration = true;
	W.StartManager();
	const bool bSwitch = FParse::Param(FCommandLine::Get(), TEXT("VoxelForceCPU"));
	const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.GPUGeneration"));
	const bool bCVar = !CVar || CVar->GetInt() != 0;
	const EVoxelGeneratorChoice Expected = UVoxelChunkManager::ChooseGenerator(true, bSwitch, bCVar, bCanRun);
	TestEqual(*FString::Printf(TEXT("the chunk manager chose %s"), UVoxelChunkManager::GeneratorChoiceToString(Expected)),
		static_cast<int32>(W.Manager->GetGeneratorChoice()), static_cast<int32>(Expected));
	TestTrue(TEXT("and generates on the GPU exactly when it chose it"),
		W.Manager->IsUsingGPUGeneration() == (Expected == EVoxelGeneratorChoice::GPU));
	if (!bCanRun && !bSwitch && bCVar)
	{
		TestEqual(TEXT("a process that cannot run the GPU path generates on the CPU by itself"),
			static_cast<int32>(W.Manager->GetGeneratorChoice()), static_cast<int32>(EVoxelGeneratorChoice::CPUProcessCannotRunGPU));
	}
	AddInfo(FString::Printf(TEXT("a GPU world's chunk manager here: %s"), UVoxelChunkManager::GeneratorChoiceToString(W.Manager->GetGeneratorChoice())));

	// The configuration's own choice still comes first.
	W.Stop();
	VoxelTesting::FTestChunkWorld C;
	C.Create(TEXT("VoxelGeneratorChoiceTestCPU"));
	C.ConfigureSmallPlane();
	C.StartManager();
	TestEqual(TEXT("a world configured for the CPU generates on the CPU"),
		static_cast<int32>(C.Manager->GetGeneratorChoice()), static_cast<int32>(EVoxelGeneratorChoice::CPUConfigured));
	TestFalse(TEXT("with the CPU generator"), C.Manager->IsUsingGPUGeneration());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
