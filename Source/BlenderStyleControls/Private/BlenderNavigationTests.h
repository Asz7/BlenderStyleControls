#pragma once

#include "CoreMinimal.h"

class FEditorViewportClient;
class FViewport;
class FJsonValue;

/** Real editor camera checks, no OS input or actor changes. Requires dedicated verification process. */
void AppendBlenderNavigationVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures);
