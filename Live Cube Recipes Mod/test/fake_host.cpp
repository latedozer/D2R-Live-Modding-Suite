// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Loads the plugin the way D2RLoader does and plays the game's part:
//  - a "game image" whose header carries the test stamp and lists the four game functions;
//  - data tables with a recipe vector at +0x1570 per data version, filled from the game's own
//    cubemain.bin (numbers, classes, property codes: Blizzard's bytes moved to the wide record)
//    plus the stand-in D2RCore's input/output cells, and three records "of another plugin";
//  - the loader's services: log, settings, console, threads (jobs run when the test lets them),
//    the tables-read notice and the table service.
// Then it writes recipes into the plugin's own recipes.txt as a user would, types the console
// command and checks the list
// (and that the mod's folder is never touched).
// usage: fake_host.exe <plugin.dll> <work folder> <folder with the game's data/global/excel>
#include <D2RLPlugin/api.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what) {
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
	fflush(stdout);
	g_failures += ok ? 0 : 1;
}

constexpr size_t kWide = 0x16c, kStock = 0x158;
constexpr uint64_t kNotOwned = 0x8000000000000000ull;

struct Vector {
	uint8_t* rows;
	uint64_t count;
	uint64_t capacity;
};

std::string ReadText(const std::string& path) {
	std::ifstream     in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}
void WriteText(const std::string& path, const std::string& text) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}
std::vector<std::string> Split(const std::string& text, char by) {
	std::vector<std::string> out;
	size_t                   start = 0;
	for (;;) {
		const size_t end = text.find(by, start);
		out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (end == std::string::npos) {
			return out;
		}
		start = end + 1;
	}
}
std::vector<std::string> Lines(const std::string& text) {
	std::vector<std::string> out = Split(text, '\n');
	for (std::string& line : out) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
	}
	if (!out.empty() && out.back().empty()) {
		out.pop_back();
	}
	return out;
}
std::string Join(const std::vector<std::string>& lines) {
	std::string out;
	for (const std::string& line : lines) {
		out += line + "\r\n";
	}
	return out;
}

// ---- the game's side

using CellFn = int (*)(uint8_t, char*, void*, int, uint64_t, int);
CellFn g_input = nullptr, g_output = nullptr;

std::vector<std::string> g_properties[2]; // [0] RotW, [1] base
const char*              kClasses[] = { "ama", "sor", "nec", "pal", "bar", "dru", "ass", "war" };

struct Tables {
	uint8_t bytes[0x1600];
	Vector* vector() { return reinterpret_cast<Vector*>(bytes + 0x1570); }
};
Tables g_rotw, g_base;
// The "class name table": shaped like the game's (bucket count at +0x18, buckets at +0x20).
struct NameTable {
	uint64_t unused[3] = {};
	uint64_t buckets   = 8;
	void*    first     = &unused;
} g_classNames;

void* GameTables(uint8_t version) {
	return version == 3 ? &g_rotw : version == 1 || version == 2 ? &g_base : nullptr;
}
int GameLookup(void* table, const char* text, int) {
	if (table != &g_classNames) {
		return -1;
	}
	for (int i = 0; i < 8; ++i) {
		if (strcmp(kClasses[i], text) == 0) {
			return i;
		}
	}
	return -1;
}
void GameMod(uint8_t version, const char* text, void* record, int offset) {
	const auto& list = g_properties[version == 3 ? 0 : 1];
	int16_t     id   = -1;
	for (size_t i = 0; i < list.size(); ++i) {
		if (list[i] == text) {
			id = static_cast<int16_t>(i);
		}
	}
	uint8_t* at = static_cast<uint8_t*>(record) + offset;
	memset(at, 0xff, 4);
	if (id >= 0) {
		memcpy(at, &id, 2);
		at[2] = at[3] = 0;
	}
}
void GameParam(uint8_t, const char*, uint32_t* value) {
	*value = 0;
}

int Column(const std::vector<std::string>& header, const char* name) {
	for (size_t i = 0; i < header.size(); ++i) {
		if (_stricmp(header[i].c_str(), name) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// One line as the game holds it: Blizzard's compiled bytes moved to the wide record, the input
// and output cells by the stand-in D2RCore.
std::vector<uint8_t> GameRecord(uint8_t version, const std::vector<std::string>& header, const std::string& line, const uint8_t* stock, int lineNumber) {
	std::vector<uint8_t> row(kWide, 0);
	memcpy(row.data(), stock, 0x14);
	const std::vector<std::string> cells = Split(line, '\t');
	const auto cell = [&](const char* name) {
		const int at = Column(header, name);
		return at >= 0 && static_cast<size_t>(at) < cells.size() ? cells[static_cast<size_t>(at)] : std::string();
	};
	for (int slot = 0; slot < 7; ++slot) {
		std::string text = cell(("input " + std::to_string(slot + 1)).c_str());
		if (!text.empty()) {
			text.push_back('\0');
			g_input(version, text.data(), row.data(), slot, 0, lineNumber);
		}
	}
	const char* outputs[] = { "output", "output b", "output c" };
	for (int slot = 0; slot < 3; ++slot) {
		std::string text = cell(outputs[slot]);
		if (!text.empty()) {
			text.push_back('\0');
			g_output(version, text.data(), row.data(), slot, 0, lineNumber);
		}
		// lvl, plvl, ilvl and the five mods: Blizzard's bytes
		const uint8_t* from = stock + 0x4c + slot * 0x59;
		uint8_t*       to   = row.data() + 0x5a + slot * 0x5b;
		memcpy(to + 0xe, from + 0xe, 3);
		memcpy(to + 0x1d, from + 0x1d, 0x59 - 0x1d);
	}
	return row;
}

struct Stock {
	std::vector<std::string> lines; // the file's lines, header first
	std::string              bin;
	std::vector<std::string> header;
};
Stock g_stock[2];

// The game reads its tables: a fresh recipe list from `lines` (stock lines by their bin record;
// lines not in the stock file cannot be made here and are an error of the test).
void LoadTables(uint8_t version, Tables& tables, const Stock& stock, int others) {
	Vector* vector = tables.vector();
	if (vector->rows != nullptr && (vector->capacity & kNotOwned) == 0) {
		free(vector->rows); // the game frees a list of its own, never one that is marked not its own
	}
	const size_t count = stock.lines.size() - 1;
	uint8_t*     rows  = static_cast<uint8_t*>(calloc(count + static_cast<size_t>(others), kWide));
	for (size_t i = 0; i < count; ++i) {
		const std::vector<uint8_t> row = GameRecord(version, stock.header, stock.lines[i + 1], reinterpret_cast<const uint8_t*>(stock.bin.data()) + 4 + i * kStock, static_cast<int>(i));
		memcpy(rows + i * kWide, row.data(), kWide);
	}
	for (int k = 0; k < others; ++k) { // another plugin's recipes, behind the file's
		uint8_t* row = rows + (count + static_cast<size_t>(k)) * kWide;
		row[0]       = 1;
		row[0xc]     = 1;
		memcpy(row + 0x14, "OTHERPLUG", 9);
		row[0x14 + 9] = static_cast<uint8_t>('0' + k);
	}
	*reinterpret_cast<void**>(tables.bytes + 0x18) = &g_classNames;
	vector->rows                                    = rows;
	vector->count                                   = count + static_cast<size_t>(others);
	vector->capacity                                = vector->count;
}

bool Same(const uint8_t* a, const uint8_t* b) { // but for the stored line numbers
	for (size_t i = 0; i < kWide; ++i) {
		const bool lineNumber = i >= 0x5a && i < 0x5a + 3 * 0x5b && (i - 0x5a) % 0x5b < 4;
		if (a[i] != b[i] && !lineNumber) {
			return false;
		}
	}
	return true;
}

// ---- the loader's side

std::vector<std::string> g_log, g_console;
std::string              g_config = "enabled = true\nconsole = true\n";

void __cdecl LogLine(const D2RL::PluginContext*, const char* message) noexcept {
	g_log.push_back(message);
	printf("  [log] %s\n", message);
	fflush(stdout);
}
bool __cdecl EnsureConfig(const D2RL::PluginContext*, const char*) noexcept {
	return true;
}
bool __cdecl ReadConfig(const D2RL::PluginContext*, char* output, uint32_t size, uint32_t*) noexcept {
	snprintf(output, size, "%s", g_config.c_str());
	return true;
}
D2RL::ConsoleCommandCallback g_command = nullptr, g_bookCommand = nullptr, g_legendCommand = nullptr;
void*                        g_bookData = nullptr, *g_legendData = nullptr;
bool __cdecl RegisterCommand(const D2RL::PluginContext*, const D2RL::ConsoleCommandRegistration* command) noexcept {
	if (strcmp(command->name, "recipes") == 0) {
		g_command = command->callback;
	} else if (strcmp(command->name, "recipebook") == 0) {
		g_bookCommand = command->callback;
		g_bookData    = command->userData;
	} else if (strcmp(command->name, "recipelegend") == 0) {
		g_legendCommand = command->callback;
		g_legendData    = command->userData;
	}
	return true;
}
void __cdecl ConsoleLine(const D2RL::PluginContext*, const char* message, D2RL::ConsoleMessageKind) noexcept {
	g_console.push_back(message);
	printf("  [console] %s\n", message);
	fflush(stdout);
}

struct Queued {
	D2RL::Threads::Callback callback;
	void*                   data;
};
CRITICAL_SECTION    g_queueLock;
std::vector<Queued> g_gameJobs, g_uiJobs;
int                 g_gameMode = 0; // 0: a game runs; 1: no game (jobs wait for ever); 2: remote client (Unavailable)
int                 g_ranOnGame = 0, g_ranOnUi = 0;

D2RL::Threads::Result __cdecl RunOnGame(const D2RL::PluginContext*, D2RL::Threads::Callback callback, void* data) noexcept {
	if (g_gameMode == 2) {
		return D2RL::Threads::Result::Unavailable;
	}
	EnterCriticalSection(&g_queueLock);
	g_gameJobs.push_back({ callback, data });
	LeaveCriticalSection(&g_queueLock);
	return D2RL::Threads::Result::Success;
}
D2RL::Threads::Result __cdecl RunOnUi(const D2RL::PluginContext*, D2RL::Threads::Callback callback, void* data) noexcept {
	EnterCriticalSection(&g_queueLock);
	g_uiJobs.push_back({ callback, data });
	LeaveCriticalSection(&g_queueLock);
	return D2RL::Threads::Result::Success;
}
D2RL::ThreadService g_threadService { D2RL::ThreadServiceSize, D2RL::ThreadService::AbiVersion, RunOnUi, RunOnGame };

D2RL::Lifecycle::DataTablesLoadedCallback g_tablesListener = nullptr;
D2RL::Lifecycle::Result __cdecl RegisterTables(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedListener* listener, D2RL::Lifecycle::ListenerHandle* handle) noexcept {
	g_tablesListener = listener->callback;
	*handle          = 1;
	return D2RL::Lifecycle::Result::Success;
}
D2RL::LifecycleService g_lifecycle {};

D2RL::DataTables::Result __cdecl GetTable(const D2RL::PluginContext*, D2RL::DataTables::Bank bank, D2RL::DataTables::TableId id, D2RL::DataTables::TableView* view) noexcept {
	if (id != D2RL::DataTables::TableId::CubeMain) {
		return D2RL::DataTables::Result::Unsupported;
	}
	Vector* vector = static_cast<Tables*>(GameTables(static_cast<uint8_t>(bank)))->vector();
	view->rows     = vector->rows;
	view->rowCount = static_cast<uint32_t>(vector->count);
	view->rowSize  = kWide;
	return D2RL::DataTables::Result::Success;
}
D2RL::DataTableService g_tableService { D2RL::DataTableServiceSize, D2RL::DataTableService::AbiVersion, GetTable, nullptr, nullptr, nullptr };

// ---- the loader's panel side, as D2RLoader 1.3.1 has it: a widget service that ends after
// getInputText (no label or button calls), panels with no flag but CloseOnEscape, no key
// listener. What a panel shows is what its layout said when D2R built it; D2R keeps a built
// panel until it is unloaded. The stand-in also counts breaches of the rules MapSense keeps:
// a panel is unloaded only when closed, and not within 1.5 s of closing.

struct FakePanel {
	std::string localId, layout, loaded; // registered layout; the layout the built panel shows
	bool        built = false, open = false;
	uint64_t    closedAt = 0;
};
std::vector<FakePanel> g_panelsFake;
int                    g_breaches = 0, g_unloads = 0;

FakePanel* PanelByPath(const std::string& path) {
	for (FakePanel& panel : g_panelsFake) {
		if (path == "data/global/ui/layouts/mod-book/" + panel.localId + "hd.json") {
			return &panel;
		}
	}
	return nullptr;
}
int g_buttonPictures = 0, g_scrollPictures = 0;
D2RL::Resources::Result __cdecl RegisterResource(const D2RL::PluginContext*, const D2RL::Resources::ResourceRegistration* resource, D2RL::Resources::RegistrationHandle* handle) noexcept {
	std::string path = resource->path;
	if (path.size() > 7 && path.substr(path.size() - 7) == ".sprite") { // the hover picture and the buttons' pictures
		bool right = resource->byteCount == 44 && memcmp(resource->bytes, "SpA1", 4) == 0 && path.find("data/hd/global/ui/d2rloader/mod-book/") == 0;
		if (path.find("/scroll") != std::string::npos && resource->byteCount > 40 && memcmp(resource->bytes, "SpA1", 4) == 0) {
			right = true; // the scroll line and its mark
			++g_scrollPictures;
		}
		if (path.find("/button") != std::string::npos && resource->byteCount > 40 && memcmp(resource->bytes, "SpA1", 4) == 0) {
			// four frames of the width in the name (half of it and 22 high for the low-resolution one)
			const bool     low   = path.find(".lowend.") != std::string::npos;
			const unsigned width = static_cast<unsigned>(atoi(path.c_str() + path.find("/button") + 7)) / (low ? 2 : 1), height = low ? 22 : 44;
			const uint8_t* b     = static_cast<const uint8_t*>(resource->bytes);
			right                = (b[6] | b[7] << 8) == static_cast<int>(width) && (b[8] | b[9] << 8) == static_cast<int>(width * 4) && b[12] == height && b[20] == 4 && resource->byteCount == 40 + width * 4 * height * 4;
			g_buttonPictures += right ? 1 : 0;
		}
		g_breaches += right ? 0 : 1;
		*handle = 1000;
		return right ? D2RL::Resources::Result::Success : D2RL::Resources::Result::InvalidArgument;
	}
	FakePanel*  panel = PanelByPath(path);
	if (panel == nullptr) { // layouts are registered before their panels
		const size_t start = path.rfind('/') + 1;
		g_panelsFake.push_back({ path.substr(start, path.size() - start - 7), "", "", false, false, 0 });
		panel = &g_panelsFake.back();
	}
	if (!panel->layout.empty()) {
		++g_breaches; // one resource a path
		return D2RL::Resources::Result::Conflict;
	}
	panel->layout.assign(static_cast<const char*>(resource->bytes), resource->byteCount);
	*handle = static_cast<D2RL::Resources::RegistrationHandle>(panel - g_panelsFake.data()) + 1;
	return D2RL::Resources::Result::Success;
}
D2RL::Resources::Result __cdecl UnregisterResource(const D2RL::PluginContext*, D2RL::Resources::RegistrationHandle handle) noexcept {
	if (handle == 0 || handle > g_panelsFake.size() || g_panelsFake[handle - 1].layout.empty()) {
		++g_breaches;
		return D2RL::Resources::Result::StaleHandle;
	}
	g_panelsFake[handle - 1].layout.clear();
	return D2RL::Resources::Result::Success;
}
D2RL::ResourceService g_resourceService { D2RL::ResourceServiceSize, D2RL::ResourceService::AbiVersion, RegisterResource, UnregisterResource, nullptr };

D2RL::Panels::Result __cdecl RegisterPanel(const D2RL::PluginContext*, const D2RL::Panels::PanelRegistration* registration, D2RL::Panels::RegistrationHandle* handle) noexcept {
	if (registration->flags != D2RL::Panels::PanelFlags::CloseOnEscape && registration->flags != D2RL::Panels::PanelFlags::None) {
		return D2RL::Panels::Result::Unsupported; // D2RLoader 1.3.1 knows no other panel flags
	}
	for (size_t i = 0; i < g_panelsFake.size(); ++i) {
		if (g_panelsFake[i].localId == registration->localId) {
			*handle = i + 1;
			return D2RL::Panels::Result::Success;
		}
	}
	return D2RL::Panels::Result::NotFound;
}
D2RL::Panels::Result __cdecl PanelInfo(const D2RL::PluginContext*, D2RL::Panels::RegistrationHandle handle, D2RL::Panels::PanelInfo* info) noexcept {
	info->presentationState = g_panelsFake[handle - 1].open ? D2RL::Panels::PresentationState::Open : D2RL::Panels::PresentationState::Closed;
	return D2RL::Panels::Result::Success;
}
D2RL::Panels::Result __cdecl OpenPanel(const D2RL::PluginContext*, D2RL::Panels::RegistrationHandle handle) noexcept {
	FakePanel& panel = g_panelsFake[handle - 1];
	if (panel.layout.empty()) {
		return D2RL::Panels::Result::NotFound;
	}
	if (!panel.built) {
		panel.loaded = panel.layout;
		panel.built  = true;
	}
	panel.open = true;
	return D2RL::Panels::Result::Success;
}
D2RL::Panels::Result __cdecl ClosePanel(const D2RL::PluginContext*, D2RL::Panels::RegistrationHandle handle) noexcept {
	g_panelsFake[handle - 1].open     = false;
	g_panelsFake[handle - 1].closedAt = GetTickCount64();
	return D2RL::Panels::Result::Success;
}
D2RL::PanelService g_panelService {};

D2RL::Widgets::Result __cdecl DispatchUiAction(const D2RL::PluginContext*, const D2RL::Widgets::UiAction* action) noexcept {
	if (strcmp(action->target, "PanelManager") != 0 || strcmp(action->command, "UnloadPanel") != 0) {
		return D2RL::Widgets::Result::InvalidArgument;
	}
	for (FakePanel& panel : g_panelsFake) {
		if ("mod-book/" + panel.localId == action->text) {
			if (panel.open || GetTickCount64() - panel.closedAt < 1500) {
				++g_breaches;
			}
			panel.built = false;
			++g_unloads;
			return D2RL::Widgets::Result::Success;
		}
	}
	return D2RL::Widgets::Result::NotFound;
}
D2RL::WidgetService g_widgetService {};

// The book on screen (g_view: "RecipeBook" or "Legend"): its open panel (two of one book open
// at once is a breach).
std::string g_view = "Left";
FakePanel* OpenBook() {
	FakePanel* found = nullptr;
	for (FakePanel& panel : g_panelsFake) {
		if (panel.open && panel.localId.rfind(g_view, 0) == 0) {
			g_breaches += found != nullptr ? 1 : 0;
			found = &panel;
		}
	}
	return found;
}
// What a widget of the panel on screen shows ("<none>" when its layout has no such widget).
std::string Widget(const char* name) {
	const FakePanel* panel = OpenBook();
	if (panel == nullptr) {
		return "<closed>";
	}
	const size_t at = panel->loaded.find("\"name\": \"" + std::string(name) + "\"");
	if (at == std::string::npos) {
		return "<none>";
	}
	size_t       text = panel->loaded.find("\"text\": \"", at);
	const size_t end  = panel->loaded.find('\n', at);
	bool         onButton = false; // a button with its text on it: read here as the word button it was, "[ Save ]"
	if (text == std::string::npos || text > end) {
		text     = panel->loaded.find("\"textString\": \"", at);
		onButton = true;
		if (text == std::string::npos || text > end) {
			return "";
		}
		text += 6;
	}
	std::string out;
	const auto  whole = [&](std::string t) { return onButton ? "[ " + t + " ]" : t; };
	for (size_t i = text + 9; i < panel->loaded.size() && panel->loaded[i] != '"'; ++i) {
		if (panel->loaded[i] == '\\') {
			++i;
		}
		out += panel->loaded[i];
	}
	return whole(out);
}

D2RL::SharedEvents::UiMessageCallback g_uiListener = nullptr;
D2RL::SharedEvents::Result __cdecl RegisterUiListener(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageListener* listener, D2RL::SharedEvents::ListenerHandle* handle) noexcept {
	g_uiListener = listener->callback;
	*handle      = 1;
	return D2RL::SharedEvents::Result::Success;
}
D2RL::SharedEventService g_eventService {};

D2RL::Input::ActionCallback      g_action      = nullptr, g_legendAction = nullptr;
void*                            g_actionData  = nullptr, *g_legendActionData = nullptr;
D2RL::Input::KeyListenerCallback g_keyListener = nullptr;
D2RL::Input::Result __cdecl RegisterAction(const D2RL::PluginContext*, const D2RL::Input::ActionRegistration* registration, D2RL::Input::ActionHandle* handle) noexcept {
	if (strcmp(registration->logicalId, "book") == 0) {
		g_action     = registration->callback;
		g_actionData = registration->userData;
	} else {
		g_legendAction     = registration->callback;
		g_legendActionData = registration->userData;
	}
	*handle = 1;
	return D2RL::Input::Result::Success;
}
D2RL::Input::Result __cdecl RegisterKeys(const D2RL::PluginContext*, const D2RL::Input::KeyListenerRegistration* registration, D2RL::Input::KeyListenerHandle* handle) noexcept {
	g_keyListener = registration->callback;
	*handle       = 1;
	return D2RL::Input::Result::Success;
}
D2RL::InputService g_inputService {};

D2RL::ServiceQueryResult __cdecl QueryService(const D2RL::PluginContext*, D2RL::ServiceId id, uint32_t, const void** service) noexcept {
	*service = id == D2RL::ServiceId::Thread ? static_cast<const void*>(&g_threadService)
	         : id == D2RL::ServiceId::Lifecycle ? static_cast<const void*>(&g_lifecycle)
	         : id == D2RL::ServiceId::DataTable ? static_cast<const void*>(&g_tableService)
	         : id == D2RL::ServiceId::Resource ? static_cast<const void*>(&g_resourceService)
	         : id == D2RL::ServiceId::Panel ? static_cast<const void*>(&g_panelService)
	         : id == D2RL::ServiceId::Widget ? static_cast<const void*>(&g_widgetService)
	         : id == D2RL::ServiceId::SharedEvent ? static_cast<const void*>(&g_eventService)
	         : id == D2RL::ServiceId::Input ? static_cast<const void*>(&g_inputService)
	         : nullptr;
	return *service != nullptr ? D2RL::ServiceQueryResult::Success : D2RL::ServiceQueryResult::UnknownService;
}

D2RL::PluginContext g_context {};

// Lets the loader's threads run for a while: UI jobs always, game jobs only while a game runs.
void Pump(int ms, const std::function<bool()>& until = nullptr) {
	for (int waited = 0; waited < ms; waited += 20) {
		std::vector<Queued> game, ui;
		EnterCriticalSection(&g_queueLock);
		if (g_gameMode == 0) {
			game.swap(g_gameJobs);
		}
		ui.swap(g_uiJobs);
		LeaveCriticalSection(&g_queueLock);
		for (const Queued& job : game) {
			job.callback(&g_context, job.data);
			++g_ranOnGame;
		}
		for (const Queued& job : ui) {
			job.callback(&g_context, job.data);
			++g_ranOnUi;
		}
		if (until && until()) {
			return;
		}
		Sleep(20);
	}
}

bool Logged(const char* part, size_t from = 0) {
	for (size_t i = from; i < g_log.size(); ++i) {
		if (g_log[i].find(part) != std::string::npos) {
			return true;
		}
	}
	return false;
}
bool Consoled(const char* part, size_t from = 0) {
	for (size_t i = from; i < g_console.size(); ++i) {
		if (g_console[i].find(part) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// The list the game should have: `lines` by index into the stock file's records, with others at the end.
struct Want {
	int                  stockLine; // >= 0: that stock record
	std::vector<uint8_t> record;    // else this one
};
bool ListIs(Vector* vector, const std::vector<Want>& want, uint8_t version, const Stock& stock, int others, std::string& why) {
	if (vector->count != want.size() + static_cast<size_t>(others)) {
		why = "count " + std::to_string(vector->count) + ", wanted " + std::to_string(want.size() + static_cast<size_t>(others));
		return false;
	}
	for (size_t i = 0; i < want.size(); ++i) {
		const std::vector<uint8_t> record = want[i].stockLine >= 0 ? GameRecord(version, stock.header, stock.lines[static_cast<size_t>(want[i].stockLine) + 1],
			reinterpret_cast<const uint8_t*>(stock.bin.data()) + 4 + static_cast<size_t>(want[i].stockLine) * kStock, 0) : want[i].record;
		if (!Same(vector->rows + i * kWide, record.data())) {
			why = "record " + std::to_string(i) + " differs";
			return false;
		}
	}
	for (int k = 0; k < others; ++k) {
		const uint8_t* row = vector->rows + (want.size() + static_cast<size_t>(k)) * kWide;
		if (memcmp(row + 0x14, "OTHERPLUG", 9) != 0 || row[0x14 + 9] != '0' + k) {
			why = "the other plugin's record " + std::to_string(k) + " is not at the end";
			return false;
		}
	}
	return true;
}

// "A + B -> C" as the book shows it: "C <- A + B".
std::string ResultFirst(const std::string& text) {
	const size_t arrow = text.rfind(" -> ");
	return arrow == std::string::npos ? text : text.substr(arrow + 4) + " <- " + text.substr(0, arrow);
}

// The user types "recipes" in the console.
void TypeRecipes() {
	D2RL::ConsoleCommandContext command {};
	command.contextSize = D2RL::ConsoleCommandContextSize;
	command.args        = "";
	g_command(nullptr, &command, nullptr);
}

std::vector<std::string> PropertyCodes(const std::string& text) {
	std::vector<std::string> out;
	bool                     first = true;
	for (const std::string& line : Lines(text)) {
		const std::string code = Split(line, '\t')[0];
		if (!first && code != "Expansion") {
			out.push_back(code);
		}
		first = false;
	}
	return out;
}

}

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("usage: fake_host <plugin.dll> <work folder> <game excel folder>\n");
		return 2;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	InitializeCriticalSection(&g_queueLock);
	const std::string work = argv[2], excel = argv[3];

	// the game's own files
	for (int b = 0; b < 2; ++b) {
		const std::string folder = excel + (b == 0 ? "\\" : "\\base\\");
		g_stock[b].lines  = Lines(ReadText(folder + "cubemain.txt"));
		g_stock[b].bin    = ReadText(folder + "cubemain.bin");
		g_properties[b]   = PropertyCodes(ReadText(folder + "Properties.txt"));
		if (g_stock[b].lines.size() < 10 || g_stock[b].bin.size() != 4 + (g_stock[b].lines.size() - 1) * kStock || g_properties[b].size() < 100) {
			printf("FAIL: the game's files are not in %s\n", folder.c_str());
			return 2;
		}
		g_stock[b].header = Split(g_stock[b].lines[0], '\t');
	}

	// the mod: the game's files as loose files
	const std::string mod = work + "\\mods\\TestMod";
	CreateDirectoryA((work + "\\mods").c_str(), nullptr);
	CreateDirectoryA(mod.c_str(), nullptr);
	std::string path = mod + "\\TestMod.mpq";
	for (const char* part : { "", "\\data", "\\global", "\\excel", "\\base" }) {
		path += part;
		CreateDirectoryA(path.c_str(), nullptr);
	}
	const std::string rotwFile = mod + "\\TestMod.mpq\\data\\global\\excel\\cubemain.txt";
	{ // the mod's copy of the game's button picture: 4 frames of 60 x 30 with gaps
		const std::string folder = mod + "\\TestMod.mpq\\data\\hd\\global\\ui\\panel\\modals";
		std::string       at;
		for (size_t i = 0; i <= folder.size(); ++i) {
			if (i == folder.size() || folder[i] == '\\') {
				CreateDirectoryA(folder.substr(0, i).c_str(), nullptr);
			}
		}
		std::string picture(40 + 243 * 30 * 4, '\x80');
		memcpy(picture.data(), "SpA1", 4);
		memset(picture.data() + 4, 0, 36);
		picture[4] = 31; picture[6] = 60; picture[8] = static_cast<char>(243); picture[12] = 30; picture[20] = 4;
		WriteText(folder + "\\modalbutton.sprite", picture);
	}
	const std::string baseFile = mod + "\\TestMod.mpq\\data\\global\\excel\\base\\cubemain.txt";
	WriteText(rotwFile, Join(g_stock[0].lines));
	WriteText(baseFile, Join(g_stock[1].lines));
	WriteText(mod + "\\TestMod.mpq\\data\\global\\excel\\itemtypes.txt", "ItemType\tCode\tEquiv1\tEquiv2\r\nArmor\tarmo\t\t\r\nAny Shield\tshld\tarmo\t\r\nAxe\taxe\t\t\r\n");
	WriteText(mod + "\\TestMod.mpq\\data\\global\\excel\\weapons.txt", "name\ttype\tcode\r\nHand Axe\taxe\thax\r\n");

	// the "game image": a header with the test stamp, and the game's functions listed at +0x2000
	uint8_t* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	auto*    dos   = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
	dos->e_magic   = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew  = 0x80;
	auto* nt       = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + 0x80);
	nt->Signature  = IMAGE_NT_SIGNATURE;
	nt->FileHeader.TimeDateStamp = 0x7e577e57;
	void** list    = reinterpret_cast<void**>(image + 0x2000);
	list[0]        = reinterpret_cast<void*>(&GameTables);
	list[1]        = reinterpret_cast<void*>(&GameMod);
	list[2]        = reinterpret_cast<void*>(&GameParam);
	list[3]        = reinterpret_cast<void*>(&GameLookup);

	const HMODULE core = LoadLibraryA("D2RCore.dll");
	g_input            = core != nullptr ? reinterpret_cast<CellFn>(reinterpret_cast<void*>(GetProcAddress(core, "CompileCubeInputField"))) : nullptr;
	g_output           = core != nullptr ? reinterpret_cast<CellFn>(reinterpret_cast<void*>(GetProcAddress(core, "CompileCubeOutputField"))) : nullptr;
	Check(g_input != nullptr && g_output != nullptr, "the stand-in D2RCore.dll is loaded");
	if (g_input == nullptr || g_output == nullptr) {
		return 2;
	}

	static D2RL::PluginApi api {};
	api.apiSize                = D2RL::PluginApiSize;
	api.logInfo = api.logWarn = api.logError = LogLine;
	api.ensureConfig           = EnsureConfig;
	api.readConfig             = ReadConfig;
	api.registerConsoleCommand = RegisterCommand;
	api.writeConsoleMessage    = ConsoleLine;
	api.queryService           = QueryService;
	g_lifecycle.serviceSize    = D2RL::LifecycleServiceSize;
	g_lifecycle.serviceVersion = D2RL::LifecycleService::AbiVersion;
	g_lifecycle.registerDataTablesLoadedListener = RegisterTables;
	g_panelService.serviceSize    = D2RL::PanelServiceSize;
	g_panelService.serviceVersion = D2RL::PanelService::AbiVersion;
	g_panelService.registerPanel  = RegisterPanel;
	g_panelService.getPanelInfo   = PanelInfo;
	g_panelService.openPanel      = OpenPanel;
	g_panelService.closePanel     = ClosePanel;
	g_widgetService.serviceSize      = D2RL::WidgetServiceRequiredSize; // 64 bytes: ends after getInputText, as in D2RLoader 1.3.1
	g_widgetService.serviceVersion   = D2RL::WidgetService::AbiVersion;
	g_widgetService.dispatchUiAction = DispatchUiAction;
	g_eventService.serviceSize               = D2RL::SharedEventServiceSize;
	g_eventService.serviceVersion            = D2RL::SharedEventService::AbiVersion;
	g_eventService.registerUiMessageListener = RegisterUiListener;
	g_inputService.serviceSize         = D2RL::InputServiceSize;
	g_inputService.serviceVersion      = D2RL::InputService::AbiVersion;
	g_inputService.serviceSize         = D2RL::InputServiceRequiredSize; // no key listener in D2RLoader 1.3.1
	g_inputService.registerAction      = RegisterAction;

	static std::wstring modDirectory(mod.begin(), mod.end());
	g_context.contextSize  = sizeof(D2RL::PluginContext);
	g_context.abiVersion   = D2RL_PLUGIN_ABI_VERSION;
	g_context.api          = &api;
	g_context.loadScope    = D2RL::LoadScope::Global;
	g_context.exeBase      = reinterpret_cast<uintptr_t>(image);
	g_context.pluginId     = "live-cube-recipes";
	g_context.activeMod    = "TestMod";
	g_context.modDirectory = modDirectory.c_str();
	const std::string   plugins = work + "\\d2rloader\\plugins";
	CreateDirectoryA((work + "\\d2rloader").c_str(), nullptr);
	CreateDirectoryA(plugins.c_str(), nullptr);
	static std::wstring pluginDirectory(plugins.begin(), plugins.end());
	g_context.pluginDirectory = pluginDirectory.c_str();
	g_context.buildVersion = "test";

	// the Mod Book first (argv[4]): the panels are its; this plugin puts its books in as tabs
	const HMODULE modBook = argc > 4 ? LoadLibraryA(argv[4]) : nullptr;
	const auto    bookLoad = modBook != nullptr ? reinterpret_cast<D2RL::LoadPluginFn>(reinterpret_cast<void*>(GetProcAddress(modBook, D2RL_PLUGIN_LOAD_EXPORT))) : nullptr;
	Check(bookLoad != nullptr && bookLoad(&g_context), "the Mod Book plugin loads and starts");
	const HMODULE liveProps = LoadLibraryA("d2rl-live-properties.dll"); // a stand-in: knows the made property "my-prop"
	const HMODULE liveTables = LoadLibraryA("d2rl-live-tables.dll"); // a stand-in: API 3, its change listener and remove veto only
	const HMODULE plugin = LoadLibraryA(argv[1]);
	Check(plugin != nullptr, "the plugin loads");
	if (plugin == nullptr) {
		return 2;
	}
	const auto getInfo = reinterpret_cast<D2RL::GetPluginInfoFn>(reinterpret_cast<void*>(GetProcAddress(plugin, D2RL_PLUGIN_GET_INFO_EXPORT)));
	const auto load    = reinterpret_cast<D2RL::LoadPluginFn>(reinterpret_cast<void*>(GetProcAddress(plugin, D2RL_PLUGIN_LOAD_EXPORT)));
	const auto unload  = reinterpret_cast<D2RL::UnloadPluginFn>(reinterpret_cast<void*>(GetProcAddress(plugin, D2RL_PLUGIN_UNLOAD_EXPORT)));
	Check(getInfo != nullptr && load != nullptr && unload != nullptr && strcmp(getInfo()->id, "live-cube-recipes") == 0, "it has the three plugin entry points");
	Check(load(&g_context) && g_tablesListener != nullptr && g_command != nullptr, "it starts, asks for the tables-read notice and registers its console command");

	// what is in the mod's folder now: it must be the same at the end
	const auto modState = [&] {
		std::string      state;
		WIN32_FIND_DATAA entry {};
		for (const std::string& folder : { mod, mod + "\\TestMod.mpq\\data\\global\\excel", mod + "\\TestMod.mpq\\data\\global\\excel\\base" }) {
			const HANDLE find = FindFirstFileA((folder + "\\*").c_str(), &entry);
			do {
				state += std::string(entry.cFileName) + ":" + std::to_string(entry.nFileSizeLow) + ":" + std::to_string(entry.ftLastWriteTime.dwLowDateTime) + ";";
			} while (FindNextFileA(find, &entry) != 0);
			FindClose(find);
		}
		return state + ReadText(rotwFile) + ReadText(baseFile);
	};
	const std::string modBefore = modState();

	// ---- the game reads its tables
	const int kOthers = 3;
	LoadTables(3, g_rotw, g_stock[0], kOthers);
	LoadTables(2, g_base, g_stock[1], 0);
	Vector*                    rotw     = g_rotw.vector();
	Vector*                    base     = g_base.vector();
	const uint8_t* const       gameRows = rotw->rows;
	const std::vector<uint8_t> gameCopy(rotw->rows, rotw->rows + rotw->count * kWide);
	D2RL::Lifecycle::DataTablesLoadedEvent event { D2RL::Lifecycle::DataTablesLoadedEventSize, 0, 1 };
	// a folder in the old place (before the Live Suite folder) is moved over at the first start
	const std::string oldOwn = work + "\\d2rloader\\live-cube-recipes\\TestMod";
	CreateDirectoryA((work + "\\d2rloader").c_str(), nullptr);
	CreateDirectoryA((work + "\\d2rloader\\live-cube-recipes").c_str(), nullptr);
	CreateDirectoryA(oldOwn.c_str(), nullptr);
	WriteText(oldOwn + "\\kept.txt", "a file of the user's");
	g_tablesListener(&g_context, &event, nullptr);
	const std::string own      = work + "\\d2rloader\\live-suite\\live-cube-recipes\\TestMod";
	Check(ReadText(own + "\\kept.txt") == "a file of the user's" && GetFileAttributesA(oldOwn.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA((work + "\\d2rloader\\live-cube-recipes").c_str()) == INVALID_FILE_ATTRIBUTES
			&& Logged("your files were moved from the old place, d2rloader\\live-cube-recipes, to d2rloader\\live-suite"),
		"the plugin's folder in the old place (d2rloader\\live-cube-recipes) is moved into d2rloader\\live-suite with what it holds");
	const std::string mineFile = own + "\\recipes.txt", mineBase = own + "\\recipes-classic-lod.txt";
	Check(Logged("check (read only) of 16 lines of the mod's cubemain.txt against the game's own records: 16 the same at the line's place, 0 the same elsewhere in the list, 0 not found"),
		"at start, sampled lines of the mod's file compile to exactly the game's records");
	Check(Logged("recipes-classic-lod.txt: your recipes for data version 2") && !Logged("data version 1 are read"), "the two old data versions share one list");
	const std::string made = ReadText(mineFile);
	Check(made.rfind("description\tenabled\t", 0) == 0 && Lines(made).size() == 1 && !ReadText(mineBase).empty() && ReadText(own + "\\README.txt").find("Restore deleted defaults") != std::string::npos,
		"the plugin's own folder is made in d2rloader, with empty recipe files (first line only) and a README");
	{
		const std::vector<std::string> all = Lines(ReadText(own + "\\all-recipes.txt"));
		const std::string firstAbout = Split(g_stock[0].lines[1], '\t')[0], lastAbout = Split(g_stock[0].lines.back(), '\t')[0];
		Check(all.size() == 1 + 227 + 3 && all[0] == made.substr(0, made.find('\r')) && Split(all[1], '\t')[0] == firstAbout && Split(all[227], '\t')[0] == lastAbout
				&& all[228].find("(recipe 228 of the cube's list: no recipe file has a line for it") == 0 && Logged("all-recipes.txt lists the cube's 230 recipes - 3 of them have no line in any recipe file"),
			"at start all-recipes.txt holds every recipe of the cube in order: 227 with the mod's text, the 3 that no file has a line for named as such");
		Check(Lines(ReadText(own + "\\all-recipes-classic-lod.txt")).size() == 1 + 155, "and all-recipes-classic-lod.txt the old data sets' 155");
		const std::string legend = ReadText(own + "\\legend.txt");
		Check(legend.find("  rar     rare (yellow name)") != std::string::npos && legend.find("  shld  Any Shield") != std::string::npos && legend.find("  hax   Hand Axe") != std::string::npos
				&& !ReadText(own + "\\legend-classic-lod.txt").empty(),
			"legend.txt explains the recipe words and lists the mod's item types and items (read from the mod's tables)");
	}
	Pump(600);
	Check(rotw->rows == gameRows && memcmp(rotw->rows, gameCopy.data(), gameCopy.size()) == 0 && (rotw->capacity & kNotOwned) == 0, "with no recipes of the user's the game's list is left alone");

	// ---- the recipe book
	const auto bookKey   = reinterpret_cast<bool (*)(unsigned, int)>(reinterpret_cast<void*>(GetProcAddress(modBook, "ModBookKey")));
	const auto bookWheel = reinterpret_cast<void (*)(int)>(reinterpret_cast<void*>(GetProcAddress(modBook, "ModBookWheel")));
	const auto type = [&](const char* text) {
		for (const char* c = text; *c != '\0'; ++c) {
			bookKey(*c == ' ' ? VK_SPACE : *c == ',' ? VK_OEM_COMMA : *c == '=' ? VK_OEM_PLUS : static_cast<unsigned>(toupper(*c)), 0);
		}
		Pump(120);
	};
	{
		Check(g_panelsFake.size() == 64 && g_legendCommand != nullptr && g_legendAction != nullptr && g_bookCommand != nullptr && g_uiListener != nullptr && g_action != nullptr && bookKey != nullptr && bookWheel != nullptr && Logged("panels: ready"),
			"on a loader as short as D2RLoader 1.3.1 the two books are set up: 32 panels each, console commands, keys, message listener");
		const auto about = [&](size_t line) { // a title as the panel has room for it: the result first
			const std::string text = ResultFirst(Split(g_stock[0].lines[line + 1], '\t')[0]);
			return text.size() <= 58 ? text : text.substr(0, 55) + "...";
		};
		const auto click = [&](const char* button) {
			if (Widget(button) == "<none>") {
				return false; // no such button on the page
			}
			const std::string name = "mod-book/Click" + g_view + button;
			D2RL::SharedEvents::UiMessageEvent ui {};
			ui.structSize = D2RL::SharedEvents::UiMessageEventSize;
			ui.target     = "PanelManager";
			ui.command    = "ClosePanel";
			ui.text       = name.c_str();
			const bool consumed = g_uiListener(&g_context, &ui, nullptr) == D2RL::SharedEvents::UiMessageAction::Consume;
			Pump(120);
			return consumed;
		};
		const auto key = [&](unsigned which) {
			const bool taken = bookKey(which, 0);
			Pump(120);
			return taken;
		};
		D2RL::SharedEvents::UiMessageEvent other {};
		other.structSize = D2RL::SharedEvents::UiMessageEventSize;
		other.target     = "PanelManager";
		other.command    = "ClosePanel";
		other.text       = "QuestLogPanel";
		Check(g_uiListener(&g_context, &other, nullptr) == D2RL::SharedEvents::UiMessageAction::Continue && !key(VK_NEXT) && !key('I'), "other panels' messages and, with the book closed, every key are left to the game");
		D2RL::ConsoleCommandContext open {};
		open.contextSize = D2RL::ConsoleCommandContextSize;
		open.args        = "";
		g_bookCommand(nullptr, &open, g_bookData);
		Pump(120);
		Check(OpenBook() != nullptr && Widget("Title0") == about(0) && Widget("Title7") == about(7) && Widget("Page") == "1 - 8 of 227" && Widget("Previous") == "<none>" && Widget("Next") != "<none>"
				&& Widget("Search") == "Type to search the recipes",
			"recipebook opens the panel on the first 8 recipes (" + Widget("Title0") + " | " + Widget("Detail0") + " | " + Widget("Page") + ")");
		Check(g_buttonPictures == 32 && Logged("buttons: the game's button picture (from the mod) in 16 widths") && OpenBook()->loaded.find("\"name\": \"DoNew\", \"fields\": { \"rect\": { \"x\": 82, \"y\": 262 }, \"filename\": \"d2rloader\\\\mod-book\\\\button232\"") != std::string::npos
				&& OpenBook()->loaded.find("\"hoveredFrame\": 3") != std::string::npos && OpenBook()->loaded.find("mod-book/ClickLeftNew\", \"textString\": \"New recipe\"") != std::string::npos && Widget("ClickNew") == "<none>",
			"buttons are the game's own button picture, resized for each width (and at half size for low resolution), with the text on the button");
		Check(Widget("Detail0") == "hst  <-  msf + vip", "each recipe has its result and, after it, what goes in under it (" + Widget("Detail0") + ")");
		{
			// line 3 is longer than the panel is wide: cut in the row, whole in its hover text
			const std::string& layout = OpenBook()->loaded;
			const size_t       hover  = layout.find("\"name\": \"Hover3\"");
			const std::string  tip    = hover == std::string::npos ? "" : layout.substr(layout.find("\"tooltipString\": \"", hover) + 18, 400);
			const std::string  whole  = ResultFirst(Split(g_stock[0].lines[4], '\t')[0]);
			Check(Widget("Title3").size() == 58 && Widget("Title3").substr(55) == "..." && whole.size() > 58 && tip.find(whole.substr(whole.size() - 20)) != std::string::npos && tip.find("\\n") != std::string::npos
					&& tip.find("<-") != std::string::npos && layout.find("d2rloader\\\\mod-book\\\\blank") != std::string::npos,
				"a long recipe is cut in its row and whole (in lines) in the hover text of an unseen button over the row");
		}
		const FakePanel* first = OpenBook();
		Check(click("Next") && Widget("Title0") == about(8) && Widget("Page") == "9 - 16 of 227" && Widget("Previous") != "<none>" && OpenBook() != first && !first->open,
			"the right arrow turns the page: the next panel of the pool is opened with it, the one before closed");
		Check(key(VK_NEXT) && Widget("Title0") == about(16) && key(VK_PRIOR) && Widget("Title0") == about(8), "Page Down and Page Up turn pages and are kept from the game");
		bookWheel(-120);
		Pump(120);
		const bool down = Widget("Title0") == about(10) && Widget("Page") == "11 - 18 of 227" && Widget("ScrollTrack") != "<none>" && Widget("ScrollMark") != "<none>";
		bookWheel(120);
		Pump(120);
		Check(down && Widget("Title0") == about(8) && g_scrollPictures == 4, "the mouse wheel scrolls the list two recipes a notch; a line with a mark shows where the list stands");
		Check(key(VK_END) && Widget("Page") == "220 - 227 of 227" && Widget("Next") == "<none>", "End goes to the end of the list, which has no arrow onward");
		Check(key(VK_HOME) && Widget("Page") == "1 - 8 of 227" && Widget("Previous") == "<none>", "Home goes to the first page, which has no arrow back");
		Check(key(VK_DOWN) && Widget("Page") == "2 - 9 of 227" && key(VK_UP) && Widget("Page") == "1 - 8 of 227" && Widget("ScrollTrack") != "<none>", "the Down and Up arrows scroll one recipe");
		Check(!key(VK_F5) && !key(VK_LEFT) && !bookKey('B', 2) && !bookKey('L', 2), "keys that are neither text nor scrolling stay the game's (Ctrl+B and Ctrl+L too)");
		type("amn rune");
		Check(Widget("Search") == "amn rune_" && Widget("Title0").find("Amn Rune") != std::string::npos && Widget("Page").find("1 - ") == 0 && Widget("Page") != "1 - 8 of 227",
			"typed letters go to the search (not to the game) and leave the recipes with every word (" + Widget("Page") + ")");
		Check(key(VK_BACK) && Widget("Search") == "amn run_", "Backspace erases a letter");
		Check(key(VK_DELETE) && Widget("Search") == "Type to search the recipes" && Widget("Page") == "1 - 8 of 227", "Delete clears the search: everything again");
		type("no such recipe");
		Check(Widget("Title0").empty() && Widget("Page") == "nothing has all of these words", "a search that finds nothing says so");
		key(VK_DELETE);
		// many pages in a row: the pool runs out of rested panels and waits, without breaking the rules
		for (int i = 0; i < 140; ++i) { // back and forth, so that every turn is a page to draw
			bookKey(i % 2 == 0 ? VK_NEXT : VK_PRIOR, 0);
			Pump(20);
		}
		bookKey(VK_END, 0);
		Pump(2500, [&] { return Widget("Page") == "220 - 227 of 227"; });
		Check(Widget("Page") == "220 - 227 of 227" && g_breaches == 0 && g_unloads > 0, "140 quick page turns and End end on the last page; panels are reused only after their 1.5 s rest (" + std::to_string(g_unloads) + " reused)");
		key(VK_HOME);
		D2RL::Input::ActionEvent press {};
		press.structSize = D2RL::Input::ActionEventSize;
		press.kind       = D2RL::Input::ActionEventKind::Pressed;
		g_action(&g_context, &press, g_actionData);
		Pump(120);
		Check(OpenBook() == nullptr && !bookKey('A', 0), "the key closes the open book; letters are the game's again");
		Pump(1600); // the pool rests after the forty pages
		g_action(&g_context, &press, g_actionData);
		Pump(120);
		Check(OpenBook() != nullptr && Widget("Page") == "1 - 8 of 227", "and opens it again");

		// ---- the legend: two sections of the Mod Book's Legend, on the other side
		const auto bookSide = reinterpret_cast<void (*)(int)>(reinterpret_cast<void*>(GetProcAddress(modBook, "ModBookSide")));
		const auto rowLine  = [&](int i) { // a Legend row as "code - meaning" (its two labels)
			return Widget(("Code" + std::to_string(i)).c_str()) + " - " + Widget(("Meaning" + std::to_string(i)).c_str());
		};
		const auto rowOf    = [&](const std::string& start) { // the Legend's row that starts so, or -1
			for (int i = 0; i < 40; ++i) {
				if (rowLine(i).rfind(start, 0) == 0) {
					return i;
				}
			}
			return -1;
		};
		g_legendCommand(nullptr, &open, g_legendData);
		Pump(120);
		g_view = "Right";
		const FakePanel* legendPanel = OpenBook();
		Check(legendPanel != nullptr && legendPanel->loaded.find("$RightPanelRect") != std::string::npos && legendPanel->loaded.find("$RightHingeSprite") != std::string::npos
				&& Widget("DoSec0") == "[ Recipe codes ]" && Widget("DoSec1") == "[ Recipe columns ]" && rowLine(0).rfind("Reading a recipe - A recipe's description", 0) == 0
				&& rowOf("nor - normal quality") >= 0 && Widget("Search") != "<none>",
			"recipelegend opens the Mod Book's Legend on the right, on Recipe codes: How it works first, then each entry as its code with its meaning under it (" + rowLine(0) + " | " + Widget("Page") + ")");
		{
			const auto legendClick = [&](const char* which) {
				const std::string name = std::string("mod-book/ClickRight") + which;
				D2RL::SharedEvents::UiMessageEvent ui {};
				ui.structSize = D2RL::SharedEvents::UiMessageEventSize;
				ui.target     = "PanelManager";
				ui.command    = "ClosePanel";
				ui.text       = name.c_str();
				g_uiListener(&g_context, &ui, nullptr);
				Pump(150);
			};
			const std::string firstPage = Widget("Page");
			legendClick("Sec1");
			const bool second = rowLine(0).rfind("The columns of a recipe - ", 0) == 0 && rowOf("op - one extra condition") >= 0 && Widget("Page") != firstPage;
			bookSide(1);
			type("op 18");
			const bool found = rowOf("op 18   (Recipe columns) - the input item's stat param") >= 0;
			bookKey(VK_DELETE, 0); // Delete empties the search
			Pump(150);
			const bool emptied = rowOf("The columns of a recipe - ") == 0;
			type("class = war");
			const bool value = rowOf("class = war   (Recipe columns) - only a Warlock can use the recipe") >= 0;
			const std::string what = rowLine(0) + " | " + Widget("Page");
			legendClick("Sec0");
			Check(second && found && emptied && value && rowLine(0).rfind("Reading a recipe - ", 0) == 0 && Widget("Page") == firstPage,
				"the Recipe columns section has the columns and their values (op 18, class = war; Delete empties the search); Recipe codes comes back as it was (" + what + ")");
			// the player drags the Legend's tab button onto the recipe book's tab on the left, then back
			const auto drag = reinterpret_cast<void (*)(const char*, int, const char*)>(reinterpret_cast<void*>(GetProcAddress(modBook, "ModBookDrag")));
			char exe[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			const std::string orderFile = std::string(exe).substr(0, std::string(exe).find_last_of('\\')) + "\\d2rloader\\live-suite\\mod-book\\tab-order.txt"; // the game's folder = the host's
			DeleteFileA(orderFile.c_str());
			bool dragged = false, back = false;
			std::string order, why;
			if (drag != nullptr) {
				drag("legend", 0, "recipes");
				Pump(400);
				g_view = "Left";
				dragged = OpenBook() != nullptr && Widget("DoTab0") == "[ Legend ]" && Widget("DoTab1") == "[ Cube Recipes ]";
				why     = Widget("DoTab0") + " " + Widget("DoTab1");
				order   = ReadText(orderFile);
				dragged = dragged && order.find("Left\tlegend\t") != std::string::npos && order.find("Left\trecipes\t") != std::string::npos;
				drag("legend", 1, nullptr);
				Pump(400);
				g_view = "Left";
				back   = OpenBook() != nullptr && Widget("DoTab0") == "<none>";
				g_view = "Right";
				order  = ReadText(orderFile);
				why += " | " + std::to_string(dragged) + std::to_string(back) + " | " + order;
				back = back && order.find("Right\tlegend\tGeneral") != std::string::npos && order.find("Left\tlegend") == std::string::npos;
			}
			DeleteFileA(orderFile.c_str());
			Check(dragged && back, "a tab's button dragged to the other side: the tab goes there and tab-order.txt keeps the order; dragged back it takes its group again (" + why + ")");
			if (OpenBook() == nullptr) { // the Legend on screen again for what follows
				g_legendCommand(nullptr, &open, g_legendData);
				Pump(150);
			}
			legendClick("Sec0");
			bookSide(-1);
		}
		g_view = "Left";
		Check(OpenBook() != nullptr && Widget("Page") == "1 - 8 of 227", "the recipe book stays open on the left");
		bookSide(1); // the cursor is over the legend
		type("shld");
		g_view = "Right";
		const bool shield = rowOf("shld   (Recipe codes) - Any Shield") >= 0;
		Check(shield && Widget("Search") == "shld_", "with the cursor over the legend, typing looks a code up there (" + Widget("Page") + ")");
		g_view = "Left";
		Check(Widget("Search") == "Type to search the recipes" && Widget("Page") == "1 - 8 of 227", "and the recipe book is not searched");
		bookSide(0); // the cursor is over the recipe book
		bookWheel(-120);
		Pump(120);
		Check(Widget("Page") == "3 - 10 of 227", "with the cursor over the recipe book, the wheel scrolls it");
		g_view = "Right";
		Check(Widget("Search") == "shld_", "and the legend stays as it was");
		g_legendAction(&g_context, &press, g_legendActionData);
		Pump(120);
		Check(OpenBook() == nullptr, "the legend's key closes it");
		g_view = "Left";
		bookKey(VK_HOME, 0);
		Pump(120);
		Check(OpenBook() != nullptr && bookKey('A', 0) && bookKey(VK_DELETE, 0), "the recipe book is still open and has the keyboard");
		Pump(120);
		bookSide(-1);
	}

	// ---- the user writes recipes while a game runs
	const size_t stockCount = g_stock[0].lines.size() - 1;
	const int    lvl = Column(g_stock[0].header, "lvl"), input1 = Column(g_stock[0].header, "input 1"), enabled = Column(g_stock[0].header, "enabled");
	const auto   with = [&](const std::string& line, int column, const std::string& value) {
        std::vector<std::string> cells = Split(line, '\t');
        cells[static_cast<size_t>(column)] = value;
        std::string out;
        for (size_t i = 0; i < cells.size(); ++i) {
            out += (i != 0 ? "\t" : "") + cells[i];
        }
        return out;
	};
	const auto record = [&](size_t line) { return GameRecord(3, g_stock[0].header, g_stock[0].lines[line + 1], reinterpret_cast<const uint8_t*>(g_stock[0].bin.data()) + 4 + line * kStock, 0); };
	// the game's line 50 with another level (same ingredients: stands in its place);
	// line 100 with another first input (a new recipe: in front); line 3 with enabled 0 (removed)
	static_cast<void>(enabled);
	std::vector<std::string> mine = { g_stock[0].lines[0] + "\t*action", with(g_stock[0].lines[51], lvl, "77"), with(g_stock[0].lines[101], input1, "\"hax,qty=2\""), g_stock[0].lines[4] + "\tremove" };
	std::vector<uint8_t>     changed = record(50), fresh = record(100);
	changed[0x5a + 0xe] = 77;
	memset(fresh.data() + 0x14, 0, 10);
	char hax[] = "\"hax,qty=2\"";
	g_input(3, hax, fresh.data(), 0, 0, 0);
	std::vector<Want> want;
	want.push_back({ -1, fresh });
	for (size_t i = 0; i < stockCount; ++i) {
		if (i != 3) {
			want.push_back(i == 50 ? Want { -1, changed } : Want { static_cast<int>(i), {} });
		}
	}
	size_t consoleFrom = g_console.size(), logFrom = g_log.size();
	const int jobsBefore = g_ranOnGame + g_ranOnUi;
	WriteText(mineFile, Join(mine));
	std::string why;
	Pump(1500);
	static_cast<void>(jobsBefore); // (the open recipe book asks for UI jobs of its own)
	Check(rotw->rows == gameRows && rotw->count == stockCount + kOthers && !Consoled("recipes.txt:", consoleFrom), "a saved file is not looked at until the command is typed (the list is the game's, nothing said)");
	TypeRecipes();
	Pump(4000, [&] { return rotw->rows != gameRows; });
	Pump(200);
	Check(rotw->rows != gameRows && (rotw->capacity & kNotOwned) != 0, "after a save the game reads a list of the plugin's, marked as not the game's to free");
	Check(ListIs(rotw, want, 3, g_stock[0], kOthers, why), "a new recipe in front, one in place of the game's, one removed; the other plugin's recipes where they were" + (why.empty() ? "" : " (" + why + ")"));
	Check(g_ranOnGame > 0 && Consoled("recipes.txt: the cube has the 227 recipes of this file (70 switched off) and 3 recipes of the game that no file has text for. The cube now knows 230 recipes.", consoleFrom) && ReadText(mineFile).find("*complete") != std::string::npos && Lines(ReadText(mineFile)).size() == 228 && Lines(ReadText(own + "\\deleted.txt")).size() == 2 && !ReadText(mineFile + ".before-0.10").empty(),
		"it was done on the game's thread and said in the console; the file of the old kind (changes to the game's list) was rewritten as the whole list, its removal noted, the old file kept");
	Pump(300);
	Check(OpenBook() != nullptr && Widget("Detail0").find("hax,qty=2") != std::string::npos && Widget("Page") == "1 - 8 of 227", "the open recipe book shows the new list at once (" + Widget("Detail0") + ")");
	WriteText(work + "\\RecipeBookhd.json", OpenBook() != nullptr ? OpenBook()->loaded : std::string());
	OpenBook()->open = false; // closed with Escape: the plugin is not told
	Pump(600);
	Check(!bookKey('A', 0) && g_breaches == 0, "after Escape the plugin notices by itself and gives the keyboard back");
	Check(memcmp(gameRows, gameCopy.data(), gameCopy.size()) == 0, "the game's own list was not written to");
	{
		const std::vector<std::string> all = Lines(ReadText(own + "\\all-recipes.txt"));
		// the user's new recipe first (with its own first input), the changed one at its place with level 77, line 3 gone
		const std::vector<std::string> first = all.size() > 51 ? Split(all[1], '\t') : std::vector<std::string> {};
		const int                      in1   = Column(Split(all[0], '\t'), "input 1"), level = Column(Split(all[0], '\t'), "lvl");
		Check(all.size() == 1 + 230 && in1 > 0 && first[static_cast<size_t>(in1)] == "\"hax,qty=2\"" && Split(all[51], '\t')[static_cast<size_t>(level)] == "77"
				&& Split(all[51], '\t')[0] == Split(g_stock[0].lines[51], '\t')[0] && Split(all[4], '\t')[0] == Split(g_stock[0].lines[3], '\t')[0] && Split(all[5], '\t')[0] == Split(g_stock[0].lines[5], '\t')[0]
				&& Consoled("all-recipes.txt lists the cube's 230 recipes", consoleFrom),
			"after the command all-recipes.txt is the new list: the user's recipes in it, the removed one not");
	}
	Check(base->rows != nullptr && (base->capacity & kNotOwned) == 0, "the other data set was left alone");

	// ---- a line the game cannot read
	std::vector<std::string> bad = mine;
	bad.push_back(with(g_stock[0].lines[11], input1, "zzz"));
	consoleFrom = g_console.size();
	WriteText(mineFile, Join(bad));
	TypeRecipes();
	Pump(3000, [&] { return Consoled("left out until it is fixed", consoleFrom); });
	Pump(200);
	Check(ListIs(rotw, want, 3, g_stock[0], kOthers, why) && Consoled("input 1: the game does not know \"zzz\"", consoleFrom),
		"a line with an unknown item is left out and named in the console; the rest stays" + (why.empty() ? "" : " (" + why + ")"));

	// ---- one more recipe, with no game running: the job waits on the game's thread, then runs on the UI's
	g_gameMode = 1;
	const int            uiBefore = g_ranOnUi;
	const uint8_t* const before2  = rotw->rows;
	mine.push_back(with(g_stock[0].lines[6], input1, "\"hax,qty=3\""));
	std::vector<uint8_t> second = record(5);
	memset(second.data() + 0x14, 0, 10);
	char hax3[] = "\"hax,qty=3\"";
	g_input(3, hax3, second.data(), 0, 0, 0);
	want.insert(want.begin() + 1, { -1, second });
	WriteText(mineFile, Join(mine));
	TypeRecipes();
	Pump(6000, [&] { return rotw->rows != before2; });
	Pump(200);
	Check(ListIs(rotw, want, 3, g_stock[0], kOthers, why) && g_ranOnUi > uiBefore, "without a running game the file is taken in from the UI thread" + (why.empty() ? "" : " (" + why + ")"));
	g_gameMode = 0;
	Pump(200); // the job left on the game's thread runs now and finds nothing to do

	// ---- an editor that deletes the file and writes it again
	DeleteFileA(mineFile.c_str());
	const uint64_t countBefore = rotw->count;
	consoleFrom                = g_console.size();
	TypeRecipes();
	Pump(500);
	Check(rotw->count == countBefore && Consoled("recipes.txt could not be read", consoleFrom), "the command with the file gone: said so, the recipes stay");
	mine.pop_back();
	want.erase(want.begin() + 1);
	WriteText(mineFile, Join(mine));
	TypeRecipes();
	Pump(4000, [&] { return rotw->count != countBefore; });
	Pump(200);
	Check(ListIs(rotw, want, 3, g_stock[0], kOthers, why), "written again with one line less: that recipe is gone" + (why.empty() ? "" : " (" + why + ")"));

	// ---- as a remote client (no game thread at all)
	g_gameMode = 2;
	const uint8_t* const before3 = rotw->rows;
	WriteText(mineFile, Join(std::vector<std::string> { mine[0], mine[1], mine[2] })); // the removal undone
	TypeRecipes();
	std::vector<Want> wantBack;
	wantBack.push_back({ -1, fresh });
	for (size_t i = 0; i < stockCount; ++i) {
		wantBack.push_back(i == 50 ? Want { -1, changed } : Want { static_cast<int>(i), {} });
	}
	Pump(4000, [&] { return rotw->rows != before3; });
	Check(ListIs(rotw, wantBack, 3, g_stock[0], kOthers, why), "a removal deleted from the file: the game's recipe is back (UI thread, no game thread on offer)" + (why.empty() ? "" : " (" + why + ")"));
	g_gameMode = 0;

	// ---- the other data set
	std::vector<Want> wantBase;
	for (size_t i = 1; i + 1 < g_stock[1].lines.size(); ++i) {
		wantBase.push_back({ static_cast<int>(i), {} });
	}
	const uint8_t* const baseBefore = base->rows;
	const uint8_t* const rotwNow    = rotw->rows;
	WriteText(mineBase, Join(std::vector<std::string> { g_stock[1].lines[0] + "\t*action", g_stock[1].lines[1] + "\tremove" }));
	TypeRecipes();
	Pump(4000, [&] { return base->rows != baseBefore; });
	Pump(200);
	Check(ListIs(base, wantBase, 2, g_stock[1], 0, why) && rotw->rows == rotwNow, "recipes-classic-lod.txt changes the old data sets' list only" + (why.empty() ? "" : " (" + why + ")"));

	// ---- the command again with nothing changed
	consoleFrom = g_console.size();
	const uint8_t* const sameList = rotw->rows;
	TypeRecipes();
	Pump(1000, [&] { return Consoled("nothing changed since it was last read", consoleFrom); });
	Check(rotw->rows == sameList && Consoled("reading", consoleFrom) && Consoled("d2rloader\\live-suite\\live-cube-recipes\\TestMod\\recipes.txt", consoleFrom) && Consoled("nothing changed since it was last read", consoleFrom),
		"the command with an unchanged file says where the file is and that nothing changed");

	// ---- D2RLoader's reload (or the next start): the game reads its tables again; the file is laid over them at once
	LoadTables(3, g_rotw, g_stock[0], kOthers); // must not free the plugin's list
	LoadTables(2, g_base, g_stock[1], 0);
	event.revision = 2;
	const uint8_t* const reloaded = rotw->rows;
	g_tablesListener(&g_context, &event, nullptr);
	Check(rotw->rows != reloaded && ListIs(rotw, wantBack, 3, g_stock[0], kOthers, why) && ListIs(base, wantBase, 2, g_stock[1], 0, why),
		"when the game reads its tables again the saved recipes are in the cube at once" + (why.empty() ? "" : " (" + why + ")"));
	const uint8_t* const afterReload = rotw->rows;
	WriteText(mineFile, Join(std::vector<std::string> { mine[0] }));
	Pump(800);
	Check(rotw->rows == afterReload, "and a later save waits for the command");
	TypeRecipes();
	std::vector<Want> wantStock;
	for (size_t i = 0; i < stockCount; ++i) {
		wantStock.push_back({ static_cast<int>(i), {} });
	}
	Pump(4000, [&] { return rotw->rows != afterReload; });
	Pump(200);
	Check(ListIs(rotw, wantStock, 3, g_stock[0], kOthers, why), "every line deleted: the cube has the game's recipes again" + (why.empty() ? "" : " (" + why + ")"));

	// ---- the editor in the recipe book. recipes.txt is the cube's whole list; an empty one starts as the game's own
	{
		g_view = "Left";
		const auto click = [&](const std::string& what) {
			const std::string name = "mod-book/ClickLeft" + what;
			D2RL::SharedEvents::UiMessageEvent ui {};
			ui.structSize = D2RL::SharedEvents::UiMessageEventSize;
			ui.target     = "PanelManager";
			ui.command    = "ClosePanel";
			ui.text       = name.c_str();
			g_uiListener(&g_context, &ui, nullptr);
			Pump(150);
		};
		const auto typeText = [&](const char* text) {
			for (const char* c = text; *c != '\0'; ++c) {
				const bool upper = *c >= 'A' && *c <= 'Z';
				bookKey(*c == ' ' ? VK_SPACE : *c == ',' ? VK_OEM_COMMA : *c == '=' ? VK_OEM_PLUS : *c == '-' ? VK_OEM_MINUS : static_cast<unsigned>(toupper(*c)), upper ? 1 : 0);
			}
			Pump(150);
		};
		const auto has  = [&](const char* name) { return Widget(name) != "<none>"; };
		const auto page = [&](unsigned key) {
			bookKey(key, 0);
			Pump(150);
		};
		const std::string notesFile = own + "\\deleted.txt";
		D2RL::ConsoleCommandContext open {};
		open.contextSize = D2RL::ConsoleCommandContextSize;
		open.args        = "";
		D2RL::Input::ActionEvent press {};
		press.structSize = D2RL::Input::ActionEventSize;
		press.kind       = D2RL::Input::ActionEventKind::Pressed;
		WriteText(notesFile, ""); // the notes of the files of the old kind above are not this part's
		LoadTables(3, g_rotw, g_stock[0], kOthers);
		LoadTables(2, g_base, g_stock[1], 0);
		event.revision = 3;
		g_tablesListener(&g_context, &event, nullptr);
		Pump(1600); // the pool rests
		g_bookCommand(nullptr, &open, g_bookData);
		Pump(150);
		const uint8_t* const before = rotw->rows;
		const std::string    first  = Widget("Title0"), second = Widget("Title1");
		Check(OpenBook() != nullptr && has("DoNew") && has("DoSearch") && has("DoRestoreAll") && !has("DoSave") && Widget("Page") == "1 - 8 of 227" && Lines(ReadText(mineFile)).size() == 1,
			"with an empty recipes.txt the book starts as the game's own list; it has New recipe, Search for new recipes, Restore deleted defaults, and no Save");
		click("Row1");
		Check(Widget("Title") == "Recipe" && Widget("What") == "As the mod has it." && has("DoEdit") && has("DoCopy") && Widget("DoToggle") == "[ Disable ]" && has("DoRemove") && has("DoBack")
				&& Widget("Value0").substr(0, 30) == second.substr(0, 30),
			"a click on a recipe opens its card: every cell, and Edit / Copy as new / Disable / Remove");
		click("Toggle");
		Check(Widget("DoSave") == "[ Save (1) ]" && has("DoDiscard") && Widget("Title1").rfind("* " + second.substr(0, 30), 0) == 0 && Widget("Detail1").find("(not enabled)") != std::string::npos && rotw->rows == before,
			"Disable: the list shows it switched off and marked as changed, Save appears; the cube is not touched");
		click("Row0");
		click("Remove");
		const bool asksRemove = Widget("Title") == "Remove a Recipe" && Widget("Ask").find("Remove this recipe from your list?") == 0 && Widget("Ask").find(first.substr(0, 30)) != std::string::npos
			&& Widget("Ask").find("\"Restore deleted defaults\" can bring it back") != std::string::npos && Widget("DoConfirmYes") == "[ Remove it ]" && Widget("DoConfirmNo") == "[ Cancel ]";
		click("ConfirmNo");
		const bool kept = Widget("Title") == "Recipe" && Widget("Value0").substr(0, 30) == first.substr(0, 30);
		click("Back");
		const bool still = Widget("DoSave") == "[ Save (1) ]" && Widget("Page") == "1 - 8 of 227";
		click("Row0");
		click("Remove");
		click("ConfirmYes");
		Check(asksRemove && kept && still && Widget("DoSave") == "[ Save (2) ]" && Widget("Title0").rfind("* " + second.substr(0, 30), 0) == 0 && Widget("Page") == "1 - 8 of 226",
			"Remove asks first (naming the recipe); Cancel keeps it; Yes takes it out of the list");
		click("Search");
		const bool nothingNew = Widget("Title") == "Search for New Recipes" && Widget("Ask").find("Every recipe of the mod and the game is in your list already (or was deleted by you)") == 0 && !has("DoConfirmYes") && Widget("DoConfirmNo") == "[ Back ]";
		click("ConfirmNo");
		Check(nothingNew && Widget("Page") == "1 - 8 of 226", "Search for new recipes asks first; the removed recipe is noted as deleted, so it is not found as new");
		click("RestoreAll");
		const bool asks = Widget("Title") == "Restore Deleted Defaults" && Widget("Ask").find("1 recipe of the mod and the game was deleted by you.") == 0 && Widget("DoConfirmYes") == "[ Put 1 recipe back ]" && Widget("DoConfirmNo") == "[ Cancel ]"
			&& Widget("First0").substr(0, 30) == first.substr(0, 30);
		click("ConfirmNo");
		const bool cancelled = Widget("Page") == "1 - 8 of 226" && Widget("DoSave") == "[ Save (2) ]";
		click("RestoreAll");
		click("ConfirmYes");
		page(VK_END);
		Check(asks && cancelled && Widget("DoSave") == "[ Save (3) ]" && Widget("Page") == "220 - 227 of 227" && Widget("Title7") == first,
			"Restore deleted defaults asks first (naming the recipe), Cancel leaves the list, Yes puts it back at the end");
		page(VK_HOME);
		click("New");
		Check(Widget("Title") == "New Recipe" && Widget("Name0") == "description" && Widget("Value0") == "_" && Widget("Name3") == "input 1" && has("DoDone") && has("DoCancel"),
			"New recipe opens the cell editor on the description");
		click("Field3");
		typeText("hax,qty=2");
		click("Field6");
		typeText("r11");
		click("Name");
		const bool named = Widget("Value0") == "r11 <- 2 Hand Axe" && has("DoName");
		click("Cancel");
		click("New");
		Check(named && Widget("Value0") == "_", "\"Name it from its cells\" writes a description: what comes out, then what goes in, with the mod's names (r11 <- 2 Hand Axe)");
		typeText("My Recipe");
		click("Field3");
		typeText("hax,qty=2");
		click("Field6");
		typeText("zzz");
		const bool typedAll = Widget("Value0") == "My Recipe" && Widget("Value3") == "hax,qty=2" && Widget("Name6") == "output" && Widget("Value6") == "zzz_" && Widget("Page") == "cells 1 - 10 of 10";
		click("Done");
		Check(typedAll && Widget("Title") == "New Recipe" && Widget("Problem").find("The game cannot read this: output: the game does not know \"zzz\"") == 0,
			"typing goes to the cell clicked (capitals too); Done with a cell the game cannot read stays in the editor and says why");
		page(VK_DELETE);
		typeText("r11");
		click("Done");
		Check(Widget("DoSave") == "[ Save (4) ]" && Widget("Title0") == "* My Recipe" && Widget("Detail0") == "r11  <-  hax,qty=2" && rotw->rows == before && Lines(ReadText(mineFile)).size() == 1,
			"Done: the new recipe is first in the list; still nothing in recipes.txt or the cube");
		click("Row0");
		const bool mineCard = Widget("What") == "Changed or made by you." && Widget("Name2") == "numinputs" && Widget("Value2") == "2";
		click("Edit");
		click("Field6");
		page(VK_BACK);
		typeText("2");
		click("Done");
		Check(mineCard && Widget("DoSave") == "[ Save (5) ]" && Widget("Detail0") == "r12  <-  hax,qty=2", "the card of a new recipe says it is yours (numinputs counted for you); Edit changes it in place");
		g_action(&g_context, &press, g_actionData); // Ctrl+B with changes not saved
		Pump(150);
		Check(OpenBook() != nullptr && Widget("Title") == "Unsaved Changes" && Widget("Ask").find("You have 5 changes") == 0 && has("DoPromptSave") && has("DoPromptDiscard") && has("DoPromptBack"),
			"closing the book with unsaved changes asks: Save, Discard, Keep editing");
		click("PromptBack");
		const bool back = Widget("DoSave") == "[ Save (5) ]";
		page(VK_ESCAPE);
		Check(back && Widget("Title") == "Unsaved Changes", "Keep editing goes back; Escape asks too");
		size_t consoleAt = g_console.size();
		click("PromptSave");
		Pump(3000, [&] { return rotw->rows != before && Consoled("recipes.txt:", consoleAt); });
		std::string saved = ReadText(mineFile);
		Check(OpenBook() == nullptr && Lines(saved).size() == 229 && saved.find("*complete") != std::string::npos && saved.find("My Recipe\t1\t") != std::string::npos && rotw->rows != before && rotw->count == 231
				&& rotw->rows[0] == 1 && rotw->rows[kWide] == 0 && rotw->rows[2 * kWide] == 1 && Lines(ReadText(notesFile)).size() == 1
				&& Consoled("recipes.txt: the cube has the 228 recipes of this file (71 switched off) and 3 recipes of the game that no file has text for. The cube now knows 231 recipes.", consoleAt),
			"Save writes recipes.txt as the whole list (228 recipes), the cube takes it at once, and the book closes as asked");
		Pump(1600);
		g_bookCommand(nullptr, &open, g_bookData);
		Pump(150);
		Check(!has("DoSave") && Widget("Title0") == "* My Recipe" && Widget("Detail1").find("(not enabled)") != std::string::npos && Widget("Page") == "1 - 8 of 228", "opened again: the saved list, nothing unsaved");
		click("Row1");
		const bool offCard = Widget("What") == "Changed or made by you. It is switched off." && Widget("DoToggle") == "[ Enable ]";
		click("Toggle");
		click("Row0");
		click("Remove");
		click("ConfirmYes");
		Check(offCard && Widget("DoSave") == "[ Save (2) ]" && Widget("Title0") == second && Widget("Page") == "1 - 8 of 227", "Enable and the removal of your own recipe are two changes; the enabled recipe is as the mod has it again (no mark)");
		click("Discard");
		page(VK_HOME);
		Check(!has("DoSave") && Widget("Title0") == "* My Recipe" && Widget("Page") == "1 - 8 of 228", "Discard drops them: the list is the saved one again");
		// a default removed and saved: the cube loses it, the note keeps a search from bringing it back
		click("Row2");
		const std::string third = Widget("Value0");
		click("Remove");
		click("ConfirmYes");
		consoleAt = g_console.size();
		click("Save");
		Pump(3000, [&] { return Consoled("recipes.txt:", consoleAt); });
		saved = ReadText(mineFile);
		const bool gone = Lines(saved).size() == 228 && rotw->count == 230 && Lines(ReadText(notesFile)).size() == 2 && saved.find(third.substr(0, 30)) == std::string::npos;
		click("Search");
		const bool notNew = Widget("Ask").find("Every recipe of the mod and the game is in your list already") == 0;
		click("ConfirmNo");
		Check(gone && notNew && !has("DoSave"), "a removed default is out of recipes.txt and the cube, and noted in deleted.txt; Search does not bring it back");
		// at the next start (the tables are read again) the saved list is the cube's list, not the game's own
		g_action(&g_context, &press, g_actionData);
		Pump(150);
		LoadTables(3, g_rotw, g_stock[0], kOthers);
		LoadTables(2, g_base, g_stock[1], 0);
		event.revision = 4;
		const uint8_t* const fresh = rotw->rows;
		g_tablesListener(&g_context, &event, nullptr);
		Check(rotw->rows != fresh && rotw->count == 230 && rotw->rows[0] == 1 && rotw->rows[kWide] == 0 && Logged("recipes.txt: the cube has the 227 recipes of this file (71 switched off)"),
			"with recipes in recipes.txt the game's own recipes are not loaded: at a start the cube has the file's list");
		// without the note (as if the mod had a recipe the list never had) Search finds it
		WriteText(notesFile, "");
		LoadTables(3, g_rotw, g_stock[0], kOthers);
		LoadTables(2, g_base, g_stock[1], 0);
		event.revision = 5;
		g_tablesListener(&g_context, &event, nullptr);
		Pump(1600);
		g_bookCommand(nullptr, &open, g_bookData);
		Pump(150);
		click("Search");
		const bool found = Widget("Ask").find("1 recipe of the mod and the game is not in your list.") == 0 && Widget("DoConfirmYes") == "[ Add 1 recipe ]" && Widget("First0").substr(0, 30) == third.substr(0, 30);
		click("ConfirmYes");
		const bool added = Widget("DoSave") == "[ Save (1) ]" && Widget("Page") == "1 - 8 of 228";
		consoleAt = g_console.size();
		click("Save");
		Pump(3000, [&] { return Consoled("recipes.txt:", consoleAt); });
		Check(found && added && Lines(ReadText(mineFile)).size() == 229 && rotw->count == 231 && g_breaches == 0, "Search for new recipes finds a recipe of the mod that is not in the list, asks, and adds it at the end");
		// ---- keys, the clipboard, fewer cells, codes from the legend
		{
			const auto bookSide = reinterpret_cast<void (*)(int)>(reinterpret_cast<void*>(GetProcAddress(modBook, "ModBookSide")));
			const auto EditorHas = [](bool taken) { return !taken; }; // the key is the book's either way
			const auto combo = [&](unsigned key, int mods) { // 1 Shift, 2 Ctrl
				const bool taken = bookKey(key, mods);
				Pump(150);
				return taken;
			};
			const auto legendClick = [&](const std::string& what) {
				const std::string name = "mod-book/ClickRight" + what;
				D2RL::SharedEvents::UiMessageEvent ui {};
				ui.structSize = D2RL::SharedEvents::UiMessageEventSize;
				ui.target     = "PanelManager";
				ui.command    = "ClosePanel";
				ui.text       = name.c_str();
				g_uiListener(&g_context, &ui, nullptr);
				Pump(200);
			};
			const auto setClip = [](const wchar_t* text) {
				if (!OpenClipboard(nullptr)) {
					return false;
				}
				EmptyClipboard();
				const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wcslen(text) + 1) * sizeof(wchar_t));
				memcpy(GlobalLock(memory), text, (wcslen(text) + 1) * sizeof(wchar_t));
				GlobalUnlock(memory);
				const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
				CloseClipboard();
				return ok;
			};
			const auto rowOf = [&](const std::string& start) { // the Legend's row ("code - meaning") that starts so
				for (int i = 0; i < 40; ++i) {
					if ((Widget(("Code" + std::to_string(i)).c_str()) + " - " + Widget(("Meaning" + std::to_string(i)).c_str())).rfind(start, 0) == 0) {
						return i;
					}
				}
				return -1;
			};
			const bool newKey = combo('N', 2) && Widget("Title") == "New Recipe" && Widget("Page") == "cells 1 - 10 of 10" && Widget("Name6") == "output" && Widget("Name7") == "mod 1" && Widget("DoCells") == "[ All cells ]";
			click("Cells");
			const bool all = Widget("DoCells") == "[ Fewer cells ]" && Widget("Page") == "cells 1 - 10 of 105" && Widget("Name9") != "<none>" && Widget("Name10") == "<none>" && Widget("ScrollMark") != "<none>";
			bookWheel(-120);
			Pump(150);
			const bool wheeled = Widget("Page") == "cells 3 - 12 of 105" && Widget("Name0") == "numinputs" && Widget("Name8") == "output";
			page(VK_NEXT);
			const bool paged = Widget("Page") == "cells 13 - 22 of 105";
			click("Cells");
			Check(newKey && all && wheeled && paged && Widget("Page") == "cells 1 - 10 of 10" && Widget("Value0") == "_",
				"Ctrl+N starts a new recipe; the editor shows the usual cells, \"All cells\" every one, and the wheel and Page Down scroll them (" + Widget("Page") + ")");
			page(VK_DOWN);
			page(VK_DOWN);
			page(VK_DOWN);
			const bool down = Widget("Value3") == "_" && Widget("Value0") == "-";
			combo(VK_TAB, 1);
			const bool backTab = Widget("Value2") == "_";
			page(VK_TAB);
			const bool clip = setClip(L"  hax,qty=2\tnext cell\r\nnext line");
			const bool took = combo('V', 2);
			const bool pasted = Widget("Value3") == "hax,qty=2_";
			setClip(L"something else");
			combo('C', 2);
			page(VK_DOWN);
			combo('V', 2);
			Check(down && backTab && clip && took && pasted && Widget("Value4") == "hax,qty=2_",
				"the arrows, Tab and Shift+Tab go from cell to cell; Ctrl+V pastes the clipboard's first cell, Ctrl+C copies the cell (" + Widget("Value3") + " | " + Widget("Value4") + ")");
			page(VK_DELETE);
			// the Legend beside it: an entry's card has Insert, which puts its text into the cell
			g_legendCommand(nullptr, &open, g_legendData);
			Pump(200);
			bookSide(1);
			g_view = "Right";
			legendClick("Sec0");
			typeText("shld");
			const int shield = rowOf("shld   (Recipe codes) - ");
			legendClick("Row" + std::to_string(shield));
			const bool card = Widget("DoInsert") == "[ Insert shld ]" && Widget("CardCode").rfind("shld", 0) == 0 && Widget("CardText").find("Any Shield") != std::string::npos;
			legendClick("Insert");
			const bool note = Widget("Page") == "'shld' put into the cell";
			legendClick("Back");
			legendClick("Sec0");
			typeText("rar");
			const int rare = rowOf("rar   (Recipe codes) - ");
			legendClick("Row" + std::to_string(rare));
			legendClick("Insert");
			legendClick("Back");
			g_view = "Left";
			const bool codes = shield >= 0 && rare >= 0 && card && note && Widget("Value4") == "shld,rar_";
			g_view = "Right";
			legendClick("Sec1");
			legendClick("Row0"); // How it works: nothing to put in
			const bool noInsert = Widget("CardCode") == "The columns of a recipe   (Recipe columns, how it works)" && Widget("DoInsert") == "<none>" && Widget("DoBack") != "<none>";
			legendClick("Back");
			typeText("op 18");
			const int op = rowOf("op 18   (Recipe columns) - ");
			legendClick("Row" + std::to_string(op));
			legendClick("Insert");
			legendClick("Back");
			g_view = "Left";
			Check(codes && noInsert && op >= 0 && Widget("Value4") == "shld,rar,18_",
				"an entry's Insert puts its text into the cell typed into (with a comma in inputs); op 18 puts 18; How it works has no Insert (" + Widget("Value4") + ")");
			g_view = "Right";
			legendClick("Sec0");
			g_legendCommand(nullptr, &open, g_legendData); // again: closed
			Pump(200);
			Check(OpenBook() == nullptr, "recipelegend again closes the Legend");
			g_view = "Left";
			bookSide(-1);
			page(VK_DELETE);
			click("Field6");
			typeText("r11");
			page(VK_RETURN);
			const bool entered = Widget("DoSave") == "[ Save (1) ]" && Widget("Title0") == "* r11 <- 2 Hand Axe" && Widget("Page") == "1 - 8 of 229";
			// undo and redo, as far back as the last save
			const bool undoThere = Widget("DoUndo") == "[ Undo ]";
			combo('Z', 2);
			const bool undone = !has("DoSave") && !has("DoUndo") && Widget("Page") == "1 - 8 of 228" && Widget("Title0") != "* r11 <- 2 Hand Axe";
			const bool noMore = !EditorHas(combo('Z', 2)) && Widget("Page") == "1 - 8 of 228";
			combo('Y', 2);
			const bool redone = Widget("DoSave") == "[ Save (1) ]" && Widget("Title0") == "* r11 <- 2 Hand Axe" && Widget("Page") == "1 - 8 of 229";
			click("Row1"); // a default: removed (noted as deleted), then switched off another, then both taken back
			click("Remove");
			click("ConfirmYes");
			click("Row1");
			click("Toggle");
			const bool three = Widget("DoSave") == "[ Save (3) ]" && Widget("Page") == "1 - 8 of 228";
			click("Undo");
			click("Undo");
			click("RestoreAll");
			const bool noteGone = Widget("Ask").find("No recipe of the mod or the game is noted as deleted by you.") == 0;
			click("ConfirmNo");
			Check(undoThere && undone && noMore && redone && three && noteGone && Widget("DoSave") == "[ Save (1) ]" && Widget("Page") == "1 - 8 of 229" && Lines(ReadText(mineFile)).size() == 229,
				"Ctrl+Z and the Undo button take changes back one by one (the note of a removed default too), Ctrl+Y makes one again; nothing reaches the file (" + Widget("DoSave") + ", " + Widget("Page") + ")");
			consoleAt = g_console.size();
			combo('S', 2);
			Pump(3000, [&] { return Consoled("recipes.txt:", consoleAt); });
			const bool cleared = !has("DoUndo") && (combo('Z', 2), Widget("Page") == "1 - 8 of 229");
			Check(cleared, "after Save there is nothing to take back");
			Check(entered && !has("DoSave") && Lines(ReadText(mineFile)).size() == 230 && ReadText(mineFile).find("r11 <- 2 Hand Axe\t1\t") != std::string::npos,
				"Enter is Done (a recipe without a description is named for you); Ctrl+S saves (" + Widget("Title0") + ")");
			click("Row0");
			click("Remove");
			const bool asked = Widget("Title") == "Remove a Recipe";
			page(VK_RETURN);
			consoleAt = g_console.size();
			combo('S', 2);
			Pump(3000, [&] { return Consoled("recipes.txt:", consoleAt); });
			Check(asked && !has("DoSave") && Widget("Page") == "1 - 8 of 228" && Lines(ReadText(mineFile)).size() == 229 && g_breaches == 0, "Enter presses the confirming button");
		}
		// ---- templates: the list kept under a name, loaded back, deleted
		const std::string templateFile = own + "\\templates\\Rune Set-1.txt";
		click("Templates");
		const bool none = Widget("Title") == "Recipe Templates" && Widget("None") == "No template saved yet." && has("DoSaveAs") && has("DoBack");
		click("SaveAs");
		const bool asksName = Widget("Title") == "New Template" && Widget("NameTyped") == "_";
		click("NameDone");
		const bool needsName = Widget("Problem") == "The template needs a name." && Widget("Title") == "New Template";
		typeText("Rune Set-1");
		const bool typedName = Widget("NameTyped") == "Rune Set-1_";
		click("NameDone");
		Check(none && asksName && needsName && typedName && Widget("Title") == "Recipe Templates" && Widget("Name0") == "Rune Set-1   (228 recipes)" && Lines(ReadText(templateFile)).size() == 229 && has("DoLoad0") && has("DoDelete0"),
			"Templates: the list in the book is saved under a typed name (a name is asked for), and listed with its number of recipes");
		click("Back");
		click("Row0");
		click("Remove");
		click("ConfirmYes");
		const bool changed = Widget("DoSave") == "[ Save (1) ]" && Widget("Page") == "1 - 8 of 227";
		click("Templates");
		click("Load0");
		const bool asksLoad = Widget("Title") == "Load a Template" && Widget("Ask").find("Load the template \"Rune Set-1\" (228 recipes)?") == 0 && Widget("Ask").find("changes not saved included") != std::string::npos
			&& Widget("DoConfirmYes") == "[ Load \"Rune Set-1\" ]";
		click("ConfirmNo");
		const bool stays = Widget("Title") == "Recipe Templates";
		click("Load0");
		click("ConfirmYes");
		Check(changed && asksLoad && stays && Widget("DoSave") == "[ Save (2) ]" && Widget("Page") == "1 - 8 of 228" && rotw->count == 231,
			"loading a template asks first, then puts its list into the book in place of the one there; the cube has it only after Save");
		click("Templates");
		click("Delete0");
		const bool asksDelete = Widget("Title") == "Delete a Template" && Widget("Ask").find("Delete the template \"Rune Set-1\"?") == 0 && Widget("DoConfirmYes") == "[ Delete \"Rune Set-1\" ]";
		click("ConfirmYes");
		Check(asksDelete && Widget("Title") == "Recipe Templates" && Widget("None") == "No template saved yet." && ReadText(templateFile).empty(), "deleting a template asks first, then removes its file");
		click("Back");
		click("Discard");
		page(VK_ESCAPE);
		Check(OpenBook() == nullptr && !bookKey('A', 0), "Escape with nothing unsaved closes the book");
		// a property made this session (Live Properties knows it, the game's name table does not yet)
		{
			const std::vector<std::string> header = Split(mine[0], '\t');
			const auto line = [&](const char* description, const char* property, bool amount = true) {
				std::vector<std::string> cells(header.size());
				const auto set = [&](const char* name, const char* value) {
					const int at = Column(header, name);
					if (at >= 0) {
						cells[static_cast<size_t>(at)] = value;
					}
				};
				set("description", description);
				set("enabled", "1");
				set("version", "100");
				set("numinputs", "1");
				set("input 1", "hax");
				set("output", "useitem");
				set("mod 1", property);
				if (amount) {
					set("mod 1 min", "1");
					set("mod 1 max", "1");
				}
				std::string text;
				for (size_t i = 0; i < cells.size(); ++i) {
					text += (i == 0 ? "" : "\t") + cells[i];
				}
				return text;
			};
			const size_t from = g_console.size();
			WriteText(mineFile, Join(std::vector<std::string> { mine[0], line("made", "my-prop"), line("unknown", "nope-prop") }));
			TypeRecipes();
			Pump(4000, [&] { return Consoled("is not a property code", from); });
			Pump(300);
			bool placed = false;
			for (size_t r = 0; r < rotw->count && rotw->rows != nullptr; ++r) {
				uint32_t number = 0;
				memcpy(&number, rotw->rows + r * kWide + 0x5a + 0x1d, 4);
				placed = placed || number == 903;
			}
			Check(liveProps != nullptr && placed && Consoled("\"nope-prop\" is not a property code", from) && !Consoled("\"my-prop\" is not a property code", from),
				"a property code the game does not know yet is asked of Live Properties: a made one takes its row (903 in Reign of the Warlock), an unknown one is still refused");

			// Live Tables: a property made AFTER the recipe was written, and a row taken out (the
			// user's vit-str, 2026-11-25); a mod without an amount is taken, with a note
			const auto fakeMake   = liveProps != nullptr ? reinterpret_cast<void(__cdecl*)(const char*, int)>(reinterpret_cast<void*>(GetProcAddress(liveProps, "FakeMakeProperty"))) : nullptr;
			const auto fakeChange = liveTables != nullptr ? reinterpret_cast<int(__cdecl*)(int, uint64_t)>(reinterpret_cast<void*>(GetProcAddress(liveTables, "FakeTableChange"))) : nullptr;
			const auto hasNumber  = [&](uint32_t wanted) {
				for (size_t r = 0; r < rotw->count && rotw->rows != nullptr; ++r) {
					uint32_t number = 0;
					memcpy(&number, rotw->rows + r * kWide + 0x5a + 0x1d, 4);
					if (number == wanted) {
						return true;
					}
				}
				return false;
			};
			Check(fakeMake != nullptr && fakeChange != nullptr && Logged("Live Tables: found (API 3); a property or stat row made or taken out during play is read into the recipes at once")
					&& Logged("Live Tables: a stat or property row your recipe files name cannot be removed (remove veto on)"),
				"Live Tables (API 3) is found at the tables-read notice; its change listener and remove veto are taken");
			if (fakeMake != nullptr && fakeChange != nullptr) {
				const size_t late = g_console.size();
				WriteText(mineFile, Join(std::vector<std::string> { mine[0], line("late", "late-prop"), line("Small Charm <- smca", "my-prop", false) }));
				TypeRecipes();
				Pump(4000, [&] { return Consoled("\"late-prop\" is not a property code", late); });
				Pump(300);
				const bool refusedFirst = !hasNumber(950) && hasNumber(903) && Consoled("\"late-prop\" is not a property code", late);
				Check(Consoled("Small Charm <- smca", late) && Consoled("mod 1 my-prop has no amount: its min, max and param are blank (0), so the item gets nothing. Put the amount in mod 1 min and mod 1 max", late),
					"a mod with a property code and blank min, max and param is taken, and the console says it gives nothing and where the amount goes");
				fakeMake("late-prop", 950);
				const size_t grown = g_console.size();
				const bool   heard = fakeChange(0, 949) == 1; // Grow, as Live Tables' job calls it: nothing typed after it
				Pump(4000, [&] { return hasNumber(950); });
				Check(refusedFirst && heard && hasNumber(950) && hasNumber(903) && Logged("Live Tables: a row was made; the recipes are compiled again") && !Consoled("\"late-prop\" is not a property code", grown),
					"a recipe line refused for a property that did not exist yet is read at once when Live Tables makes the row (no recipes typed, no kept refusal)");
				fakeMake("late-prop", 949);
				fakeChange(5, 900); // Remove of a row below it: late-prop is one lower now
				Pump(4000, [&] { return hasNumber(949); });
				Check(hasNumber(949) && !hasNumber(950) && Logged("Live Tables: a row was taken out (numbers moved); the recipes are compiled again"),
					"when Live Tables takes a row out, the cube's records are compiled again with the new numbers (the kept compiled lines are let go)");
				const size_t quiet = g_log.size();
				WriteText(mineFile, Join(std::vector<std::string> { mine[0], line("made", "my-prop") }));
				TypeRecipes();
				Pump(4000, [&] { return !hasNumber(949); });
				fakeChange(0, 950);
				Pump(500);
				Check(Logged("Live Tables: a row was made; no recipe line waited for it", quiet), "a row made while no line was refused leaves the cube's list as it is");
				// the remove veto: recipes.txt names my-prop (903) in mod 1 of line 2
				const auto fakeAsk = reinterpret_cast<int(__cdecl*)(int, int, int, char*, size_t)>(reinterpret_cast<void*>(GetProcAddress(liveTables, "FakeAskRemove")));
				char       whyNamed[400] = "", whyAfter[400] = "", whyFree[400] = "", whyStat[400] = "";
				const int  named = fakeAsk != nullptr ? fakeAsk(1, 3, 903, whyNamed, sizeof(whyNamed)) : -1;
				const int  after = fakeAsk != nullptr ? fakeAsk(1, 3, 800, whyAfter, sizeof(whyAfter)) : -1; // a middle row: 903 would move
				const int  free_ = fakeAsk != nullptr ? fakeAsk(1, 3, 904, whyFree, sizeof(whyFree)) : -1;
				const int  stat  = fakeAsk != nullptr ? fakeAsk(0, 3, 0, whyStat, sizeof(whyStat)) : -1;  // the mod's Horadric Staff line has op 28 (no stat): nothing names a stat
				Check(named == 0 && std::string(whyNamed).find("Live Cube Recipes: recipes.txt: line 2 (made) names property 903 or a row after it in mod 1; change or remove that line first") == 0
						&& after == 0 && free_ == 1 && stat == 1 && Logged("Live Tables asked to remove property row 903 (data version 3): refused"),
					"Live Tables' remove veto: a property a recipes.txt line names (or a row before it, which would move it) is refused, naming the line and cell; a row no line names may go (" + std::string(whyNamed) + " | " + whyStat + ")");
				// a rename (Live Tables API 3): recipes.txt follows the code after a dated copy, and so do the
				// book's unsaved rows; the cube's record keeps the row's number
				{
					const auto fakeRename = reinterpret_cast<int(__cdecl*)(int, uint64_t, const char*, const char*)>(reinterpret_cast<void*>(GetProcAddress(liveTables, "FakeRename")));
					CreateDirectoryA((own + "\\templates").c_str(), nullptr);
					const std::string templateRename = own + "\\templates\\Renamed One.txt";
					WriteText(templateRename, ReadText(mineFile));
					WriteText(notesFile, ReadText(mineFile));
					const std::string fileBefore = ReadText(mineFile);
					std::string       wanted     = fileBefore;
					const size_t      at         = wanted.find("\tmy-prop\t");
					wanted.replace(at, 9, "\tmy-prop2\t");
					g_bookCommand(nullptr, &open, g_bookData);
					Pump(150);
					click("Row0");
					click("Toggle"); // an unsaved change: the book's rows are not the file's
					const bool unsaved = Widget("DoSave") == "[ Save (1) ]";
					const size_t renamedLog = g_log.size();
					fakeMake("my-prop2", 903);
					const bool called = fakeRename != nullptr && fakeRename(1, 903, "my-prop", "my-prop2") == 1;
					Pump(4000, [&] { return ReadText(mineFile).find("my-prop2") != std::string::npos && Logged("the recipe book: property my-prop is now my-prop2", renamedLog); });
					std::string      copyName, copyText;
					WIN32_FIND_DATAA entry {};
					const HANDLE     find = FindFirstFileA((own + "\\recipes.before-rename-*.txt").c_str(), &entry);
					if (find != INVALID_HANDLE_VALUE) {
						copyName = entry.cFileName;
						copyText = ReadText(own + "\\" + copyName);
						FindClose(find);
					}
					Check(called && at != std::string::npos && ReadText(mineFile) == wanted && copyText == fileBefore && copyName.size() == std::string("recipes.before-rename-20261125-220000.txt").size()
							&& Logged(("recipes.txt: property my-prop is now my-prop2 - 1 cell rewritten (line 2 (made) mod 1); the file before: " + copyName).c_str(), renamedLog) && hasNumber(903),
						"a property renamed by Live Tables: a dated copy of recipes.txt first, then the code rewritten in the cell that named it and nothing else; the cube keeps the row (" + copyName + ")");
					std::string tcopyName;
					const HANDLE tfind = FindFirstFileA((own + "\\templates\\before-rename\\Renamed One.before-rename-*.txt").c_str(), &entry);
					if (tfind != INVALID_HANDLE_VALUE) {
						tcopyName = entry.cFileName;
						FindClose(tfind);
					}
					std::string dcopyName;
					const HANDLE dfind = FindFirstFileA((own + "\\deleted.before-rename-*.txt").c_str(), &entry);
					if (dfind != INVALID_HANDLE_VALUE) {
						dcopyName = entry.cFileName;
						FindClose(dfind);
					}
					Check(ReadText(templateRename) == wanted && ReadText(notesFile) == wanted && !tcopyName.empty() && ReadText(own + "\\templates\\before-rename\\" + tcopyName) == fileBefore
							&& !dcopyName.empty() && ReadText(own + "\\" + dcopyName) == fileBefore
							&& Logged("templates\\Renamed One.txt: property my-prop is now my-prop2 - 1 cell rewritten", renamedLog) && Logged("deleted.txt: property my-prop is now my-prop2 - 1 cell rewritten", renamedLog),
						"a rename rewrites deleted.txt and every template the same way (dated copies: deleted.before-rename-..., templates\\before-rename\\, out of the template list)");
					click("Row0");
					bool shown = false;
					for (int n = 0; n < 40 && !shown; ++n) {
						shown = Widget(("Name" + std::to_string(n)).c_str()) == "mod 1" && Widget(("Value" + std::to_string(n)).c_str()) == "my-prop2";
					}
					click("Back");
					Check(unsaved && shown && Widget("DoSave") == "[ Save (1) ]" && Logged("the recipe book: property my-prop is now my-prop2 - 1 cell of its rows rewritten (not saved yet)", renamedLog),
						"the recipe book's unsaved rows follow the rename too (the change stays unsaved)");
					click("Discard");
					page(VK_ESCAPE);
					DeleteFileA((own + "\\" + copyName).c_str());
					DeleteFileA((own + "\\" + dcopyName).c_str());
					DeleteFileA((own + "\\templates\\before-rename\\" + tcopyName).c_str());
					RemoveDirectoryA((own + "\\templates\\before-rename").c_str());
					DeleteFileA(templateRename.c_str());
					WriteText(notesFile, "");
				}
			}
		}
		// back to an empty file for what follows
		WriteText(mineFile, Join(std::vector<std::string> { mine[0] }));
		WriteText(notesFile, "");
		LoadTables(3, g_rotw, g_stock[0], kOthers);
		LoadTables(2, g_base, g_stock[1], 0);
		event.revision = 6;
		g_tablesListener(&g_context, &event, nullptr);
		Check(ListIs(rotw, wantStock, 3, g_stock[0], kOthers, why), "an emptied recipes.txt: the cube has the game's own recipes again" + (why.empty() ? "" : " (" + why + ")"));
	}

	// ---- someone else replaces the list: the plugin stops instead of writing over it
	uint8_t* foreign = static_cast<uint8_t*>(calloc(10, kWide));
	rotw->rows       = foreign;
	rotw->count      = 10;
	rotw->capacity   = 10;
	consoleFrom      = g_console.size();
	WriteText(mineFile, Join(mine));
	TypeRecipes();
	Pump(3000, [&] { return Consoled("something else replaced", consoleFrom); });
	Check(rotw->rows == foreign && rotw->count == 10 && Consoled("something else replaced", consoleFrom), "a list the plugin does not know is not touched");

	Check(modState() == modBefore, "nothing in the mod's folder was added, removed or changed");
	Check(!Logged("is too long for"), "every action button's label fits its slots (at most 12 letters in one slot, 28 in two; the plugin logs any that does not)");
	static_cast<void>(logFrom);

	unload();
	printf(g_failures == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
