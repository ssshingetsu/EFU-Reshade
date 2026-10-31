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
constexpr DWORD SETTLE_MS = 2000; // let the runtime finish starting up after il2cpp_init, same as upstream

// IL2CPP surface (C ABI exports of GameAssembly.dll)
struct Il2CppMethodInfo { uintptr_t address; /* methodPointer is the first field */ };
using il2cpp_get_corlib_t = void *(*)();
using il2cpp_domain_get_t = void *(*)();
using il2cpp_thread_attach_t = void *(*)(void *domain);
using il2cpp_thread_detach_t = void (*)(void *thread);
using il2cpp_domain_assembly_open_t = void *(*)(void *domain, const char *name);
using il2cpp_assembly_get_image_t = void *(*)(void *assembly);
using il2cpp_class_from_name_t = void *(*)(void *image, const char *ns, const char *name);
using il2cpp_class_get_method_from_name_t = Il2CppMethodInfo *(*)(void *klass, const char *name, int argc);
using instance_void_method_t = void (*)(void *instance, const Il2CppMethodInfo *method);

enum class status : int { waiting_for_game, resolving, active, failed_resolve, failed_hook, timed_out };

std::atomic<bool> g_enabled{true};
std::atomic<status> g_status{status::waiting_for_game};
std::atomic<bool> g_worker_started{false};

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
struct il2cpp_api
{
	il2cpp_get_corlib_t get_corlib;
	il2cpp_domain_get_t domain_get;
	il2cpp_thread_attach_t thread_attach;
	il2cpp_thread_detach_t thread_detach;
	il2cpp_domain_assembly_open_t assembly_open;
	il2cpp_assembly_get_image_t assembly_get_image;
	il2cpp_class_from_name_t class_from_name;
	il2cpp_class_get_method_from_name_t method_from_name;
};

template <typename T>
bool get_export(HMODULE module, const char *name, T &out)
{
	out = reinterpret_cast<T>(GetProcAddress(module, name));
	return out != nullptr;
}

bool load_api(HMODULE game_assembly, il2cpp_api &api)
{
	// il2cpp_get_corlib / thread_attach / thread_detach are optional; the rest are required
	get_export(game_assembly, "il2cpp_get_corlib", api.get_corlib);
	get_export(game_assembly, "il2cpp_thread_attach", api.thread_attach);
	get_export(game_assembly, "il2cpp_thread_detach", api.thread_detach);
	return get_export(game_assembly, "il2cpp_domain_get", api.domain_get) &&
		get_export(game_assembly, "il2cpp_domain_assembly_open", api.assembly_open) &&
		get_export(game_assembly, "il2cpp_assembly_get_image", api.assembly_get_image) &&
		get_export(game_assembly, "il2cpp_class_from_name", api.class_from_name) &&
		get_export(game_assembly, "il2cpp_class_get_method_from_name", api.method_from_name);
}

struct targets { Il2CppMethodInfo *process_pitch; Il2CppMethodInfo *force_clear; };

bool try_resolve(const il2cpp_api &api, void *domain, targets &out)
{
	void *const assembly = api.assembly_open(domain, "Gameplay.Beyond.dll");
	if (!assembly) return false;
	void *const image = api.assembly_get_image(assembly);
	if (!image) return false;
	void *const camera_class = api.class_from_name(image, "Beyond.Gameplay.View", "CameraMono");
	if (!camera_class) return false;

	out.process_pitch = api.method_from_name(camera_class, "_ProcessDitherByPitch", 0);
	out.force_clear = api.method_from_name(camera_class, "ForceClearDither", 0);
	return out.process_pitch && out.force_clear && out.process_pitch->address && out.force_clear->address;
}

// Worker: ReShade loads add-ons at graphics init, which can be before IL2CPP is ready, so wait for it here.
// Calling into IL2CPP before il2cpp_init has finished (or from a thread the GC doesn't know about) crashes the game,
// so: wait for GameAssembly.dll, then for the runtime to report corlib loaded, then attach this thread before resolving.
DWORD WINAPI worker(LPVOID)
{
	const DWORD start = GetTickCount();
	const auto timed_out = [start] { return GetTickCount() - start > RESOLVE_TIMEOUT_MS; };

	HMODULE game_assembly = nullptr;
	while (!(game_assembly = GetModuleHandleW(L"GameAssembly.dll")))
	{
		if (timed_out()) { g_status = status::timed_out; return 0; }
		Sleep(POLL_MS);
	}

	g_status = status::resolving;
	il2cpp_api api{};
	if (!load_api(game_assembly, api)) { g_status = status::failed_resolve; return 0; }

	if (api.get_corlib)
	{
		while (!api.get_corlib()) // set by il2cpp_init once the metadata is loaded
		{
			if (timed_out()) { g_status = status::timed_out; return 0; }
			Sleep(POLL_MS);
		}
	}
	Sleep(SETTLE_MS);

	void *const domain = api.domain_get();
	if (!domain) { g_status = status::failed_resolve; return 0; }
	void *const thread = api.thread_attach ? api.thread_attach(domain) : nullptr;

	targets t{};
	bool resolved = false;
	while (!(resolved = try_resolve(api, domain, t)) && !timed_out())
		Sleep(POLL_MS);

	if (thread && api.thread_detach)
		api.thread_detach(thread);

	if (!resolved) { g_status = status::failed_resolve; return 0; }

	g_force_clear = t.force_clear;
	g_pitch_target = reinterpret_cast<void *>(t.process_pitch->address);

	if (MH_CreateHook(g_pitch_target, reinterpret_cast<void *>(&camera_pitch_hook), reinterpret_cast<void **>(&g_original_pitch)) != MH_OK ||
		MH_EnableHook(g_pitch_target) != MH_OK)
	{
		g_status = status::failed_hook;
		return 0;
	}
	g_status = status::active;
	return 0;
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

// ReShade calls these on every add-on load/unload, outside the loader lock.
// ReShade may unload and reload add-ons while the game starts (e.g. when the graphics device is recreated), but the hook
// lives in the game's code for the rest of the session, so the module is pinned and the hook is installed only once.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
	if (!reshade::register_addon(addon_module, reshade_module))
	{
		reshade::unregister_addon(addon_module, reshade_module); // undo a partial registration (e.g. ImGui version mismatch)
		return false;
	}

	bool enabled = true;
	if (reshade::get_config_value(nullptr, CONFIG_SECTION, "Enabled", enabled))
		g_enabled = enabled;

	reshade::register_event<reshade::addon_event::reshade_present>(&on_present);
	reshade::register_overlay(nullptr, &draw_settings);

	if (!g_worker_started.exchange(true))
	{
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&worker), &pinned);

		if (MH_Initialize() != MH_OK)
			g_status = status::failed_hook;
		else if (HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
			CloseHandle(thread);
		else
			g_status = status::failed_hook;
	}
	return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
	reshade::unregister_overlay(nullptr, &draw_settings);
	reshade::unregister_event<reshade::addon_event::reshade_present>(&on_present);
	reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(module);
	return TRUE;
}
