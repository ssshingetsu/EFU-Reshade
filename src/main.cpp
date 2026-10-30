// Endfield Uncensored - ReShade add-on
// Port of the runtime patch from DynamiByte/Endfield-Uncensored (src/dll.zig) to a ReShade add-on.
// SPDX-License-Identifier: AGPL-3.0-only
//
// What it does: after CameraMono._ProcessDitherByPitch runs each frame, call CameraMono.ForceClearDither
// so the pitch-based transparency dither on characters never sticks.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <MinHook.h>

#include <atomic>
#include <cstdint>

extern "C" __declspec(dllexport) const char *NAME = "Endfield Uncensored";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Removes Arknights: Endfield's camera-pitch transparency dither. Toggle with Alt+F12 or in the add-on settings.";

namespace
{
constexpr const char *CONFIG_SECTION = "EndfieldUncensored";
constexpr DWORD RESOLVE_TIMEOUT_MS = 5 * 60 * 1000; // give up if IL2CPP never comes up
constexpr DWORD POLL_MS = 250;

// IL2CPP surface (C ABI exports of GameAssembly.dll)
struct Il2CppMethodInfo { uintptr_t address; /* methodPointer is the first field */ };
using il2cpp_domain_get_t = void *(*)();
using il2cpp_domain_assembly_open_t = void *(*)(void *domain, const char *name);
using il2cpp_assembly_get_image_t = void *(*)(void *assembly);
using il2cpp_class_from_name_t = void *(*)(void *image, const char *ns, const char *name);
using il2cpp_class_get_method_from_name_t = Il2CppMethodInfo *(*)(void *klass, const char *name, int argc);
using instance_void_method_t = void (*)(void *instance, const Il2CppMethodInfo *method);

enum class status : int { waiting_for_game, resolving, active, failed_resolve, failed_hook, timed_out };

HMODULE g_module = nullptr;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_stop{false};
std::atomic<status> g_status{status::waiting_for_game};
std::atomic<bool> g_hook_installed{false};

const Il2CppMethodInfo *g_force_clear = nullptr;
instance_void_method_t g_original_pitch = nullptr;
void *g_pitch_target = nullptr;

bool g_combo_was_down = false;

const char *status_text(status s)
{
	switch (s)
	{
	case status::waiting_for_game: return "Waiting for GameAssembly.dll...";
	case status::resolving: return "Waiting for IL2CPP / CameraMono...";
	case status::active: return "Active";
	case status::failed_resolve: return "Failed: CameraMono methods not found (game update changed names?)";
	case status::failed_hook: return "Failed: could not install hook";
	case status::timed_out: return "Failed: timed out waiting for the game";
	}
	return "?";
}

// Hook
void camera_pitch_hook(void *instance, const Il2CppMethodInfo *method)
{
	g_original_pitch(instance, method);

	if (!g_enabled.load(std::memory_order_acquire))
		return;
	const auto force_clear = reinterpret_cast<instance_void_method_t>(g_force_clear->address);
	force_clear(instance, g_force_clear);
}

// Target resolution
struct targets { Il2CppMethodInfo *process_pitch; Il2CppMethodInfo *force_clear; };

bool try_resolve(HMODULE game_assembly, targets &out)
{
	const auto domain_get = reinterpret_cast<il2cpp_domain_get_t>(GetProcAddress(game_assembly, "il2cpp_domain_get"));
	const auto assembly_open = reinterpret_cast<il2cpp_domain_assembly_open_t>(GetProcAddress(game_assembly, "il2cpp_domain_assembly_open"));
	const auto assembly_get_image = reinterpret_cast<il2cpp_assembly_get_image_t>(GetProcAddress(game_assembly, "il2cpp_assembly_get_image"));
	const auto class_from_name = reinterpret_cast<il2cpp_class_from_name_t>(GetProcAddress(game_assembly, "il2cpp_class_from_name"));
	const auto method_from_name = reinterpret_cast<il2cpp_class_get_method_from_name_t>(GetProcAddress(game_assembly, "il2cpp_class_get_method_from_name"));
	if (!domain_get || !assembly_open || !assembly_get_image || !class_from_name || !method_from_name)
		return false;

	void *const domain = domain_get();
	if (!domain) return false;
	void *const assembly = assembly_open(domain, "Gameplay.Beyond.dll");
	if (!assembly) return false;
	void *const image = assembly_get_image(assembly);
	if (!image) return false;
	void *const camera_class = class_from_name(image, "Beyond.Gameplay.View", "CameraMono");
	if (!camera_class) return false;

	out.process_pitch = method_from_name(camera_class, "_ProcessDitherByPitch", 0);
	out.force_clear = method_from_name(camera_class, "ForceClearDither", 0);
	return out.process_pitch && out.force_clear && out.process_pitch->address && out.force_clear->address;
}

// Worker: ReShade loads add-ons at graphics init, which is before IL2CPP is ready, so wait for it here.
DWORD WINAPI worker(LPVOID)
{
	const DWORD start = GetTickCount();
	const auto timed_out = [start] { return GetTickCount() - start > RESOLVE_TIMEOUT_MS; };

	HMODULE game_assembly = nullptr;
	while (!(game_assembly = GetModuleHandleW(L"GameAssembly.dll")))
	{
		if (g_stop || timed_out()) { g_status = status::timed_out; goto done; }
		Sleep(POLL_MS);
	}

	{
		g_status = status::resolving;
		targets t{};
		bool resolved = false;
		while (!(resolved = try_resolve(game_assembly, t)))
		{
			if (g_stop || timed_out()) break;
			Sleep(POLL_MS);
		}
		if (g_stop) goto done;
		if (!resolved) { g_status = status::failed_resolve; goto done; }

		g_force_clear = t.force_clear;
		g_pitch_target = reinterpret_cast<void *>(t.process_pitch->address);

		if (MH_CreateHook(g_pitch_target, reinterpret_cast<void *>(&camera_pitch_hook), reinterpret_cast<void **>(&g_original_pitch)) != MH_OK ||
			MH_EnableHook(g_pitch_target) != MH_OK)
		{
			g_status = status::failed_hook;
			goto done;
		}
		g_hook_installed = true;
		g_status = status::active;
	}

done:
	// Drop the reference taken in DllMain so the add-on can't be unloaded while this thread is still running.
	FreeLibraryAndExitThread(g_module, 0);
}

// ReShade callbacks
bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool game_window_focused()
{
	const HWND window = GetForegroundWindow();
	if (!window || IsIconic(window)) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	return pid == GetCurrentProcessId();
}

void save_enabled(bool enabled)
{
	reshade::set_config_value(nullptr, CONFIG_SECTION, "Enabled", enabled);
}

void on_present(reshade::api::effect_runtime *)
{
	const bool combo_down = key_down(VK_MENU) && key_down(VK_F12);
	if (combo_down && !g_combo_was_down && game_window_focused())
	{
		const bool enabled = !g_enabled.load(std::memory_order_acquire);
		g_enabled.store(enabled, std::memory_order_release);
		save_enabled(enabled);
	}
	g_combo_was_down = combo_down;
}

void draw_settings(reshade::api::effect_runtime *)
{
	bool enabled = g_enabled.load(std::memory_order_acquire);
	if (ImGui::Checkbox("Remove transparency dither (Alt+F12)", &enabled))
	{
		g_enabled.store(enabled, std::memory_order_release);
		save_enabled(enabled);
	}
	ImGui::TextDisabled("Status: %s", status_text(g_status.load()));
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		g_module = module;
		if (!reshade::register_addon(module))
			return FALSE;

		bool enabled = true;
		if (reshade::get_config_value(nullptr, CONFIG_SECTION, "Enabled", enabled))
			g_enabled = enabled;

		reshade::register_event<reshade::addon_event::reshade_present>(&on_present);
		reshade::register_overlay(nullptr, &draw_settings);

		if (const MH_STATUS mh = MH_Initialize(); mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
		{
			g_status = status::failed_hook;
			break;
		}

		// Hold a reference for the worker thread's lifetime (released via FreeLibraryAndExitThread).
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &self);
		if (HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
			CloseHandle(thread);
		else if (self)
			FreeLibrary(self);
		break;
	}
	case DLL_PROCESS_DETACH:
		g_stop = true;
		if (reserved == nullptr) // dynamic unload (not process exit): take the hook back out of the game
		{
			if (g_hook_installed)
				MH_DisableHook(MH_ALL_HOOKS);
			MH_Uninitialize();
		}
		reshade::unregister_overlay(nullptr, &draw_settings);
		reshade::unregister_event<reshade::addon_event::reshade_present>(&on_present);
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
