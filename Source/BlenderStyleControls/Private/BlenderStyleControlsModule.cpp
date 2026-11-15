#include "Modules/ModuleManager.h"
#include "BlenderStyleEdMode.h"
#include "BlenderStyleControlsTests.h"
#include "BlenderControlsVisualTests.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"
#include "Containers/Ticker.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"

#define LOCTEXT_NAMESPACE "BlenderStyleControls"

class FBlenderStyleControlsModule : public IModuleInterface
{
    TArray<IConsoleObject*> Commands;
    TWeakPtr<SWidget> ToggleWidget;
    FTSTicker::FDelegateHandle VerificationTicker;

    void QueueVerification()
    {
        if (!FParse::Param(FCommandLine::Get(),TEXT("BlenderControlsVerify")) || VerificationTicker.IsValid()) return;
        VerificationTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this,Attempts=0](float) mutable {
            bool Ready=false;
            if (GEditor && FSlateApplication::IsInitialized())
                for (auto* Client:GEditor->GetLevelViewportClients())
                    Ready |= Client && Client->Viewport && Client->Viewport->GetSizeXY().GetMin()>0;
            if (!Ready && ++Attempts<30) return true;
            VerificationTicker.Reset();
            RunBlenderStyleControlsVerification();
            return false;
        }),1.0f);
    }

    static bool IsEnabled()
    {
        return GEditor && GLevelEditorModeTools().IsModeActive(FBlenderStyleEdMode::ModeId);
    }

    static bool CanToggle() { return GEditor && !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor; }

    static void SetEnabled(bool bEnabled)
    {
        if (!CanToggle()) return;
        if (bEnabled) GLevelEditorModeTools().ActivateMode(FBlenderStyleEdMode::ModeId);
        else GLevelEditorModeTools().DeactivateMode(FBlenderStyleEdMode::ModeId);
        GEditor->RedrawLevelEditingViewports();
    }

    static void Toggle() { SetEnabled(!IsEnabled()); }

    static void NotifyHelp()
    {
        FNotificationInfo Info(LOCTEXT("QuickHelp",
            "开启后，将鼠标移入关卡视口：\n中键环绕 · Shift+中键平移 · 滚轮/Ctrl+中键缩放 · 小键盘.聚焦\nTab 多物体编辑/提交 · 1点/2边/3面 · Shift点击加减选\nB 框选 · Alt点击循环 · Ctrl+Alt点击环带 · Ctrl点击最短路径\nCtrl+Shift点击填充路径区域 · Ctrl小键盘±扩选/缩选 · L连选\nE 面挤出（cm）· Alt+E 挤出菜单 · XYZ锁世界轴\nCtrl+R 单条四边面环切 · 左键插入后滑移 · 再次左键确认\n挤出 Esc/右键保留零距离挤出；环切滑移 Esc/右键保留居中切线\nCtrl+Z 可撤销整次挤出/环切 · Tab 提交后按需保存\n右上角切着色 · Z菜单 · Shift+Z线框 · Alt+Z透视\nG/R/S 变换 · XYZ彩色锁轴 · Shift+A添加 · Shift+D复制"));
        Info.ExpireDuration = 15.0f;
        Info.bUseLargeFont = false;
        FSlateNotificationManager::Get().AddNotification(Info);
    }

    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        auto Button = SNew(SButton)
            .IsFocusable(false)
            .ContentPadding(FMargin(12,5))
            .IsEnabled_Static(&CanToggle)
            .ButtonColorAndOpacity_Lambda([] { return IsEnabled() ? FLinearColor(.12f,.54f,.34f) : FLinearColor(.19f,.19f,.19f); })
            .ToolTipText(LOCTEXT("ToggleTip", "开启或关闭 Blender 式关卡操作。只影响关卡视口；关闭时恢复 UE 默认键位。"))
            .OnClicked_Lambda([] { Toggle(); return FReply::Handled(); })
            [SNew(STextBlock).Text_Lambda([] { return IsEnabled() ? LOCTEXT("On", "Blender 操作：开") : LOCTEXT("Off", "Blender 操作：关"); })];
        ToggleWidget = Button;
        UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User"))
            ->FindOrAddSection(TEXT("BlenderStyleControls"))
            .AddEntry(FToolMenuEntry::InitWidget(TEXT("BlenderStyleControls.Toggle"), Button, LOCTEXT("ButtonTitle", "Blender 操作")));

        auto& Section = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"))->FindOrAddSection(TEXT("BlenderStyleControls"));
        Section.AddMenuEntry(TEXT("BlenderStyleControls.Toggle"), LOCTEXT("MenuToggle", "Blender 操作开关"),
            LOCTEXT("MenuTip", "勾选时启用 Blender 式关卡快捷操作"),
            FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
            FUIAction(FExecuteAction::CreateStatic(&Toggle), FCanExecuteAction::CreateStatic(&CanToggle), FIsActionChecked::CreateStatic(&IsEnabled)),
            EUserInterfaceActionType::ToggleButton);
        Section.AddMenuEntry(TEXT("BlenderStyleControls.Help"), LOCTEXT("HelpTitle", "Blender 操作说明"),
            LOCTEXT("HelpTip", "显示快捷键与单位说明"), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&NotifyHelp)));
    }

    void CaptureUI()
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("BlenderControlsVerify"))) return;
        const FString Folder=FPaths::ProjectSavedDir()/TEXT("BlenderStyleControls");
        IFileManager::Get().MakeDirectory(*Folder,true);
        if (auto Widget=ToggleWidget.Pin())
        {
            TArray<FColor> Pixels; FIntVector Size;
            if (FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(),Pixels,Size))
            {
                for (auto& P:Pixels) P.A=255;
                TArray<uint8> Png; FImageUtils::CompressImageArray(Size.X,Size.Y,Pixels,Png);
                FFileHelper::SaveArrayToFile(Png,*(Folder/(IsEnabled()?TEXT("button_on.png"):TEXT("button_off.png"))));
            }
        }
        if (auto Window=FSlateApplication::Get().GetActiveTopLevelWindow())
        {
            TArray<FColor> Pixels; FIntVector Size;
            if (FSlateApplication::Get().TakeScreenshot(Window->GetContent(),Pixels,Size))
            {
                for (auto& P:Pixels) P.A=255;
                TArray<uint8> Png; FImageUtils::CompressImageArray(Size.X,Size.Y,Pixels,Png);
                FFileHelper::SaveArrayToFile(Png,*(Folder/(IsEnabled()?TEXT("editor_on.png"):TEXT("editor_off.png"))));
            }
        }
    }

public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        FEditorModeRegistry::Get().RegisterMode<FBlenderStyleEdMode>(FBlenderStyleEdMode::ModeId,
            LOCTEXT("ModeTitle", "Blender 操作"), FSlateIcon(), false);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,&FBlenderStyleControlsModule::RegisterMenus));
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.Toggle"),TEXT("Toggle Blender-style level controls"),FConsoleCommandDelegate::CreateStatic(&Toggle)));
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.On"),TEXT("Enable Blender-style level controls"),FConsoleCommandDelegate::CreateLambda([]{SetEnabled(true);}))); 
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.Off"),TEXT("Disable Blender-style level controls"),FConsoleCommandDelegate::CreateLambda([]{SetEnabled(false);}))); 
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.Help"),TEXT("Show shortcut help"),FConsoleCommandDelegate::CreateStatic(&NotifyHelp)));
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.Verify"),TEXT("Run temporary-level tests; requires -BlenderControlsVerify"),FConsoleCommandDelegate::CreateRaw(this,&FBlenderStyleControlsModule::QueueVerification)));
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.CaptureUI"),TEXT("Capture toggle button for verification"),FConsoleCommandDelegate::CreateRaw(this,&FBlenderStyleControlsModule::CaptureUI)));
        Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(TEXT("BlenderControls.VisualVerify"),TEXT("Render axis and multi-object edit verification; dedicated test process only"),FConsoleCommandDelegate::CreateStatic(&RunBlenderControlsVisualVerification)));
    }

    virtual void ShutdownModule() override
    {
        if (IsRunningCommandlet()) return;
        if (VerificationTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(VerificationTicker);
        if (GEditor) GLevelEditorModeTools().DeactivateMode(FBlenderStyleEdMode::ModeId);
        for (auto* Command:Commands) IConsoleManager::Get().UnregisterConsoleObject(Command);
        Commands.Empty();
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        FEditorModeRegistry::Get().UnregisterMode(FBlenderStyleEdMode::ModeId);
        ToggleWidget.Reset();
    }
};

IMPLEMENT_MODULE(FBlenderStyleControlsModule, BlenderStyleControls)
#undef LOCTEXT_NAMESPACE
