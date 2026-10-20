#pragma once
#include "MenuWindowLayout.h"
#include <imgui/imgui_internal.h>

namespace MenuDocking
{
inline constexpr const char* WindowIdentity = "###OptiScalerMainMenu";
inline void SetInputFlags(ImGuiIO& io, ImGuiConfigFlags flags)
{
    constexpr ImGuiConfigFlags inputFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    io.ConfigFlags = (io.ConfigFlags & ~inputFlags) | flags | ImGuiConfigFlags_DockingEnable;
}
struct State
{
    ImGuiContext* context = nullptr;
    int builtAnchor = -1;
    bool wasDocked = false;
    // Submit before the main window, including while hidden: otherwise ImGui undocks its contents.
    void Submit(int anchor, bool visible, const char* title, const ImVec2& preferred)
    {
        auto* current = ImGui::GetCurrentContext();
        // Render entrypoints can legitimately skip NewFrame while all UI is hidden.
        // Even KeepAliveOnly dereferences the current window in this ImGui version.
        if (!current || !current->WithinFrameScope || !current->CurrentWindow)
            return;
        if (context != current) { context = current; builtAnchor = -1; wasDocked = false; }
        auto* viewport = ImGui::GetMainViewport();
        const ImGuiID dockId = ImHashStr("OptiScalerMenuDockSpace");
        anchor = MenuWindowLayout::ValidAnchor(anchor);
        const int side = MenuWindowLayout::IsDocked(anchor) ? anchor : 0;
        ImGuiWindowClass windowClass;
        windowClass.ClassId = ImHashStr("OptiScalerMenuClass");
        windowClass.DockingAllowUnclassed = false;
        if (builtAnchor != side || !ImGui::DockBuilderGetNode(dockId))
        {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodePos(dockId, viewport->WorkPos);
            ImGui::DockBuilderSetNodeSize(dockId, viewport->WorkSize);
            if (side)
            {
                const bool across = side == MenuWindowLayout::DockTop || side == MenuWindowLayout::DockBottom;
                const float extent = across ? viewport->WorkSize.y : viewport->WorkSize.x;
                const float desired = across ? preferred.y : preferred.x;
                const float ratio = std::clamp(desired / std::max(1.f, extent), .2f, .8f);
                const ImGuiDir direction = side == MenuWindowLayout::DockLeft ? ImGuiDir_Left :
                    side == MenuWindowLayout::DockRight ? ImGuiDir_Right :
                    side == MenuWindowLayout::DockTop ? ImGuiDir_Up : ImGuiDir_Down;
                ImGuiID node = 0;
                ImGui::DockBuilderSplitNode(dockId, direction, ratio, &node, nullptr);
                ImGui::DockBuilderDockWindow(title, node);
            }
            ImGui::DockBuilderFinish(dockId);
            builtAnchor = side;
            wasDocked = side != 0;
        }
        if (visible)
        {
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
            ImGui::Begin("##OptiScalerDockHost", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar(3);
            ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode |
                ImGuiDockNodeFlags_NoDockingOverCentralNode, &windowClass);
            ImGui::End();
        }
        else
            ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly, &windowClass);
        if (visible) ImGui::SetNextWindowClass(&windowClass);
    }
    bool ObserveUndocked(bool docked)
    {
        const bool detached = wasDocked && !docked;
        wasDocked = docked;
        return detached;
    }
};
}
