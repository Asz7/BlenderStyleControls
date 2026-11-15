#pragma once

#include "CoreMinimal.h"

class FEditorViewportClient;
class FViewport;
class FJsonValue;

void AppendBlenderTopologyVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures);
