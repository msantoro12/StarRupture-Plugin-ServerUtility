// SUProbe: a throwaway, logging-only probe for the dedicated server. It hooks the placement,
// unlock and gather functions the ServerUtility cheat features depend on, calls every original
// unchanged and writes what it saw to its own log. It writes nothing to the game. Never ships.
//
// Log: <ModLoader>\Logs\SUProbe-probe.log (beside ModLoader.log). Pointers, class names, numbers
// only: no player names, platform IDs or addresses of peers.

#include "plugin_interface.h"
#include "plugin_helpers.h"
#include "server_utility_fields.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

static IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.0-probe"
#endif

PLUGIN_EXPORT_HOST_INFO()

static PluginInfo s_pluginInfo = {
	"SUProbe",
	MODLOADER_BUILD_TAG,
	"probe",
	"Logging-only probe of the server's build and gather functions. Changes nothing.",
	PLUGIN_DECLARED_INTERFACE,
	PLUGIN_TARGET_SERVER,
	PLUGIN_GAME_BUILT_CL,
	PLUGIN_MAP_MIGRATED
};

namespace
{
	// ---- Own log: buffered in memory, flushed from the tick and on shutdown. ----

	constexpr uint32_t kFullCalls    = 20;   // the first calls of each function are logged in full
	constexpr double   kSummarySecs  = 5.0;  // then a count this often
	constexpr double   kFlushSecs    = 2.0;
	constexpr size_t   kMaxBuffer    = 1u << 20; // drop the oldest rather than grow without bound

	std::mutex  g_logMutex;
	std::string g_buffer;
	std::wstring g_logPath;
	std::atomic<bool> g_started{ false };
	ULONGLONG g_t0 = 0;

	std::wstring LogPath()
	{
		wchar_t module[MAX_PATH] = {};
		HMODULE self = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        reinterpret_cast<LPCWSTR>(&LogPath), &self)
		    || GetModuleFileNameW(self, module, MAX_PATH) == 0)
			return {};
		// ...\ModLoader\Plugins\SUProbe.dll -> ...\ModLoader\Logs\SUProbe-probe.log
		std::filesystem::path p(module);
		return (p.parent_path().parent_path() / L"Logs" / L"SUProbe-probe.log").wstring();
	}

	void Append(const char* line)
	{
		char stamp[40];
		sprintf_s(stamp, "[%8.3f] ", static_cast<double>(GetTickCount64() - g_t0) / 1000.0);
		std::lock_guard<std::mutex> lock(g_logMutex);
		if (g_buffer.size() > kMaxBuffer)
			g_buffer.erase(0, g_buffer.size() / 2);
		g_buffer += stamp;
		g_buffer += line;
		g_buffer += '\n';
	}

	void AppendF(const char* fmt, ...)
	{
		char text[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		Append(text);
	}

	void Flush()
	{
		std::string out;
		{
			std::lock_guard<std::mutex> lock(g_logMutex);
			out.swap(g_buffer);
		}
		if (out.empty() || g_logPath.empty())
			return;
		FILE* f = nullptr;
		if (_wfopen_s(&f, g_logPath.c_str(), L"ab") == 0 && f)
		{
			fwrite(out.data(), 1, out.size(), f);
			fclose(f);
		}
	}

	// ---- Safe reads (SEH frames hold no objects needing unwinding). ----

	void ClassNameImpl(void* obj, char* out, size_t n)
	{
		SDK::UClass** cls = ServerUtilityFields::ObjectClass.In(obj);
		if (!cls || !*cls) { strcpy_s(out, n, "-"); return; }
		const std::string name = (*cls)->GetName();
		strncpy_s(out, n, name.c_str(), _TRUNCATE);
	}

	void ClassName(void* obj, char* out, size_t n)
	{
		strcpy_s(out, n, "-");
		if (!obj) return;
		__try { ClassNameImpl(obj, out, n); }
		__except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "<fault>"); }
	}

	bool IsActor_SEH(void* obj)
	{
		__try
		{
			return obj && static_cast<SDK::UObject*>(obj)->IsA(SDK::AActor::StaticClass());
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void Hex(const void* p, size_t n, char* out, size_t outSize)
	{
		out[0] = 0;
		if (!p || outSize < n * 2 + 1) return;
		__try
		{
			const uint8_t* b = static_cast<const uint8_t*>(p);
			for (size_t i = 0; i < n; ++i)
				sprintf_s(out + i * 2, outSize - i * 2, "%02X", b[i]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, outSize, "<fault>"); }
	}

	// Same chain as the SU build hooks: a placement component's outer is its actor, an actor's owner
	// is the next link, a pawn's controller is the player's. Pointer only, used in this frame.
	void* ControllerOf_SEH(void* object)
	{
		__try
		{
			SDK::UObject* cur = static_cast<SDK::UObject*>(object);
			for (int hop = 0; cur && hop < 8; ++hop)
			{
				if (cur->IsA(SDK::APlayerController::StaticClass()))
					return cur;
				if (cur->IsA(SDK::APawn::StaticClass()))
				{
					SDK::AController** at = ServerUtilityFields::PawnController.In(cur);
					SDK::AController* controller = at ? *at : nullptr;
					return controller && controller->IsA(SDK::APlayerController::StaticClass()) ? controller : nullptr;
				}
				SDK::UObject* next = nullptr;
				if (cur->IsA(SDK::AActor::StaticClass()))
				{
					SDK::AActor** owner = ServerUtilityFields::ActorOwner.In(cur);
					next = owner ? *owner : nullptr;
				}
				if (!next)
				{
					SDK::UObject** outer = ServerUtilityFields::ObjectOuter.In(cur);
					next = outer ? *outer : nullptr;
				}
				cur = next;
			}
			return nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	bool OnGameThread()
	{
		IPluginHooks* hooks = GetHooks();
		return hooks && hooks->Engine && hooks->Engine->IsGameThread();
	}

	// ---- The hook table. ----

	enum Fn : int
	{
		F_Cost, F_CustomStab, F_PillarStab, F_MainMesh, F_Box, F_Snapped, F_SocketFree, F_CheckAvail, F_Deplete, F_Count
	};

	struct HookRow
	{
		const char* name;
		const char* key;
		void*       detour;
		void**      original;
		uintptr_t   address = 0;
		HookHandle  handle = nullptr;
		PluginMap::Feature feature;
		std::atomic<uint32_t> total{ 0 };
		std::atomic<uint32_t> sinceSummary{ 0 };
		HookRow(const char* n, const char* k, void* d, void** o)
			: name(n), key(k), detour(d), original(o), feature{ n, n, PluginMap::KeysOf(k) } {}
	};

	std::vector<HookRow*> g_rows;
	std::atomic<bool> g_shutdown{ false };

	// True when this call is logged in full; otherwise only counted for the summary.
	bool Full(int fn)
	{
		HookRow& r = *g_rows[fn];
		const uint32_t n = r.total.fetch_add(1) + 1;
		if (n <= kFullCalls) return true;
		r.sinceSummary.fetch_add(1);
		return false;
	}

	struct Ctx
	{
		char cls[96];
		char thread[8];
		void* controller;
	};

	// `this` class name, thread and the owning controller (game thread only, like the SU hooks).
	void Describe(void* self, Ctx& c)
	{
		const bool game = OnGameThread();
		strcpy_s(c.thread, game ? "game" : "other");
		ClassName(self, c.cls, sizeof(c.cls));
		c.controller = game ? ControllerOf_SEH(self) : nullptr;
	}

	void LogCall(int fn, void* self, const char* extra)
	{
		Ctx c;
		Describe(self, c);
		AppendF("%s thread=%s this=%p class=%s owner=%p %s", g_rows[fn]->name, c.thread, self, c.cls, c.controller, extra);
	}

	// ---- Original pointers. ----

	using CostFn       = int64_t(__fastcall*)(void* self);
	using StabilityFn  = bool(__fastcall*)(void* self, const void* placementData);
	using MainMeshFn   = uint8_t(__fastcall*)(void* self, const void* placementData);
	using BoxFn        = uint8_t(__fastcall*)(void* self, const void* min, const void* max, const void* ignoredActors, const void* ignoredClasses, bool flag);
	using SnappedFn    = void*(__fastcall*)(void* self, const void* data, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9, void* a10);
	using SocketFreeFn = bool(__fastcall*)(void* self, void* building, const void* socket);
	using CheckFn      = void(__fastcall*)(void* keeper, void* corporation, int64_t reputation);
	using DepleteFn    = void*(__fastcall*)(void* self, void* ret, const void* location, void* player);

	CostFn       o_Cost = nullptr;
	StabilityFn  o_Custom = nullptr, o_Pillar = nullptr;
	MainMeshFn   o_MainMesh = nullptr;
	BoxFn        o_Box = nullptr;
	SnappedFn    o_Snapped = nullptr;
	SocketFreeFn o_SocketFree = nullptr;
	CheckFn      o_Check = nullptr;
	DepleteFn    o_Deplete = nullptr;

	// ---- Detours: original first, arguments and result untouched, then one log line. ----

	int64_t __fastcall D_Cost(void* self)
	{
		const int64_t r = o_Cost(self);
		if (!g_shutdown && Full(F_Cost)) { char e[64]; sprintf_s(e, "result=%lld", static_cast<long long>(r)); LogCall(F_Cost, self, e); }
		return r;
	}

	bool __fastcall D_Custom(void* self, const void* data)
	{
		const bool r = o_Custom(self, data);
		if (!g_shutdown && Full(F_CustomStab)) { char e[64]; sprintf_s(e, "data=%p result=%d", data, r ? 1 : 0); LogCall(F_CustomStab, self, e); }
		return r;
	}

	bool __fastcall D_Pillar(void* self, const void* data)
	{
		const bool r = o_Pillar(self, data);
		if (!g_shutdown && Full(F_PillarStab)) { char e[64]; sprintf_s(e, "data=%p result=%d", data, r ? 1 : 0); LogCall(F_PillarStab, self, e); }
		return r;
	}

	uint8_t __fastcall D_MainMesh(void* self, const void* data)
	{
		const uint8_t r = o_MainMesh(self, data);
		if (!g_shutdown && Full(F_MainMesh)) { char e[64]; sprintf_s(e, "data=%p result=%u", data, r); LogCall(F_MainMesh, self, e); }
		return r;
	}

	uint8_t __fastcall D_Box(void* self, const void* mn, const void* mx, const void* ia, const void* ic, bool flag)
	{
		const uint8_t r = o_Box(self, mn, mx, ia, ic, flag);
		if (!g_shutdown && Full(F_Box)) { char e[96]; sprintf_s(e, "flag=%d result=%u", flag ? 1 : 0, r); LogCall(F_Box, self, e); }
		return r;
	}

	void* __fastcall D_Snapped(void* self, const void* data, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9, void* a10)
	{
		void* r = o_Snapped(self, data, a3, a4, a5, a6, a7, a8, a9, a10);
		if (!g_shutdown && Full(F_Snapped)) { char e[96]; sprintf_s(e, "data=%p result=%p", data, r); LogCall(F_Snapped, self, e); }
		return r;
	}

	bool __fastcall D_SocketFree(void* self, void* building, const void* socket)
	{
		const bool r = o_SocketFree(self, building, socket);
		if (!g_shutdown && Full(F_SocketFree))
		{
			char bc[96]; ClassName(building, bc, sizeof(bc));
			char e[192]; sprintf_s(e, "building=%p buildingClass=%s socket=%p result=%d", building, bc, socket, r ? 1 : 0);
			LogCall(F_SocketFree, self, e);
		}
		return r;
	}

	void __fastcall D_Check(void* keeper, void* corporation, int64_t reputation)
	{
		o_Check(keeper, corporation, reputation);
		if (!g_shutdown && Full(F_CheckAvail))
		{
			const bool* flag = ServerUtilityFields::UnlockAllBuildings.In(keeper);
			char e[128]; sprintf_s(e, "corp=%p rep=%lld unlockFlagAfter=%d", corporation, static_cast<long long>(reputation), flag ? (*flag ? 1 : 0) : -1);
			LogCall(F_CheckAvail, keeper, e);
		}
	}

	constexpr size_t kMinedWindow = 0x80;

	// ACrGatherableBaseActor::DepleteResource: this RCX, hidden return pointer RDX (FCrMinedResource),
	// FVector* R8, player R9. The return bytes are dumped before and after the original runs.
	void* __fastcall D_Deplete(void* self, void* ret, const void* location, void* player)
	{
		const bool full = !g_shutdown && Full(F_Deplete);
		char before[kMinedWindow * 2 + 1] = {};
		if (full) Hex(ret, kMinedWindow, before, sizeof(before));
		void* r = o_Deplete(self, ret, location, player);
		if (full)
		{
			char after[kMinedWindow * 2 + 1];
			Hex(ret, kMinedWindow, after, sizeof(after));
			void* thisMinus = static_cast<uint8_t*>(self) - 0x2B0;
			char pc[96]; ClassName(player, pc, sizeof(pc));
			char tm[96]; ClassName(thisMinus, tm, sizeof(tm));
			char e[256];
			sprintf_s(e, "ret=%p loc=%p player=%p playerClass=%s resultPtr=%p thisIsActor=%d this-0x2B0=%p this-0x2B0Class=%s this-0x2B0IsActor=%d",
				ret, location, player, pc, r, IsActor_SEH(self) ? 1 : 0, thisMinus, tm, IsActor_SEH(thisMinus) ? 1 : 0);
			LogCall(F_Deplete, self, e);
			AppendF("  DepleteResource ret before(%zu bytes)=%s", kMinedWindow, before);
			AppendF("  DepleteResource ret after (%zu bytes)=%s", kMinedWindow, after);
		}
		return r;
	}

	// ---- bUnlockAllBuildings watch, read on the game thread from the tick. ----

	bool ReadUnlock_SEH(void** keeperOut, int* valueOut)
	{
		__try
		{
			auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
			if (!world || !world->GameState || !world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
				return false;
			SDK::ACrTechnologyKeeper** at = ServerUtilityFields::TechnologyKeeper.In(world->GameState);
			SDK::ACrTechnologyKeeper* keeper = at ? *at : nullptr;
			*keeperOut = keeper;
			if (!keeper) return true;
			const bool* flag = ServerUtilityFields::UnlockAllBuildings.In(keeper);
			*valueOut = flag ? (*flag ? 1 : 0) : -1;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void*  g_lastKeeper = nullptr;
	int    g_lastValue = -2;
	double g_sinceFlush = 0.0, g_sinceSummary = 0.0;

	void OnTick(float dt)
	{
		if (g_shutdown) return;
		void* keeper = nullptr;
		int value = -1;
		if (ReadUnlock_SEH(&keeper, &value))
		{
			if (keeper != g_lastKeeper || value != g_lastValue)
			{
				AppendF("unlockWatch keeper=%p bUnlockAllBuildings=%d (was keeper=%p value=%d)", keeper, value, g_lastKeeper, g_lastValue);
				g_lastKeeper = keeper;
				g_lastValue = value;
			}
		}

		g_sinceSummary += dt;
		if (g_sinceSummary >= kSummarySecs)
		{
			g_sinceSummary = 0.0;
			for (HookRow* r : g_rows)
			{
				const uint32_t n = r->sinceSummary.exchange(0);
				if (n) AppendF("%s: %u more calls in the last %.0fs (total %u)", r->name, n, kSummarySecs, r->total.load());
			}
		}
		g_sinceFlush += dt;
		if (g_sinceFlush >= kFlushSecs)
		{
			g_sinceFlush = 0.0;
			Flush();
		}
	}

	// Takes every hook out and writes the rest of the log. Safe on any thread and twice.
	void Stop()
	{
		if (g_shutdown.exchange(true)) return;
		IPluginHooks* hooks = GetHooks();
		if (hooks && hooks->Engine && hooks->Engine->UnregisterOnTick)
			hooks->Engine->UnregisterOnTick(OnTick);
		for (HookRow* r : g_rows)
		{
			if (r->handle && hooks && hooks->Hooks && hooks->Hooks->Remove)
				hooks->Hooks->Remove(r->handle);
			r->handle = nullptr;
		}
		for (HookRow* r : g_rows)
		{
			const uint32_t n = r->sinceSummary.exchange(0);
			AppendF("end %s: total calls %u (%u not logged in full)", r->name, r->total.load(), n);
		}
		Append("probe stopped");
		Flush();
	}

	void OnEngineShutdown() { Stop(); }

	void BuildRows()
	{
		if (!g_rows.empty()) return;
		g_rows.push_back(new HookRow("UCrBuildingComponent::GetPlacementResourceConditionResult", ServerUtilityFields::PlacementResourceCondition, &D_Cost, reinterpret_cast<void**>(&o_Cost)));
		g_rows.push_back(new HookRow("ACrAPHelperActorCustom::CheckStability", ServerUtilityFields::CheckStabilityCustom, &D_Custom, reinterpret_cast<void**>(&o_Custom)));
		g_rows.push_back(new HookRow("ACrAPHelperDynamicPillar::CheckStability", ServerUtilityFields::CheckStabilityPillar, &D_Pillar, reinterpret_cast<void**>(&o_Pillar)));
		g_rows.push_back(new HookRow("ACrAPHelperActorCustom::CheckMainMeshCollision", ServerUtilityFields::CheckMainMeshCollision, &D_MainMesh, reinterpret_cast<void**>(&o_MainMesh)));
		g_rows.push_back(new HookRow("ACrAPHelper::GetBoxCollisionResult", ServerUtilityFields::GetBoxCollisionResult, &D_Box, reinterpret_cast<void**>(&o_Box)));
		g_rows.push_back(new HookRow("UCrSocketSnapPlacementMethod::GetSnappedSocket", ServerUtilityFields::GetSnappedSocket, &D_Snapped, reinterpret_cast<void**>(&o_Snapped)));
		g_rows.push_back(new HookRow("UCrBuildingStabilitySubsystem::IsSocketFree", ServerUtilityFields::IsSocketFree, &D_SocketFree, reinterpret_cast<void**>(&o_SocketFree)));
		g_rows.push_back(new HookRow("ACrTechnologyKeeper::CheckAvailableBuildings", ServerUtilityFields::CheckAvailableBuildings, &D_Check, reinterpret_cast<void**>(&o_Check)));
		g_rows.push_back(new HookRow("ACrGatherableBaseActor::DepleteResource", "ACrGatherableBaseActor::DepleteResource", &D_Deplete, reinterpret_cast<void**>(&o_Deplete)));
	}

	// The members the probe reads, as one feature: a miss turns off the owner lookup and the unlock
	// watch, not the plugin.
	PluginMap::Feature g_fields{ "probe-fields", "Probe member reads", PluginMap::KeysOf(
		ServerUtilityFields::ObjectClass, ServerUtilityFields::ObjectOuter, ServerUtilityFields::ActorOwner,
		ServerUtilityFields::PawnController, ServerUtilityFields::TechnologyKeeper, ServerUtilityFields::UnlockAllBuildings) };
}

extern "C" {
__declspec(dllexport) PluginInfo* GetPluginInfo() { return &s_pluginInfo; }

__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
{
	g_self = self;
	BuildRows();
	for (HookRow* r : g_rows)
		PluginMap::DeclareFeature(scanner, self, r->feature);
	PluginMap::DeclareFeature(scanner, self, g_fields);

	PluginMap::ResolveFields(scanner, self,
		ServerUtilityFields::ObjectClass, ServerUtilityFields::ObjectOuter, ServerUtilityFields::ActorOwner,
		ServerUtilityFields::PawnController, ServerUtilityFields::TechnologyKeeper, ServerUtilityFields::UnlockAllBuildings);

	for (HookRow* r : g_rows)
		r->address = PluginMap::Function(scanner, self, r->key, nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	PluginSdkRuntime::Init(self->hooks);
	g_self = self;
	g_t0 = GetTickCount64();
	g_logPath = LogPath();
	BuildRows();
	Append("SUProbe start (logging only; nothing is changed)");

	IPluginHooks* hooks = self->hooks;
	if (!hooks || !hooks->Hooks || !hooks->Hooks->Install || !hooks->Engine)
	{
		Append("hook or engine interface missing: nothing installed");
		Flush();
		return false;
	}

	for (HookRow* r : g_rows)
	{
		if (!r->address)
		{
			AppendF("row unresolved at load: %s", r->name);
			continue;
		}
		r->handle = hooks->Hooks->Install(r->address, r->detour, r->original);
		AppendF("%s: address 0x%llX %s", r->name, static_cast<unsigned long long>(r->address), r->handle ? "hooked" : "INSTALL FAILED");
	}

	int readable = 0;
	void* keeper = nullptr; int value = -1;
	if (ServerUtilityFields::TechnologyKeeper.offset >= 0 && ServerUtilityFields::UnlockAllBuildings.offset >= 0) readable = 1;
	AppendF("unlock watch member rows %s (keeper offset %d, flag offset %d)", readable ? "resolved" : "missing",
		ServerUtilityFields::TechnologyKeeper.offset, ServerUtilityFields::UnlockAllBuildings.offset);
	(void)keeper; (void)value;

	hooks->Engine->RegisterOnTick(OnTick);
	hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
	Flush();
	LOG_INFO("SUProbe loaded; log at Logs\\SUProbe-probe.log");
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	Stop();
	g_self = nullptr;
}
}
