#pragma once

#include "CoreMinimal.h"

class FEditorViewportClient;
class FViewport;
class FJsonValue;

/**
 * Targeted real-editor checks. Requires the dedicated -BlenderControlsVerify process
 * and its already-created, unsaved test world. Creates and removes only its own
 * fixture actors and unsaved mesh variants; never saves a package or user map.
 * InputKey coverage verifies mode routing, not physical keyboard/Slate focus routing.
 */
void AppendBlenderMeshEditVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures);
