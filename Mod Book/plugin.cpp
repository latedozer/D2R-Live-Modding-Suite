// Mod Book: two side panels of the game's own kind (left: the book, right: the legend) that
// other plugins put tabs into (mod_book_api.h). It has no content of its own.
//
// Taken out of Live Cube Recipes 0.13.1, whose recipe book and legend these panels were.
// D2RLoader 1.3.1's widget service cannot change a label or wire a button, its panels know no
// text entry and it has no key listener. So, as RuffnecKk's MapSense does on this loader:
//  * a page's text is written into the layout itself. A side is a pool of panels; to show
//    another page the next panel of the pool gets a new layout and is opened, then the one
//    before is closed. A closed panel is unloaded ("PanelManager:UnloadPanel") before it is
//    used again, and not sooner than 1.5 s after it closed;
//  * a button's click is a "PanelManager:ClosePanel" message for a panel name that does not
//    exist, which the UI message listener takes as the click;
//  * hover text is the tooltip of an unseen button;
//  * while a side is open a low-level keyboard and mouse hook takes the keys that are text
//    (so the game's hotkeys do not fire), Backspace, Page Up/Down, Home/End and the wheel.
//    With both sides open they go to the one the cursor is over.
//
// Keys: Ctrl+B the left side, Ctrl+L the right side. Console: modbook [right].

#include <D2RLPlugin/api.h>

#include "mod_book_api.h"

#include <windows.h>

#include <CascLib.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kVersion = "0.5.1";

const D2RL::PluginContext* g_ctx = nullptr;

void LogF(const char* format, ...) {
	if (g_ctx == nullptr) {
		return;
	}
	char    text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

uint64_t NowMs() {
	return GetTickCount64();
}

// ---------------------------------------------------------------- files

std::wstring Widen(const std::string& text) {
	std::wstring out(text.size(), L'\0');
	const int    length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
	out.resize(static_cast<size_t>(length > 0 ? length : 0));
	return out;
}

std::wstring GameDirectory() {
	wchar_t      path[1024] = {};
	const DWORD  length     = GetModuleFileNameW(nullptr, path, 1000);
	std::wstring out(path, length < 1000 ? length : 0);
	const size_t slash = out.find_last_of(L'\\');
	return slash != std::wstring::npos ? out.substr(0, slash) : out;
}

bool IsDirectory(const std::wstring& path) {
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The folder the game reads the active mod's loose files from: mods/<mod>/<mod>.mpq/data.
bool FindDataRoot(std::wstring& root) {
	const std::string modName = g_ctx->activeMod != nullptr ? g_ctx->activeMod : "";
	if (modName.empty()) {
		return false;
	}
	const std::wstring        mod = Widen(modName);
	std::vector<std::wstring> folders;
	if (g_ctx->modDirectory != nullptr && g_ctx->modDirectory[0] != L'\0') {
		std::wstring base = g_ctx->modDirectory;
		while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
			base.pop_back();
		}
		folders.push_back(base + L"\\" + mod + L".mpq");
		folders.push_back(base);
	}
	folders.push_back(GameDirectory() + L"\\mods\\" + mod + L"\\" + mod + L".mpq");
	for (const std::wstring& folder : folders) {
		if (IsDirectory(folder + L"\\data")) {
			root = folder + L"\\data";
			return true;
		}
	}
	return false;
}

bool ReadWhole(const std::wstring& path, std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	text.clear();
	char  buffer[65536];
	DWORD got = 0;
	while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got != 0) {
		text.append(buffer, got);
	}
	CloseHandle(file);
	return true;
}

// ---------------------------------------------------------------- pictures

// A button of the wanted size from the game's button picture (4 frames side by side): the ends
// are kept, the middle is repeated mirrored. (Live Cube Recipes' cube::ButtonSprite.)
std::string ButtonSprite(std::string_view source, int width, int height) {
	constexpr size_t kHeader = 40;
	const auto       u16     = [&](size_t at) { return static_cast<uint32_t>(static_cast<uint8_t>(source[at])) | static_cast<uint32_t>(static_cast<uint8_t>(source[at + 1])) << 8; };
	const auto       u32     = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
	if (source.size() <= kHeader || source.substr(0, 4) != "SpA1" || width < 8 || height < 4 || width > 4096 || height > 1024) {
		return {};
	}
	const uint32_t frameWidth = u16(6), allWidth = u32(8), srcHeight = u32(12), frames = u32(20);
	if (frames == 0 || frames > 16 || frameWidth < 8 || srcHeight < 4 || srcHeight > 4096 || allWidth < frames * frameWidth || allWidth > 65536
		|| source.size() < kHeader + static_cast<size_t>(allWidth) * srcHeight * 4) {
		return {};
	}
	const uint32_t gap    = frames > 1 ? (allWidth - frames * frameWidth) / (frames - 1) : 0;
	const double   scale  = static_cast<double>(srcHeight) / height;
	const int      scaled = static_cast<int>(frameWidth / scale + 0.5);
	int            cap    = static_cast<int>(srcHeight * 0.62 / scale + 0.5);
	cap                   = std::min(cap, std::min(width / 2, (scaled - 2) / 2));
	if (scaled < 4 || cap < 1) {
		return {};
	}
	const int   middle = scaled - 2 * cap;
	const auto* pixels = reinterpret_cast<const uint8_t*>(source.data()) + kHeader;
	const int   outAll = width * static_cast<int>(frames);
	std::string out(kHeader + static_cast<size_t>(outAll) * height * 4, '\0');
	const auto  put16 = [&](size_t at, uint32_t v) { out[at] = static_cast<char>(v); out[at + 1] = static_cast<char>(v >> 8); };
	const auto  put32 = [&](size_t at, uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
	out.replace(0, 4, "SpA1");
	put16(4, 31);
	put16(6, static_cast<uint32_t>(width));
	put32(8, static_cast<uint32_t>(outAll));
	put32(12, static_cast<uint32_t>(height));
	put32(20, frames);
	put32(32, static_cast<uint32_t>(out.size() - kHeader));
	put32(36, 4);
	for (uint32_t f = 0; f < frames; ++f) {
		const uint32_t frameAt = f * (frameWidth + gap);
		for (int x = 0; x < width; ++x) {
			int sx;
			if (x < cap) {
				sx = x;
			} else if (x >= width - cap) {
				sx = scaled - (width - x);
			} else {
				const int t = (x - cap) % (2 * middle);
				sx          = cap + (t < middle ? t : 2 * middle - 1 - t);
			}
			const uint32_t x0 = std::min(frameWidth - 1, static_cast<uint32_t>(sx * scale));
			const uint32_t x1 = std::min(frameWidth, std::max(x0 + 1, static_cast<uint32_t>((sx + 1) * scale + 0.5)));
			for (int y = 0; y < height; ++y) {
				const uint32_t y0 = std::min(srcHeight - 1, static_cast<uint32_t>(y * scale));
				const uint32_t y1 = std::min(srcHeight, std::max(y0 + 1, static_cast<uint32_t>((y + 1) * scale + 0.5)));
				uint64_t       sum[4] {};
				for (uint32_t v = y0; v < y1; ++v) {
					const uint8_t* p = pixels + (static_cast<size_t>(v) * allWidth + frameAt + x0) * 4;
					for (uint32_t u = x0; u < x1; ++u, p += 4) {
						sum[0] += static_cast<uint64_t>(p[0]) * p[3];
						sum[1] += static_cast<uint64_t>(p[1]) * p[3];
						sum[2] += static_cast<uint64_t>(p[2]) * p[3];
						sum[3] += p[3];
					}
				}
				const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
				char*          to    = out.data() + kHeader + (static_cast<size_t>(y) * outAll + f * static_cast<uint32_t>(width) + static_cast<uint32_t>(x)) * 4;
				if (sum[3] != 0) {
					to[0] = static_cast<char>(sum[0] / sum[3]);
					to[1] = static_cast<char>(sum[1] / sum[3]);
					to[2] = static_cast<char>(sum[2] / sum[3]);
					to[3] = static_cast<char>(sum[3] / count);
				}
			}
		}
	}
	return out;
}

constexpr int kButtonWidths[] = { 116, 188, 200, 216, 220, 232, 250, 300, 330, 470, 480, 482, 500, 512, 700, 742 };
bool          g_buttonArt     = false;
bool          g_barArt        = false;
constexpr int kTrackHeight = 840, kMarkHeight = 70;

bool HasButtonArt(int width) {
	return g_buttonArt && std::find(std::begin(kButtonWidths), std::end(kButtonWidths), width) != std::end(kButtonWidths);
}

const D2RL::ResourceService* g_resources = nullptr;
const D2RL::PanelService*    g_panels    = nullptr;
const D2RL::WidgetService*   g_widgets   = nullptr;
const D2RL::ThreadService*   g_threads   = nullptr;
std::string                  g_off       = "not set up"; // why there are no panels

bool RegisterSprite(const std::string& path, const std::string& bytes) {
	D2RL::Resources::ResourceRegistration sprite {};
	sprite.structSize = D2RL::Resources::ResourceRegistrationSize;
	sprite.path       = path.c_str();
	sprite.bytes      = reinterpret_cast<const uint8_t*>(bytes.data());
	sprite.byteCount  = bytes.size();
	D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
	return g_resources->registerResource(g_ctx, &sprite, &handle) == D2RL::Resources::Result::Success;
}

// The game's own button picture ("PANEL\\Modals\\ModalButton"): the mod's copy when it has one, else the game's.
bool ReadButtonSource(std::string& out, std::string& from) {
	std::wstring root;
	if (FindDataRoot(root) && ReadWhole(root + L"\\hd\\global\\ui\\panel\\modals\\modalbutton.sprite", out) && !out.empty()) {
		from = "the mod";
		return true;
	}
	HANDLE storage = nullptr;
	if (!CascOpenStorage(GameDirectory().c_str(), 0, &storage)) {
		from = "the game's data could not be opened (error " + std::to_string(GetCascError()) + ")";
		return false;
	}
	HANDLE file = nullptr;
	bool   ok   = CascOpenFile(storage, "data:data\\hd\\global\\ui\\panel\\modals\\modalbutton.sprite", 0, CASC_OPEN_BY_NAME, &file);
	if (ok) {
		ULONGLONG size = 0;
		ok             = CascGetFileSize64(file, &size) && size > 40 && size <= 64ULL * 1024 * 1024;
		if (ok) {
			out.resize(static_cast<size_t>(size));
			DWORD read = 0;
			ok         = CascReadFile(file, out.data(), static_cast<DWORD>(size), &read) && read == size;
		}
		CascCloseFile(file);
	}
	CascCloseStorage(storage);
	from = ok ? "the game" : "the game's data has no such picture";
	return ok;
}

void ArtSetUp() {
	static std::vector<std::string> kept; // the loader keeps pointers to these
	kept.reserve(std::size(kButtonWidths) * 2 + 8);
	// the scroll line and its mark: two plain pictures (half size for low resolution)
	struct Picture {
		const char* name;
		int         width, height;
		uint8_t     color[4];
	};
	const Picture pictures[] = { { "scrolltrack", 4, kTrackHeight, { 150, 120, 70, 120 } }, { "scrollmark", 12, kMarkHeight, { 222, 184, 110, 255 } } };
	g_barArt                 = true;
	for (const Picture& picture : pictures) {
		for (int low = 0; low < 2 && g_barArt; ++low) {
			const uint32_t width = static_cast<uint32_t>(picture.width / (low + 1)), height = static_cast<uint32_t>(picture.height / (low + 1));
			std::string    bytes(40 + static_cast<size_t>(width) * height * 4, '\0');
			const uint32_t head[10] = { 0x31417053u, 31u | width << 16, width, height, 0, 1, 0, 0, width * height * 4, 4 };
			memcpy(bytes.data(), head, 40);
			for (size_t at = 40; at < bytes.size(); at += 4) {
				memcpy(bytes.data() + at, picture.color, 4);
			}
			kept.push_back(std::move(bytes));
			g_barArt = RegisterSprite(std::string("data/hd/global/ui/d2rloader/mod-book/") + picture.name + (low != 0 ? ".lowend.sprite" : ".sprite"), kept.back());
		}
	}
	if (!g_barArt) {
		LogF("the scroll line's picture could not be registered: lists are shown without it");
	}
	const ULONGLONG started = GetTickCount64();
	std::string     source, from;
	if (!ReadButtonSource(source, from)) {
		LogF("buttons: plain words, the game's button picture could not be read (%s)", from.c_str());
		return;
	}
	for (const int width : kButtonWidths) {
		for (int low = 0; low < 2; ++low) {
			kept.push_back(ButtonSprite(source, low != 0 ? width / 2 : width, low != 0 ? 22 : 44));
			if (kept.back().empty() || !RegisterSprite("data/hd/global/ui/d2rloader/mod-book/button" + std::to_string(width) + (low != 0 ? ".lowend.sprite" : ".sprite"), kept.back())) {
				LogF("buttons: plain words, the picture for width %d could not be made or registered", width);
				return;
			}
		}
	}
	g_buttonArt = true;
	LogF("buttons: the game's button picture (from %s) in %zu widths, %llu ms", from.c_str(), std::size(kButtonWidths), static_cast<unsigned long long>(GetTickCount64() - started));
}

// ---------------------------------------------------------------- tabs and sides

constexpr int      kSlots    = 32;   // the loader allows a plugin 64 panels in all (two sides)
constexpr uint64_t kPace     = 50;   // ms between two drawings of a side: (kSlots - 1) * kPace >= kCooldown, so the pool never runs dry and a drag never stalls
constexpr uint64_t kCooldown = 1500; // ms a closed panel is left alone before it is unloaded
constexpr int      kTabsMost = 9;    // tabs a side has room for
constexpr char     kClick[]  = "mod-book/Click"; // + the side's id + what was clicked

// A text of a page that may change without a new panel: a label's, or a spot's hover text.
struct PageVar {
	std::string name;
	bool        tip;
	std::string text;
};

struct Slot {
	std::string                         name;   // "mod-book/Left07"
	std::string                         path;   // its layout resource
	D2RL::Panels::RegistrationHandle    panel  = D2RL::Panels::InvalidHandle;
	D2RL::Resources::RegistrationHandle layout = D2RL::Resources::InvalidHandle;
	bool                                loaded   = false; // D2R holds a panel made from its layout
	uint64_t                            closedAt = 0;     // when it was closed (0: open, or never used)
};

struct TabEntry {
	modbook::Tab tab {};
	std::string  id, title, group;
};

struct Side {
	const char* id; // "Left" / "Right": panel names, click names
	bool        right;

	// asked for from other threads (the hooks, the UI message listener, the tabs' plugins); keys and clicks under g_typeLock
	std::atomic<bool>        open { false };  // believed open (cleared when found closed)
	std::atomic<int>         move { 0 };      // pages to go
	std::atomic<int>         scroll { 0 };    // rows to go (the wheel)
	std::atomic<int>         jump { 0 };      // -1 first, 1 last
	std::atomic<bool>        dirty { false }; // the page must be drawn again
	std::atomic<int>         wantTab { -1 };  // a tab asked to be shown (index into tabs)
	std::atomic<int>         wantOpen { 0 };  // 1 open, 2 close, 3 toggle - asked for from another thread
	std::string              keys;
	std::vector<std::string> clicks;

	// UI thread only
	std::vector<int> tabs;         // handles - 1, in the order they were added
	int              current = 0;  // the tab shown (index into tabs)
	bool             told    = false; // the current tab was told it is on screen
	Slot             slots[kSlots];
	int              active = -1;  // the slot on screen
	int              cursor = 0;
	uint64_t         shownAt = 0;  // when the page on screen was drawn
	std::string            shape;       // the page on screen (Page::shape, vars, mark)
	std::vector<PageVar> vars;
	int                    mark = -1, marks = 0;
	uint64_t               marksPending = 0; // since when the spare marks wait to be hidden
	int              down   = 0;   // how far the page shown moved its tab's widgets down
	// the list's scroll bar on the page shown (UI thread), for dragging it
	bool     hasBar = false;
	int      barY   = 0; // the bar's top on the page
	uint64_t barTop = 0, barShown = 0, barCount = 0;
	// where the panel is in the game window, learnt from clicks on the bar (Calibrate)
	bool  calOk = false;
	float scale = 0, originXLo = 0, originXHi = 0, originYLo = 0, originYHi = 0;
	int   firstSeg = -1, firstBarY = 0;
	float firstY = 0;
	// the same for the mouse hook's thread, under g_typeLock; places in the window's own pixels
	bool     trackOk = false;
	float    trackX = 0, trackY = 0, trackW = 0, trackH = 0, trackRange = 0, markH = 0;
	uint64_t dragShown = 0, dragCount = 0, dragTop = 0;
	std::atomic<long long> dragTo { -1 }; // the row the bar was dragged to
};
Side    g_left { "Left", false }, g_right { "Right", true };
Side*   g_sides[] = { &g_left, &g_right };
SRWLOCK g_typeLock = SRWLOCK_INIT; // keys, clicks
SRWLOCK g_tabLock  = SRWLOCK_INIT; // g_tabs, Side::tabs

std::vector<TabEntry*> g_tabs; // never shrinks; entries are never freed

TabEntry* TabOf(int handle) {
	AcquireSRWLockShared(&g_tabLock);
	TabEntry* entry = handle >= 1 && static_cast<size_t>(handle) <= g_tabs.size() ? g_tabs[static_cast<size_t>(handle) - 1] : nullptr;
	ReleaseSRWLockShared(&g_tabLock);
	return entry;
}

std::vector<int> TabsOf(Side& s) {
	AcquireSRWLockShared(&g_tabLock);
	std::vector<int> tabs = s.tabs;
	ReleaseSRWLockShared(&g_tabLock);
	return tabs;
}

// ---------------------------------------------------------------- a page's layout

// Text inside a JSON string of a layout. Tabs give Latin-1; the layout is UTF-8.
std::string JsonText(const std::string& text) {
	std::string out;
	// a leading @ or $ would make D2R look the text up as a string key or a layout variable
	if (!text.empty() && (text[0] == '@' || text[0] == '$')) {
		out += ' ';
	}
	for (const char c : text) {
		const auto u = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (c == '\n') {
			out += "\\n";
		} else if (u < 0x20 || u >= 0x80) {
			char code[8];
			snprintf(code, sizeof(code), "\\u%04x", u < 0x20 ? 0x20u : u);
			out += code;
		} else {
			out += c;
		}
	}
	return out;
}

} // namespace

struct modbook::Page {
	std::string out;
	const Side* side;
	int         down;
	std::string title = {};
	bool        bar = false;
	int         barY = 0;
	uint64_t    barTop = 0, barShown = 0, barCount = 0; // how far a tab's widgets at y >= 250 are moved (the tab row)
	// What may change without a new panel (InPlace): the texts of labels and the hover texts of
	// spots. `shape` is `out` with each of those texts left out; two pages of one shape differ
	// in nothing else. \x02 in both stands for the panel's name.
	using Var = PageVar;
	std::string      shape = {};
	std::vector<Var> vars  = {};
	int              mark = -1, marks = 0; // the scroll bar's mark: which of how many is seen
	int         y(int at) const { return at >= 250 ? at + down : at; }
	std::string clickName(const std::string& click) const { return std::string(kClick) + side->id + click; }
};

namespace {

using modbook::Page;

bool g_inPlace = false; // a page's texts can be changed in the panel on screen (InPlaceSetUp)

void Put(Page& p, const std::string& fixed) {
	p.out   += fixed;
	p.shape += fixed;
}
void PutText(Page& p, const std::string& name, bool tip, const std::string& text) {
	p.out   += JsonText(text);
	p.shape += '\x01';
	p.vars.push_back({ name, tip, text });
}

void PageStart(Page& p, const std::string& title) {
	const std::string side = p.side->right ? "Right" : "Left";
	p.out = std::string("{\n\t\"type\": \"Panel\", \"name\": \"\x02\",\n\t\"fields\": { \"anchor\": \"$") + side + "PanelAnchor\", \"rect\": \"$" + side
	      + "PanelRect\", \"priority\": 8500 },\n\t\"children\": [\n\t\t{ \"type\": \"ImageWidget\", \"name\": \"Background\", \"fields\": { \"filename\": \"PANEL\\\\PANEL_BG\" } },\n"
	        "\t\t{ \"type\": \"ImageWidget\", \"name\": \"Hinge\", \"fields\": { \"rect\": \"$"
	      + side + "HingeRect\", \"filename\": \"$" + side + "HingeSprite\" } },\n"
	      + "\t\t{ \"type\": \"ClickCatcherWidget\", \"name\": \"ClickCatcher\", \"fields\": { \"rect\": \"$PanelClickCatcherRect\" } },\n";
	p.shape = p.out;
	p.title = title;
}

void PageDone(Page& p) {
	Put(p, "\t\t{ \"type\": \"TextBoxWidget\", \"name\": \"Title\", \"fields\": { \"rect\": \"$InGamePanelTitleRect\", \"style\": \"$StyleTitleBlock\", \"text\": \"");
	PutText(p, "Title", false, p.title);
	Put(p, "\" } }\n\t]\n}\n");
}

// The page's layout for the panel of this name.
std::string Layout(const Page& p, const std::string& panelName) {
	std::string  out = p.out;
	const size_t at  = out.find('\x02');
	if (at != std::string::npos) {
		out.replace(at, 1, panelName);
	}
	return out;
}

void RawLabel(Page& p, const std::string& name, int x, int y, int width, int height, const char* color, const char* align, bool wrap, const std::string& text) {
	char head[500];
	snprintf(head, sizeof(head),
		"\t\t{ \"type\": \"TextBoxWidget\", \"name\": \"%s\", \"fields\": { \"rect\": { \"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d },"
		" \"style\": { \"fontColor\": \"%s\", \"pointSize\": \"$SmallFontSize\", \"alignment\": { \"h\": \"%s\", \"v\": \"%s\" }%s }, \"text\": \"",
		name.c_str(), x, y, width, height, color, align, wrap ? "top" : "center", wrap ? ", \"options\": { \"lineWrap\": true }" : "");
	Put(p, head);
	PutText(p, name, false, text);
	Put(p, "\" } },\n");
}

// An unseen button over a place: a click sends `click` (when not empty), hovering shows `tip`.
void RawSpot(Page& p, const std::string& name, int x, int y, int width, int height, const std::string& click, const std::string& tip) {
	char head[400];
	snprintf(head, sizeof(head),
		"\t\t{ \"type\": \"ButtonWidget\", \"name\": \"%s\", \"fields\": { \"rect\": { \"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d },"
		" \"filename\": \"d2rloader\\\\mod-book\\\\blank\", \"normalFrame\": 0, \"pressedFrame\": 0, \"disabledFrame\": 0, \"hoveredFrame\": 0",
		name.c_str(), x, y, width, height);
	Put(p, head);
	if (!click.empty()) {
		Put(p, ", \"onClickMessage\": \"PanelManager:ClosePanel:" + p.clickName(click) + "\"");
	}
	if (!tip.empty()) {
		Put(p, ", \"tooltipString\": \"");
		PutText(p, name, true, tip);
		Put(p, "\"");
	}
	Put(p, " } },\n");
}

// A button: the game's own button picture at this width with the text on it (lit under the
// cursor). Without the picture: a word to click, "[ Save ]".
void RawButton(Page& p, const std::string& click, int x, int y, int width, const std::string& text, const char* color) {
	if (!HasButtonArt(width)) {
		RawLabel(p, "Do" + click, x, y, width, 44, color, "center", false, "[ " + text + " ]");
		RawSpot(p, "Click" + click, x, y, width, 44, click, "");
		return;
	}
	char head[500];
	snprintf(head, sizeof(head),
		"\t\t{ \"type\": \"ButtonWidget\", \"name\": \"Do%s\", \"fields\": { \"rect\": { \"x\": %d, \"y\": %d },"
		" \"filename\": \"d2rloader\\\\mod-book\\\\button%d\", \"normalFrame\": 0, \"pressedFrame\": 1, \"disabledFrame\": 2, \"hoveredFrame\": 3,"
		" \"textColor\": \"%s\", \"text/style\": \"$StyleFEButtonText\", \"pointSize\": \"$SmallFontSize\","
		" \"onClickMessage\": \"PanelManager:ClosePanel:%s\", \"textString\": \"",
		click.c_str(), x, y, width, color, p.clickName(click).c_str());
	Put(p, head + JsonText(text) + "\" } },\n");
}

void RawArrow(Page& p, const char* name, int x, const char* sprite) {
	char text[500];
	snprintf(text, sizeof(text),
		"\t\t{ \"type\": \"ButtonWidget\", \"name\": \"%s\", \"fields\": { \"rect\": { \"x\": %d, \"y\": 1352 }, \"filename\": \"PANEL\\\\Stash\\\\%s\","
		" \"normalFrame\": 0, \"hoveredFrame\": 1, \"pressedFrame\": 2, \"disabledFrame\": 3, \"onClickMessage\": \"PanelManager:ClosePanel:%s\" } },\n",
		name, x, sprite, p.clickName(name).c_str());
	Put(p, text);
}

const char* Text(const char* text) {
	return text != nullptr ? text : "";
}

// The Api's drawing calls: a tab's places, moved down when its side has a tab row.
void __cdecl ApiLabel(Page* p, const char* name, int x, int y, int width, int height, const char* color, const char* align, int wrap, const char* text) noexcept {
	if (p != nullptr) {
		RawLabel(*p, Text(name), x, p->y(y), width, height, color != nullptr ? color : "$FontColorWhite", align != nullptr ? align : "left", wrap != 0, Text(text));
	}
}
void __cdecl ApiSpot(Page* p, const char* name, int x, int y, int width, int height, const char* click, const char* tip) noexcept {
	if (p != nullptr) {
		RawSpot(*p, Text(name), x, p->y(y), width, height, Text(click), Text(tip));
	}
}
void __cdecl ApiButton(Page* p, const char* click, int x, int y, int width, const char* text, const char* color) noexcept {
	if (p != nullptr) {
		RawButton(*p, Text(click), x, p->y(y), width, Text(text), color != nullptr ? color : "$FontColorWhite");
	}
}
void __cdecl ApiImage(Page* p, const char* name, int x, int y, const char* filename) noexcept {
	if (p != nullptr) {
		char head[300];
		snprintf(head, sizeof(head), "\t\t{ \"type\": \"ImageWidget\", \"name\": \"%s\", \"fields\": { \"rect\": { \"x\": %d, \"y\": %d }, \"filename\": \"", Text(name), x, p->y(y));
		Put(*p, head + JsonText(Text(filename)) + "\" } },\n");
	}
}
constexpr int kBarPiece = 40, kBarSpotX = 1062, kBarSpotWidth = 38, kMarksMost = 49;

void __cdecl ApiBar(Page* p, int y, uint64_t top, uint64_t shown, uint64_t count) noexcept {
	if (p == nullptr || !g_barArt || count <= shown) {
		return;
	}
	p->bar = true, p->barY = p->y(y), p->barTop = top, p->barShown = shown, p->barCount = count;
	// unseen pieces over the bar: a click on one goes there, and tells where the bar is on screen (Calibrate)
	for (int seg = 0; seg * kBarPiece < kTrackHeight - p->down; ++seg) {
		const std::string n = std::to_string(seg);
		RawSpot(*p, "BarSpot" + n, kBarSpotX, p->y(y) + seg * kBarPiece, kBarSpotWidth, kBarPiece, "Bar" + n, "");
	}
	char text[500];
	snprintf(text, sizeof(text), "\t\t{ \"type\": \"ImageWidget\", \"name\": \"ScrollTrack\", \"fields\": { \"rect\": { \"x\": 1070, \"y\": %d }, \"filename\": \"d2rloader\\\\mod-book\\\\scrolltrack\" } },\n", p->y(y));
	Put(*p, text);
	const int range = kTrackHeight - kMarkHeight - p->down;
	if (!g_inPlace) { // one mark, where it belongs
		snprintf(text, sizeof(text), "\t\t{ \"type\": \"ImageWidget\", \"name\": \"ScrollMark\", \"fields\": { \"rect\": { \"x\": 1066, \"y\": %d }, \"filename\": \"d2rloader\\\\mod-book\\\\scrollmark\" } },\n",
			p->y(y) + static_cast<int>(static_cast<uint64_t>(range) * top / (count - shown)));
		Put(*p, text);
		return;
	}
	// A mark at every place it can stand (kMarksMost at most), one of them seen: moving the
	// mark is then showing another one, which needs no new panel.
	const uint64_t steps = count - shown < static_cast<uint64_t>(kMarksMost - 1) ? count - shown : static_cast<uint64_t>(kMarksMost - 1);
	p->marks             = static_cast<int>(steps) + 1;
	p->mark              = static_cast<int>((top * steps + (count - shown) / 2) / (count - shown));
	for (int i = 0; i < p->marks; ++i) {
		snprintf(text, sizeof(text), "\t\t{ \"type\": \"ImageWidget\", \"name\": \"ScrollMark%d\", \"fields\": { \"rect\": { \"x\": 1066, \"y\": %d }, \"filename\": \"d2rloader\\\\mod-book\\\\scrollmark\" } },\n",
			i, p->y(y) + static_cast<int>(static_cast<uint64_t>(range) * static_cast<uint64_t>(i) / steps));
		Put(*p, text);
	}
}
void __cdecl ApiPager(Page* p, int before, int after, const char* text) noexcept {
	if (p == nullptr) {
		return;
	}
	if (before != 0) { // an arrow is there only when there is more that way
		RawArrow(*p, "Previous", 300, "Stash_LeftArrow");
	}
	if (after != 0) {
		RawArrow(*p, "Next", 799, "Stash_RightArrow");
	}
	RawLabel(*p, "Page", 370, 1352, 422, 63, "$FontColorLightGold", "center", false, Text(text));
}

void __cdecl ApiTitle(Page* p, const char* text) noexcept {
	if (p != nullptr) {
		p->title = Text(text);
	}
}

// The bar's place in the game window, for the mouse hook's thread. UI thread.
void UpdateTrack(Side& s) {
	AcquireSRWLockExclusive(&g_typeLock);
	s.trackOk = s.calOk && s.hasBar && s.barCount > s.barShown;
	if (s.trackOk) {
		s.trackX     = (s.originXLo + s.originXHi) / 2 + s.scale * kBarSpotX;
		s.trackW     = s.scale * kBarSpotWidth;
		s.trackY     = (s.originYLo + s.originYHi) / 2 + s.scale * static_cast<float>(s.barY);
		s.markH      = s.scale * kMarkHeight;
		s.trackRange = s.scale * static_cast<float>(kTrackHeight - kMarkHeight - s.down);
		s.trackH     = s.trackRange + s.markH;
		s.dragShown = s.barShown, s.dragCount = s.barCount, s.dragTop = s.barTop;
	}
	ReleaseSRWLockExclusive(&g_typeLock);
}

// The groups of a side's tabs, "General" first, then as they came; groupOf: each tab's group.
// Empty: no tab of the side has a group (one row of tabs, as before groups).
std::vector<std::string> GroupsOf(const std::vector<int>& tabs, std::vector<int>& groupOf) {
	std::vector<std::string> names, groups;
	bool                     any = false;
	for (const int index : tabs) {
		const TabEntry* entry = TabOf(index + 1);
		names.push_back(entry != nullptr ? entry->group : std::string());
		any = any || !names.back().empty();
	}
	groupOf.assign(tabs.size(), 0);
	if (!any) {
		return groups;
	}
	for (std::string& name : names) {
		name = name.empty() ? "Other" : name;
		if (name == "General") {
			groups.assign(1, name);
		}
	}
	for (size_t i = 0; i < names.size(); ++i) {
		size_t at = 0;
		while (at < groups.size() && groups[at] != names[i]) {
			++at;
		}
		if (at == groups.size()) {
			groups.push_back(names[i]);
		}
		groupOf[i] = static_cast<int>(at);
	}
	return groups;
}

// The page a side shows now: the frame, the tab row, and what the current tab draws.
Page SidePage(Side& s) {
	const std::vector<int> tabs = TabsOf(s);
	std::vector<int>       groupOf;
	const std::vector<std::string> groups = GroupsOf(tabs, groupOf);
	const size_t           shown = static_cast<size_t>(s.current) < tabs.size() ? static_cast<size_t>(s.current) : 0;
	std::vector<size_t>    members; // the tabs of the group shown
	for (size_t i = 0; i < tabs.size() && !groups.empty(); ++i) {
		if (groupOf[i] == groupOf[shown]) {
			members.push_back(i);
		}
	}
	s.down = groups.empty() ? (tabs.size() > 1 ? modbook::kStripHeight : 0) : modbook::kStripHeight * (members.size() > 1 ? 2 : 1);
	Page      page { {}, &s, s.down };
	TabEntry* tab = !tabs.empty() ? TabOf(tabs[shown] + 1) : nullptr;
	PageStart(page, tab != nullptr ? tab->title : std::string("Mod Book"));
	const auto row = [&](size_t n, int y, const auto& button) { // 1 or 2 buttons 480 wide, 3 at 300, 4 at 232, 5 at 188
		const int width = n <= 2 ? 480 : n == 3 ? 300 : n == 4 ? 232 : 188;
		const int step  = n > 1 ? (990 - width) / static_cast<int>(n - 1) : 0;
		for (size_t i = 0; i < n && i < 5; ++i) {
			button(i, 82 + static_cast<int>(i) * step, y, width);
		}
	};
	if (!groups.empty()) {
		row(groups.size(), 262, [&](size_t i, int x, int y, int width) {
			RawButton(page, "Group" + std::to_string(i), x, y, width, groups[i], static_cast<int>(i) == groupOf[shown] ? "$FontColorWhite" : "$FontColorGoldYellow");
		});
		if (members.size() > 1) {
			row(members.size(), 262 + modbook::kStripHeight, [&](size_t i, int x, int y, int width) {
				const TabEntry* entry = TabOf(tabs[members[i]] + 1);
				RawButton(page, "Tab" + std::to_string(members[i]), x, y, width, entry != nullptr ? entry->title : "?", members[i] == shown ? "$FontColorWhite" : "$FontColorGoldYellow");
			});
		}
	} else if (tabs.size() > 1) {
		// the tab row: 2 tabs 480 wide, 3 at 300, 4 at 232, 5 at 188; the one shown is white
		const size_t n     = tabs.size();
		const int    width = n == 2 ? 480 : n == 3 ? 300 : n == 4 ? 232 : 188;
		const int    step  = n > 1 ? (990 - width) / static_cast<int>(n - 1) : 0;
		for (size_t i = 0; i < n; ++i) {
			const TabEntry* entry = TabOf(tabs[i] + 1);
			RawButton(page, "Tab" + std::to_string(i), 82 + static_cast<int>(i) * step, 262, width, entry != nullptr ? entry->title : "?",
				static_cast<int>(i) == s.current ? "$FontColorWhite" : "$FontColorGoldYellow");
		}
	}
	if (tab == nullptr) {
		RawLabel(page, "Empty", 82, 400, 990, 200, "$FontColorGoldYellow", "center", true, "No plugin has put a page here.");
	} else if (tab->tab.draw != nullptr) {
		tab->tab.draw(tab->tab.user, &page);
	}
	s.hasBar = page.bar, s.barY = page.barY, s.barTop = page.barTop, s.barShown = page.barShown, s.barCount = page.barCount;
	UpdateTrack(s);
	PageDone(page);
	return page;
}

// ---------------------------------------------------------------- showing

bool SlotIsOpen(const Slot& slot) {
	D2RL::Panels::PanelInfo info {};
	info.structSize = D2RL::Panels::PanelInfoSize;
	return g_panels->getPanelInfo(g_ctx, slot.panel, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Open;
}

// Is the side on screen? Also notices that the game closed it.
bool SideIsOpen(Side& s) {
	if (g_panels == nullptr || s.active < 0) {
		return false;
	}
	if (SlotIsOpen(s.slots[s.active])) {
		return true;
	}
	s.slots[s.active].closedAt = NowMs();
	s.active                   = -1;
	return false;
}

bool SlotRegister(Slot& slot, const std::string& layout) {
	D2RL::Resources::ResourceRegistration resource {};
	resource.structSize = D2RL::Resources::ResourceRegistrationSize;
	resource.path       = slot.path.c_str();
	resource.bytes      = layout.data();
	resource.byteCount  = layout.size();
	slot.layout         = D2RL::Resources::InvalidHandle;
	return g_resources->registerResource(g_ctx, &resource, &slot.layout) == D2RL::Resources::Result::Success;
}

TabEntry* CurrentTab(Side& s) {
	const std::vector<int> tabs = TabsOf(s);
	if (tabs.empty()) {
		return nullptr;
	}
	if (static_cast<size_t>(s.current) >= tabs.size()) {
		s.current = 0;
	}
	return TabOf(tabs[static_cast<size_t>(s.current)] + 1);
}

void Tell(Side& s, bool on) { // the current tab: on screen or not
	if (s.told == on) {
		return;
	}
	s.told = on;
	if (TabEntry* tab = CurrentTab(s); tab != nullptr && tab->tab.shown != nullptr) {
		tab->tab.shown(tab->tab.user, on ? 1 : 0);
	}
}

enum class Shown { Yes, Later, No };

void AskJob();

// ---------------------------------------------------------------- changing a page in place
//
// This D2RLoader's widget service ends before setText, so a changed page used to need a new
// panel every time (a pool of them, each resting 1.5 s after use): a dragged scroll bar drew
// 25 panels a second at best. Here the texts are changed in the panel on screen with the
// game's own routines, found in game build 3.4.95041 (image stamp 0x6ab3782c) - the ones the
// loader's own widget service calls (its slots: panel manager, find panel, find child):
//   0x3440170  the panel manager (a pointer)
//   0x89f760   panel   = FindPanel(manager, name)
//   0x855c90   child   = FindChild(widget, name)
//   0x86dd50   SetText(text block, { text, length })   - a TextBoxWidget's block is at +0x818,
//              its vtable 0x1cf4610
//   0x80cf0    Assign(string, text, length)            - a FocusableWidget's hover text is the
//              string at +0x88
// Used only when the image stamp and the routines' first bytes are these; anything that does
// not look as expected switches it off for the session (the log says what), and pages are
// drawn as new panels again.

constexpr uint32_t  kGameStamp  = 0x6ab3782c;
constexpr uintptr_t kManagerAt  = 0x3440170, kFindPanelAt = 0x89f760, kFindChildAt = 0x855c90, kSetTextAt = 0x86dd50, kAssignAt = 0x80cf0, kTextBlockTable = 0x1cf4610;
constexpr size_t    kTextBlock  = 0x818, kBlockString = 0x88, kHoverString = 0x88;

struct GameView {
	const char* text;
	size_t      length;
};
struct GameString {
	char*  text;
	size_t length;
	size_t room;
};
using FindFn    = void* (*)(void* self, const char* name);
using SetTextFn = void (*)(void* block, const GameView* text);
using AssignFn  = void* (*)(GameString* string, const char* text, size_t length);

uintptr_t g_game = 0, g_gameSize = 0;

bool Readable(const void* at, size_t size) {
	MEMORY_BASIC_INFORMATION info {};
	if (at == nullptr || VirtualQuery(at, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	return reinterpret_cast<uintptr_t>(at) + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

void InPlaceSetUp() {
	const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (base == 0 || dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
	if (nt->FileHeader.TimeDateStamp != kGameStamp) {
		LogF("in place: off - another game build (image stamp %08x, known %08x); pages are drawn as new panels", static_cast<unsigned>(nt->FileHeader.TimeDateStamp), static_cast<unsigned>(kGameStamp));
		return;
	}
	const struct {
		uintptr_t     at;
		unsigned char bytes[12];
	} routines[] = {
		{ kSetTextAt, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x42, 0x08, 0x48, 0x8b } },
		{ kFindChildAt, { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 0x20, 0x4c } },
		{ kFindPanelAt, { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 0x20, 0x4c } },
		{ kAssignAt, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89 } },
	};
	for (const auto& routine : routines) {
		if (!Readable(reinterpret_cast<const void*>(base + routine.at), sizeof(routine.bytes)) || memcmp(reinterpret_cast<const void*>(base + routine.at), routine.bytes, sizeof(routine.bytes)) != 0) {
			LogF("in place: off - the game's routine at %llx does not start as known; pages are drawn as new panels", static_cast<unsigned long long>(routine.at));
			return;
		}
	}
	g_game     = base;
	g_gameSize = nt->OptionalHeader.SizeOfImage;
	g_inPlace = true;
	LogF("in place: on - a page's texts and its scroll mark change in the panel on screen (game routines of build %08x)", static_cast<unsigned>(kGameStamp));
}

// A text as the game holds it: UTF-8, made from the tab's Latin-1 the way JsonText writes it.
std::string GameText(const std::string& text) {
	std::string out;
	if (!text.empty() && (text[0] == '@' || text[0] == '$')) {
		out += ' ';
	}
	for (const char c : text) {
		const auto u = static_cast<unsigned char>(c);
		if (c == '\n') {
			out += c;
		} else if (u < 0x20) {
			out += ' ';
		} else if (u >= 0x80) {
			out += static_cast<char>(0xc0 | (u >> 6));
			out += static_cast<char>(0x80 | (u & 0x3f));
		} else {
			out += c;
		}
	}
	return out;
}

void InPlaceOff(Side& s, const std::string& why) {
	g_inPlace = false;
	LogF("%s: in place: off - %s; pages are drawn as new panels from now on", s.id, why.c_str());
}

// The panel on screen, as the game holds it; null when it has none of that name (yet).
void* GamePanel(Side& s) {
	const auto* manager = reinterpret_cast<void* const*>(g_game + kManagerAt);
	if (s.active < 0 || g_game == 0 || !Readable(manager, sizeof(void*)) || *manager == nullptr) {
		return nullptr;
	}
	return reinterpret_cast<FindFn>(g_game + kFindPanelAt)(*manager, s.slots[s.active].name.c_str());
}

// Shows the mark `to` and hides `from` (-1: hides every other one): the widget's own "seen"
// routine, the one the loader's setWidgetVisible calls (its table, +0x50).
bool MarkShow(Side& s, int from, int to) {
	void* panel = GamePanel(s);
	if (panel == nullptr) {
		return false;
	}
	for (int i = 0; i < s.marks; ++i) {
		if (i != to && from >= 0 && i != from) {
			continue;
		}
		void* mark = reinterpret_cast<FindFn>(g_game + kFindChildAt)(panel, ("ScrollMark" + std::to_string(i)).c_str());
		if (mark == nullptr || !Readable(mark, sizeof(void*))) {
			return false;
		}
		const auto table = *reinterpret_cast<const uintptr_t*>(mark);
		if (table < g_game || table >= g_game + g_gameSize || !Readable(reinterpret_cast<const void*>(table + 0x50), sizeof(void*))) {
			return false;
		}
		const auto seen = *reinterpret_cast<const uintptr_t*>(table + 0x50);
		if (seen < g_game || seen >= g_game + g_gameSize) {
			return false;
		}
		reinterpret_cast<void (*)(void*, bool)>(seen)(mark, i == to);
	}
	return true;
}

// Makes the panel on screen say what `page` says, when only texts and the mark differ. False:
// it needs a new panel. UI thread.
bool InPlace(Side& s, const Page& page) {
	if (!g_inPlace || s.active < 0 || s.marksPending != 0 || page.shape != s.shape || page.vars.size() != s.vars.size() || page.marks != s.marks) {
		return false;
	}
	struct Change {
		void*       widget;
		bool        tip;
		std::string text;
	};
	std::vector<Change> changes;
	void*               panel = nullptr;
	for (size_t i = 0; i < page.vars.size(); ++i) {
		const Page::Var& now = page.vars[i];
		const Page::Var& old = s.vars[i];
		if (now.text == old.text) {
			continue;
		}
		if (panel == nullptr && (panel = GamePanel(s)) == nullptr) {
			return false; // the panel is not there (any more): a new one, in place stays on
		}
		void* widget = reinterpret_cast<FindFn>(g_game + kFindChildAt)(panel, now.name.c_str());
		if (widget == nullptr) {
			InPlaceOff(s, "the panel has no '" + now.name + "'");
			return false;
		}
		const std::string was = GameText(old.text);
		if (now.tip) {
			const auto* held = reinterpret_cast<const GameString*>(static_cast<char*>(widget) + kHoverString);
			if (!Readable(held, sizeof(GameString)) || held->length > 0x10000 || !Readable(held->text, held->length + 1) || held->length != was.size() || memcmp(held->text, was.data(), was.size()) != 0) {
				const bool        can   = Readable(held, sizeof(GameString)) && held->length <= 0x10000 && Readable(held->text, held->length + 1);
				const std::string found = can ? std::string(held->text, held->length < 60 ? held->length : 60) : std::string("(unreadable)");
				InPlaceOff(s, "the hover text of '" + now.name + "' is not where it is known to be (expected '" + was.substr(0, 60) + "', found '" + found + "')");
				return false;
			}
		} else {
			const auto* block = static_cast<char*>(widget) + kTextBlock;
			if (!Readable(block, kBlockString + sizeof(GameString)) || *reinterpret_cast<const uintptr_t*>(block) != g_game + kTextBlockTable) {
				InPlaceOff(s, "'" + now.name + "' is not a text box as known");
				return false;
			}
		}
		changes.push_back({ widget, now.tip, GameText(now.text) });
	}
	if (page.mark != s.mark && !MarkShow(s, s.mark, page.mark)) {
		InPlaceOff(s, "the scroll bar's mark could not be moved");
		return false;
	}
	for (const Change& change : changes) {
		if (change.tip) {
			reinterpret_cast<AssignFn>(g_game + kAssignAt)(reinterpret_cast<GameString*>(static_cast<char*>(change.widget) + kHoverString), change.text.c_str(), change.text.size());
		} else {
			const GameView view { change.text.c_str(), change.text.size() };
			reinterpret_cast<SetTextFn>(g_game + kSetTextAt)(static_cast<char*>(change.widget) + kTextBlock, &view);
		}
	}
	static bool said = false;
	if (!said) {
		said = true;
		LogF("%s: in place: the first page changed without a new panel (%zu texts)", s.id, changes.size());
	}
	s.vars = page.vars, s.mark = page.mark;
	return true;
}

// Shows the side's current page in the next free panel of its pool, then closes the one before.
Shown SideShow(Side& s) {
	const uint64_t now    = NowMs();
	int            target = -1;
	Tell(s, true); // before it draws: the tab learns it is the one shown
	Page page = SidePage(s);
	if (InPlace(s, page)) {
		return Shown::Yes; // the panel on screen now says what this page says
	}
	if (page.marks > 0 && !g_inPlace) {
		page = SidePage(s); // in place was just switched off: the page with one mark
	}
	if (s.active >= 0 && now - s.shownAt < kPace) {
		return Shown::Later; // drawn a moment ago: the newest state is drawn on the next UI update after the pace
	}
	for (int offset = 1; offset <= kSlots && target < 0; ++offset) {
		const int   index = (s.cursor + offset) % kSlots;
		const Slot& slot  = s.slots[index];
		if (index != s.active && (!slot.loaded || (slot.closedAt != 0 && now - slot.closedAt >= kCooldown))) {
			target = index;
		}
	}
	if (target < 0) {
		return Shown::Later; // every panel of the pool was used within the last 1.5 s
	}
	Slot& next = s.slots[target];
	if (next.loaded) {
		D2RL::Widgets::UiAction unload {};
		unload.structSize = D2RL::Widgets::UiActionSize;
		unload.target     = "PanelManager";
		unload.command    = "UnloadPanel";
		unload.text       = next.name.c_str();
		if (const auto result = g_widgets->dispatchUiAction(g_ctx, &unload); result != D2RL::Widgets::Result::Success) {
			LogF("%s: an old page could not be unloaded (result %u)", s.id, static_cast<unsigned>(result));
			return Shown::No;
		}
		next.loaded = false;
	}
	if (next.layout != D2RL::Resources::InvalidHandle) {
		static_cast<void>(g_resources->unregisterResource(g_ctx, next.layout));
		next.layout = D2RL::Resources::InvalidHandle;
	}
	if (!SlotRegister(next, Layout(page, next.name))) {
		LogF("%s: the page's layout could not be registered", s.id);
		return Shown::No;
	}
	const D2RL::Panels::Result opened = g_panels->openPanel(g_ctx, next.panel);
	if (opened != D2RL::Panels::Result::Success) {
		LogF("%s: could not be opened (panel result %u; 5 = not found: the game did not accept the layout, 6 = busy, 3 = no UI to open it in)", s.id, static_cast<unsigned>(opened));
		return Shown::No;
	}
	const int previous = s.active;
	s.active           = target;
	s.cursor           = target;
	next.loaded        = true;
	next.closedAt      = 0;
	s.shownAt          = now;
	s.shape = page.shape, s.vars = page.vars, s.mark = page.mark, s.marks = page.marks;
	s.marksPending = 0;
	if (page.marks > 0 && !MarkShow(s, -1, page.mark)) { // every mark was drawn: all but one are hidden now,
		s.marksPending = now;                             // or as soon as the game has the panel (Job)
		AskJob();
	}
	if (previous >= 0) {
		static_cast<void>(g_panels->closePanel(g_ctx, s.slots[previous].panel));
		s.slots[previous].closedAt = NowMs();
	}
	s.open.store(true);
	return Shown::Yes;
}

void SideClose(Side& s) {
	if (s.active >= 0) {
		static_cast<void>(g_panels->closePanel(g_ctx, s.slots[s.active].panel));
		s.slots[s.active].closedAt = NowMs();
		s.active                   = -1;
	}
	s.open.store(false);
	Tell(s, false);
}

bool AnyOpen() {
	return g_left.open.load() || g_right.open.load();
}

std::atomic<bool>     g_dragging { false }; // a scroll bar is held (the mouse hook's thread says so)
std::atomic<uint64_t> g_dragEndAt { 0 };    // when it was let go
std::atomic<long> g_upX { 0 }, g_upY { 0 }; // where the left button last came up (screen), from the mouse hook
std::atomic<uint64_t> g_upAt { 0 };

// This D2RLoader cannot say where a widget is on screen. A click on a piece of the bar can:
// the game just told us which piece, and the mouse hook saw where the button came up. That
// gives the panel's place in the window to within a piece; more clicks narrow it down, and two
// clicks far apart also check the scale (the layouts are written for a 2160-line screen).
void Calibrate(Side& s, int seg) {
	POINT      up { g_upX.load(), g_upY.load() };
	RECT       client {};
	const HWND window  = GetForegroundWindow();
	DWORD      process = 0;
	if (NowMs() - g_upAt.load() > 1500 || window == nullptr || GetWindowThreadProcessId(window, &process) == 0 || process != GetCurrentProcessId() || !ScreenToClient(window, &up)
		|| !GetClientRect(window, &client) || client.bottom <= 0) {
		return;
	}
	float scale = s.scale > 0 ? s.scale : static_cast<float>(client.bottom) / 2160.0f;
	if (s.firstSeg >= 0 && s.firstBarY == s.barY && (seg - s.firstSeg >= 6 || s.firstSeg - seg >= 6)) {
		const float measured = (static_cast<float>(up.y) - s.firstY) / static_cast<float>((seg - s.firstSeg) * kBarPiece);
		if (measured > 0.15f && measured < 4.0f && (measured > scale * 1.08f || measured < scale * 0.92f)) {
			LogF("%s: scroll bar: two clicks say a page pixel is %.3f window pixels, not %.3f; taken", s.id, measured, scale);
			scale   = measured;
			s.calOk = false;
		}
	}
	if (s.firstSeg < 0 || s.firstBarY != s.barY) {
		s.firstSeg = seg, s.firstBarY = s.barY, s.firstY = static_cast<float>(up.y);
	}
	const float yLo = static_cast<float>(up.y) - scale * static_cast<float>(s.barY + (seg + 1) * kBarPiece), yHi = static_cast<float>(up.y) - scale * static_cast<float>(s.barY + seg * kBarPiece);
	const float xLo = static_cast<float>(up.x) - scale * (kBarSpotX + kBarSpotWidth), xHi = static_cast<float>(up.x) - scale * kBarSpotX;
	const bool  was = s.calOk;
	if (s.calOk && yLo <= s.originYHi && yHi >= s.originYLo && xLo <= s.originXHi && xHi >= s.originXLo) { // agrees with what was known: narrower
		s.originYLo = yLo > s.originYLo ? yLo : s.originYLo, s.originYHi = yHi < s.originYHi ? yHi : s.originYHi;
		s.originXLo = xLo > s.originXLo ? xLo : s.originXLo, s.originXHi = xHi < s.originXHi ? xHi : s.originXHi;
	} else {
		s.originYLo = yLo, s.originYHi = yHi, s.originXLo = xLo, s.originXHi = xHi;
	}
	s.scale = scale;
	s.calOk = true;
	if (!was) {
		LogF("%s: scroll bar: a click on piece %d came up at %ld, %ld of a %ld x %ld window; a page pixel is taken as %.3f window pixels, the panel's corner at about %.0f, %.0f. The bar can be dragged now.", s.id, seg,
			up.x, up.y, client.right, client.bottom, scale, (s.originXLo + s.originXHi) / 2, (s.originYLo + s.originYHi) / 2);
	}
	UpdateTrack(s);
}

void Keyboard(bool on);
void __cdecl Job(const D2RL::PluginContext*, void*) noexcept;

void AskJob() {
	if (g_threads != nullptr) {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, Job, nullptr));
	}
}

// May the current tab be left (closed, or another tab shown)? It may say no and show why.
bool MayLeave(Side& s) {
	TabEntry* tab = CurrentTab(s);
	return tab == nullptr || tab->tab.leave == nullptr || tab->tab.leave(tab->tab.user) != 0;
}

// Opens a side, or closes it when it is open (its tab is asked first). UI thread.
void SideToggle(Side& s, int how) { // 1 open, 2 close without asking, 3 toggle
	if (g_panels == nullptr) {
		g_ctx->WriteConsoleMessage(("Mod Book: no panels - " + g_off).c_str());
		return;
	}
	const bool open = SideIsOpen(s);
	if (open && how != 1) {
		if (how == 3 && !MayLeave(s)) {
			s.dirty.store(true); // the tab shows why not
			AskJob();
			return;
		}
		SideClose(s);
		if (!AnyOpen()) {
			Keyboard(false);
		}
		return;
	}
	if (how == 2) {
		return;
	}
	const Shown shown = SideShow(s);
	if (shown == Shown::Yes) {
		Keyboard(true);
	} else if (!open) {
		g_ctx->WriteConsoleMessage(shown == Shown::Later ? "Mod Book: that panel was just closed; try again in a second."
		                                                 : "Mod Book: the panel could not be opened (it needs a game in progress); the reason is in mod-book.log.");
	}
}

// Everything asked for since the last time, on the UI thread: clicks, keys, the wheel, a tab
// wanting to be drawn again, the hook thread's look four times a second.
void __cdecl Job(const D2RL::PluginContext*, void*) noexcept {
	bool again = false;
	for (Side* side : g_sides) {
		Side& s = *side;
		AcquireSRWLockExclusive(&g_typeLock);
		const std::string              keys   = std::move(s.keys);
		const std::vector<std::string> clicks = std::move(s.clicks);
		s.keys.clear();
		s.clicks.clear();
		ReleaseSRWLockExclusive(&g_typeLock);
		const int wantTab = s.wantTab.exchange(-1);
		if (const int wantOpen = s.wantOpen.exchange(0); wantOpen != 0 || wantTab >= 0) {
			const bool open = g_panels != nullptr && SideIsOpen(s);
			if (wantTab >= 0 && wantTab != s.current && (!open || MayLeave(s))) {
				Tell(s, false);
				s.current = wantTab;
				s.dirty.store(true);
			}
			if (wantOpen != 0 && !(wantOpen == 1 && open)) {
				s.dirty.store(false);
				SideToggle(s, wantOpen);
				continue;
			}
		}
		if (g_panels == nullptr || !SideIsOpen(s)) {
			s.open.store(false);
			s.move.store(0);
			s.scroll.store(0);
			s.jump.store(0);
			Tell(s, false);
			continue;
		}
		if (s.marksPending != 0) {
			if (MarkShow(s, -1, s.mark)) {
				static bool said = false;
				if (!said) {
					said = true;
					LogF("%s: the scroll bar's spare marks were hidden %llu ms after the panel opened (not at once)", s.id, static_cast<unsigned long long>(NowMs() - s.marksPending));
				}
				s.marksPending = 0;
			} else if (NowMs() - s.marksPending > 1000) {
				s.marksPending = 0;
				InPlaceOff(s, "the scroll bar's spare marks could not be hidden");
				s.dirty.store(true);
			} else {
				again = true;
			}
		}
		int move = s.move.exchange(0);
		// While a scroll bar is held and just after it is let go, the game may still report a click
		// on whatever the mouse was over (a row, a piece of the bar): that is the drag, not a click.
		const bool dragClicks = g_dragging.load() || NowMs() - g_dragEndAt.load() < 400;
		for (const std::string& click : clicks) {
			if (dragClicks) {
				continue;
			}
			if (click == "Previous" || click == "Next") {
				move += click == "Next" ? 1 : -1;
			} else if ((click.rfind("Tab", 0) == 0 && click.size() == 4 && click[3] >= '0' && click[3] <= '9') || (click.rfind("Group", 0) == 0 && click.size() == 6 && click[5] >= '0' && click[5] <= '9')) {
				int to = click[3] - '0';
				if (click[0] == 'G') { // a group: its first tab (the one shown stays when it is of that group)
					std::vector<int>       groupOf;
					const std::vector<int> all = TabsOf(s);
					static_cast<void>(GroupsOf(all, groupOf));
					to = s.current;
					if (static_cast<size_t>(s.current) >= all.size() || groupOf[static_cast<size_t>(s.current)] != click[5] - '0') {
						for (size_t i = 0; i < all.size(); ++i) {
							if (groupOf[i] == click[5] - '0') {
								to = static_cast<int>(i);
								break;
							}
						}
					}
				}
				if (to != s.current && static_cast<size_t>(to) < TabsOf(s).size() && MayLeave(s)) {
					Tell(s, false);
					s.current = to;
				}
				s.dirty.store(true);
			} else if (click.rfind("Bar", 0) == 0 && click.size() >= 4 && click[3] >= '0' && click[3] <= '9') {
				const int seg = atoi(click.c_str() + 3);
				Calibrate(s, seg);
				if (s.hasBar && s.barCount > s.barShown && kTrackHeight - kMarkHeight - s.down > 0) { // the mark's middle goes to the piece's middle
					float part = static_cast<float>(seg * kBarPiece + kBarPiece / 2 - kMarkHeight / 2) / static_cast<float>(kTrackHeight - kMarkHeight - s.down);
					part       = part < 0 ? 0 : part > 1 ? 1 : part;
					s.dragTo.store(static_cast<long long>(part * static_cast<float>(s.barCount - s.barShown) + 0.5f));
				}
			} else if (TabEntry* tab = CurrentTab(s); tab != nullptr && tab->tab.click != nullptr) {
				tab->tab.click(tab->tab.user, click.c_str());
			}
			if (!s.open.load()) {
				break; // the tab closed its side
			}
		}
		if (!s.open.load()) {
			continue;
		}
		TabEntry* tab = CurrentTab(s);
		if (!keys.empty() && tab != nullptr && tab->tab.keys != nullptr) {
			tab->tab.keys(tab->tab.user, keys.c_str());
		}
		int scroll = s.scroll.exchange(0);
		const int jump = s.jump.exchange(0);
		if (const long long to = s.dragTo.exchange(-1); to >= 0 && s.hasBar) {
			scroll += static_cast<int>(to - static_cast<long long>(s.barTop));
			// The list stands there from now on, drawn yet or not: the next step of the drag is
			// counted from here (counting it from the last page DRAWN made the list jump).
			s.barTop = static_cast<uint64_t>(to);
			UpdateTrack(s);
		}
		if ((move != 0 || scroll != 0 || jump != 0) && tab != nullptr && tab->tab.move != nullptr) {
			tab->tab.move(tab->tab.user, move, scroll, jump);
		}
		if (!s.open.load() || !s.dirty.exchange(false)) {
			continue; // nothing to show that is not shown
		}
		const Shown shown = SideShow(s);
		if (shown == Shown::Later) {
			s.dirty.store(true);
			again = true; // the pool is cooling down: again on the next UI update
		}
	}
	if (!AnyOpen()) {
		Keyboard(false);
	} else if (again) {
		AskJob();
	}
}

// The buttons: "close panel" messages for names that are no panels.
D2RL::SharedEvents::UiMessageAction __cdecl OnMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept {
	if (event == nullptr || event->target == nullptr || event->command == nullptr || event->text == nullptr || strcmp(event->target, "PanelManager") != 0 || strcmp(event->command, "ClosePanel") != 0
		|| strncmp(event->text, kClick, sizeof(kClick) - 1) != 0) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	const char* which = event->text + sizeof(kClick) - 1;
	for (Side* side : g_sides) {
		const size_t length = strlen(side->id);
		if (strncmp(which, side->id, length) == 0) {
			AcquireSRWLockExclusive(&g_typeLock);
			side->clicks.emplace_back(which + length);
			ReleaseSRWLockExclusive(&g_typeLock);
		}
	}
	AskJob();
	return D2RL::SharedEvents::UiMessageAction::Consume;
}

// ---------------------------------------------------------------- keys and the wheel while a side is open

std::atomic<int> g_testSide { -1 }; // the test host: 0 left, 1 right; -1 = by the cursor

// Keys and the wheel go to: the open side; with both open, the one the cursor is over.
Side* TargetSide() {
	const bool left = g_left.open.load(), right = g_right.open.load();
	if (!left || !right) {
		return left ? &g_left : right ? &g_right : nullptr;
	}
	if (const int side = g_testSide.load(); side >= 0) {
		return g_sides[side];
	}
	POINT      cursor {};
	RECT       window {};
	const HWND front = GetForegroundWindow();
	if (front != nullptr && GetCursorPos(&cursor) && GetWindowRect(front, &window) && window.right > window.left) {
		return cursor.x >= (window.left + window.right) / 2 ? &g_right : &g_left;
	}
	return &g_left;
}

void Push(Side& side, char key) {
	AcquireSRWLockExclusive(&g_typeLock);
	if (side.keys.size() < 4096) {
		side.keys += key;
	}
	ReleaseSRWLockExclusive(&g_typeLock);
}

// The letter a key gives on a plain US layout, when the system cannot say (and for the tests).
char PlainChar(unsigned key, bool shift) {
	if (key >= 'A' && key <= 'Z') {
		return static_cast<char>(shift ? key : key + 32);
	}
	if (key >= '0' && key <= '9') {
		return shift ? ")!@#$%^&*("[key - '0'] : static_cast<char>(key);
	}
	if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
		return static_cast<char>('0' + (key - VK_NUMPAD0));
	}
	switch (key) {
	case VK_SPACE: return ' ';
	case VK_OEM_COMMA: return shift ? '<' : ',';
	case VK_OEM_PERIOD: return shift ? '>' : '.';
	case VK_DECIMAL: return '.';
	case VK_OEM_MINUS: return shift ? '_' : '-';
	case VK_SUBTRACT: return '-';
	case VK_OEM_PLUS: return shift ? '+' : '=';
	case VK_ADD: return '+';
	case VK_MULTIPLY: return '*';
	case VK_DIVIDE: return '/';
	case VK_OEM_7: return shift ? '"' : '\'';
	case VK_OEM_1: return shift ? ':' : ';';
	case VK_OEM_2: return shift ? '?' : '/';
	case VK_OEM_4: return shift ? '{' : '[';
	case VK_OEM_6: return shift ? '}' : ']';
	case VK_OEM_5: return shift ? '|' : '\\';
	default: return 0;
	}
}

// A key went down while a side is open; `typed` is its letter (0: not a letter).
// True: it was the book's (the game must not see it).
bool Key(unsigned key, char typed, bool shift = false, bool control = false) {
	Side* side = TargetSide();
	if (side == nullptr) {
		return false;
	}
	if (control) { // Ctrl+S, N, V, C, Z, Y go to the tab; every other Ctrl key is the game's
		const char command = key == 'S' ? modbook::kKeySave : key == 'N' ? modbook::kKeyNew : key == 'V' ? modbook::kKeyPaste : key == 'C' ? modbook::kKeyCopy : key == 'Z' ? modbook::kKeyUndo
		                   : key == 'Y' ? modbook::kKeyRedo : 0;
		if (command == 0) {
			return false;
		}
		Push(*side, command);
	} else if (key == VK_ESCAPE) {
		AcquireSRWLockExclusive(&g_typeLock);
		side->clicks.emplace_back("Escape");
		ReleaseSRWLockExclusive(&g_typeLock);
	} else if (typed != 0) {
		Push(*side, typed);
	} else if (key == VK_BACK) {
		Push(*side, modbook::kKeyBackspace);
	} else if (key == VK_DELETE) {
		Push(*side, modbook::kKeyDelete);
	} else if (key == VK_TAB) {
		Push(*side, shift ? modbook::kKeyBackTab : modbook::kKeyTab);
	} else if (key == VK_RETURN) {
		Push(*side, modbook::kKeyEnter);
	} else if (key == VK_UP || key == VK_DOWN) {
		Push(*side, key == VK_UP ? modbook::kKeyUp : modbook::kKeyDown);
	} else if (key == VK_PRIOR) {
		side->move.fetch_sub(1);
	} else if (key == VK_NEXT) {
		side->move.fetch_add(1);
	} else if (key == VK_HOME || key == VK_END) {
		side->jump.store(key == VK_HOME ? -1 : 1);
	} else {
		return false; // not the book's: F keys, left and right ...
	}
	AskJob();
	return true;
}

constexpr int kWheelRows = 2; // rows a notch of the wheel moves a list

void Wheel(int notches) {
	if (Side* side = TargetSide(); side != nullptr && notches != 0) {
		side->scroll.fetch_add(notches > 0 ? -kWheelRows : kWheelRows); // wheel up: the entries before
		AskJob();
	}
}

HHOOK             g_keyHook = nullptr, g_mouseHook = nullptr;
std::atomic<bool> g_hooksWanted { false };
HANDLE            g_hookThread = nullptr;
bool              g_keyHeld[256] = {}; // key downs the book took: their key ups are taken too

bool GameIsInFront() {
	const HWND front   = GetForegroundWindow();
	DWORD      process = 0;
	return front != nullptr && GetWindowThreadProcessId(front, &process) != 0 && process == GetCurrentProcessId();
}

LRESULT CALLBACK KeyProc(int code, WPARAM message, LPARAM parameter) {
	const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
	if (code < 0 || key == nullptr || key->vkCode > 0xff || (message != WM_KEYDOWN && message != WM_KEYUP)) {
		return CallNextHookEx(g_keyHook, code, message, parameter);
	}
	if (message == WM_KEYUP) {
		if (g_keyHeld[key->vkCode]) {
			g_keyHeld[key->vkCode] = false;
			return 1;
		}
		return CallNextHookEx(g_keyHook, code, message, parameter);
	}
	// Alt combinations stay the game's; so do Ctrl combinations but the book's six (Key)
	if (!AnyOpen() || !GameIsInFront() || (key->flags & LLKHF_ALTDOWN) != 0) {
		return CallNextHookEx(g_keyHook, code, message, parameter);
	}
	if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
		if (Key(key->vkCode, 0, false, true)) {
			g_keyHeld[key->vkCode] = true;
			return 1;
		}
		return CallNextHookEx(g_keyHook, code, message, parameter);
	}
	// the key's letter by the keyboard layout in use (Shift and Caps Lock as they are now)
	const bool shift      = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	BYTE       state[256] = {};
	state[VK_SHIFT]       = shift ? 0x80 : 0;
	state[VK_CAPITAL]     = (GetKeyState(VK_CAPITAL) & 1) != 0 ? 1 : 0;
	wchar_t   letters[4]  = {};
	const int count       = ToUnicodeEx(key->vkCode, key->scanCode, state, letters, 4, 0, GetKeyboardLayout(0));
	char      typed       = count == 1 && letters[0] >= 0x20 && letters[0] < 0x7f ? static_cast<char>(letters[0]) : 0;
	if (typed == 0 && count <= 0) {
		typed = PlainChar(key->vkCode, shift);
	}
	if (Key(key->vkCode, typed, shift)) {
		g_keyHeld[key->vkCode] = true;
		return 1;
	}
	return CallNextHookEx(g_keyHook, code, message, parameter);
}

Side* g_dragSide = nullptr; // the side whose scroll bar is held (hook thread only)
float g_dragGrab = 0;       // where on the mark it is held, from the mark's top
long long g_dragFrom = 0, g_dragLast = 0; // rows, for the log
POINT     g_dragStart {};

// The mouse on a scroll bar: the row its mark's middle stands for goes to the side. False: not on a bar.
bool DragBar(Side* held, POINT at) {
	const HWND window = GetForegroundWindow();
	if (window == nullptr || !ScreenToClient(window, &at)) {
		return false;
	}
	bool on = false;
	for (Side* side : g_sides) {
		if ((held != nullptr && side != held) || !side->open.load()) {
			continue;
		}
		AcquireSRWLockExclusive(&g_typeLock);
		const float slack = side->markH * 0.6f; // the bar is thin: some room either side (only the press is tested; held, it follows the mouse's height wherever it goes)
		if (side->trackOk && side->dragCount > side->dragShown && side->trackRange > 1
			&& (held != nullptr || (at.x >= side->trackX - slack && at.x <= side->trackX + side->trackW + slack && at.y >= side->trackY && at.y <= side->trackY + side->trackRange + side->markH))) {
			if (held == nullptr) {
				g_dragFrom  = static_cast<long long>(side->dragTop);
				g_dragStart = at;
				g_dragging.store(true);
			}
			if (held == nullptr) { // the press: on the mark it is held where it was taken, elsewhere the mark's middle comes to the mouse
				const float markTop = side->trackY + side->trackRange * static_cast<float>(side->dragTop) / static_cast<float>(side->dragCount - side->dragShown);
				const float in      = static_cast<float>(at.y) - markTop;
				g_dragGrab          = in >= 0 && in <= side->markH ? in : side->markH / 2;
			}
			float part = (static_cast<float>(at.y) - side->trackY - g_dragGrab) / side->trackRange;
			part       = part < 0 ? 0 : part > 1 ? 1 : part;
			g_dragLast = static_cast<long long>(part * static_cast<float>(side->dragCount - side->dragShown) + 0.5f);
			side->dragTo.store(g_dragLast);
			g_dragSide = side;
			on         = true;
		}
		ReleaseSRWLockExclusive(&g_typeLock);
		if (on) {
			AskJob();
			return true;
		}
	}
	return false;
}

LRESULT CALLBACK MouseProc(int code, WPARAM message, LPARAM parameter) {
	const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(parameter);
	if (code >= 0 && mouse != nullptr && message == WM_LBUTTONUP) {
		g_upX.store(mouse->pt.x), g_upY.store(mouse->pt.y);
		g_upAt.store(NowMs());
	}
	if (code >= 0 && mouse != nullptr && g_dragSide != nullptr && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP)) {
		if (message == WM_LBUTTONUP || !AnyOpen()) {
			POINT at = mouse->pt;
			if (const HWND window = GetForegroundWindow(); window != nullptr) {
				ScreenToClient(window, &at);
			}
			LogF("%s: scroll bar held at %ld, %ld (row %lld) and let go at %ld, %ld (row %lld)", g_dragSide->id, g_dragStart.x, g_dragStart.y, g_dragFrom, at.x, at.y, g_dragLast);
			g_dragEndAt.store(NowMs());
			g_dragging.store(false);
			g_dragSide = nullptr;
			return message == WM_LBUTTONUP ? 1 : CallNextHookEx(g_mouseHook, code, message, parameter); // the game never saw the press
		}
		DragBar(g_dragSide, mouse->pt);
		return CallNextHookEx(g_mouseHook, code, message, parameter);
	}
	if (code >= 0 && mouse != nullptr && message == WM_LBUTTONDOWN && AnyOpen() && GameIsInFront() && DragBar(nullptr, mouse->pt)) {
		return 1;
	}
	if (code >= 0 && mouse != nullptr && message == WM_MOUSEWHEEL && AnyOpen() && GameIsInFront()) {
		Wheel(static_cast<short>(HIWORD(mouse->mouseData)));
		return 1;
	}
	return CallNextHookEx(g_mouseHook, code, message, parameter);
}

// The hooks live on a thread of their own (a low-level hook is called on the thread that set
// it, which must be reading messages), and only while a side is open.
DWORD WINAPI HookThread(void*) {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&KeyProc), &self);
	g_keyHook   = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, self, 0);
	g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, self, 0);
	if (g_keyHook == nullptr) {
		LogF("panels: the keyboard could not be taken (error %lu): typing may set off the game's hotkeys", GetLastError());
	}
	const UINT_PTR timer = SetTimer(nullptr, 0, 250, nullptr);
	MSG            message {};
	while (g_hooksWanted.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
		if (message.message == WM_TIMER && AnyOpen()) {
			AskJob(); // notices a side the game closed by itself
		}
	}
	KillTimer(nullptr, timer);
	if (g_keyHook != nullptr) {
		UnhookWindowsHookEx(g_keyHook);
	}
	if (g_mouseHook != nullptr) {
		UnhookWindowsHookEx(g_mouseHook);
	}
	g_keyHook = g_mouseHook = nullptr;
	memset(g_keyHeld, 0, sizeof(g_keyHeld));
	return 0;
}

// Takes the keyboard and wheel for the book, or gives them back. UI thread.
void Keyboard(bool on) {
	if (on == g_hooksWanted.load()) {
		return;
	}
	g_hooksWanted.store(on);
	if (on) {
		if (g_hookThread != nullptr) { // the last one is on its way out
			WaitForSingleObject(g_hookThread, 1000);
			CloseHandle(g_hookThread);
		}
		g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
	} else if (g_hookThread != nullptr) {
		PostThreadMessageW(GetThreadId(g_hookThread), WM_NULL, 0, 0); // wakes it; it sees it is not wanted
	}
}

// ---------------------------------------------------------------- the Api

int __cdecl ApiAddTab(const modbook::Tab* tab) noexcept {
	if (tab == nullptr || tab->size < offsetof(modbook::Tab, group) || (tab->side != modbook::kLeft && tab->side != modbook::kRight) || tab->id == nullptr || tab->title == nullptr || tab->draw == nullptr) {
		LogF("a tab was refused: it is not filled in (size, side, id, title and draw are needed)");
		return 0;
	}
	Side& side  = *g_sides[tab->side];
	auto* entry = new TabEntry; // never freed: the UI thread may be drawing it
	memcpy(&entry->tab, tab, tab->size < sizeof(modbook::Tab) ? tab->size : sizeof(modbook::Tab)); // a tab from before groups has none
	entry->group     = tab->size >= sizeof(modbook::Tab) && tab->group != nullptr ? tab->group : "";
	entry->tab.group = nullptr;
	entry->tab.size  = sizeof(modbook::Tab);
	entry->id        = tab->id;
	entry->title     = tab->title;
	entry->tab.id    = entry->id.c_str();
	entry->tab.title = entry->title.c_str();
	AcquireSRWLockExclusive(&g_tabLock);
	int handle = 0;
	for (const int index : side.tabs) {
		if (g_tabs[static_cast<size_t>(index)]->id == entry->id) { // the same tab again (its plugin set itself up again): the new callbacks take its place
			*g_tabs[static_cast<size_t>(index)] = *entry;
			g_tabs[static_cast<size_t>(index)]->tab.id    = g_tabs[static_cast<size_t>(index)]->id.c_str();
			g_tabs[static_cast<size_t>(index)]->tab.title = g_tabs[static_cast<size_t>(index)]->title.c_str();
			handle = index + 1;
		}
	}
	const bool room = handle != 0 || side.tabs.size() < static_cast<size_t>(kTabsMost);
	if (handle == 0 && room) {
		g_tabs.push_back(entry);
		// the Mod Book's own Terms tab stays the last one of its side
		if (!side.tabs.empty() && g_tabs[static_cast<size_t>(side.tabs.back())]->id == "terms" && entry->id != "terms") {
			if (side.current == static_cast<int>(side.tabs.size()) - 1) {
				++side.current;
			}
			side.tabs.insert(side.tabs.end() - 1, static_cast<int>(g_tabs.size()) - 1);
		} else {
			side.tabs.push_back(static_cast<int>(g_tabs.size()) - 1);
		}
		handle = static_cast<int>(g_tabs.size());
	}
	ReleaseSRWLockExclusive(&g_tabLock);
	if (!room) {
		LogF("tab '%s' was refused: the %s side has %d tabs already", tab->id, side.id, kTabsMost);
		delete entry;
		return 0;
	}
	LogF("tab '%s' (%s) is on the %s side", entry->id.c_str(), entry->title.c_str(), side.id);
	side.dirty.store(true);
	if (side.open.load()) {
		AskJob();
	}
	return handle;
}

// A tab's side and its place there; false: no such tab.
bool Place(int handle, Side*& side, int& index) {
	const TabEntry* entry = TabOf(handle);
	if (entry == nullptr) {
		return false;
	}
	side                        = g_sides[entry->tab.side];
	const std::vector<int> tabs = TabsOf(*side);
	const auto             at   = std::find(tabs.begin(), tabs.end(), handle - 1);
	index                       = static_cast<int>(at - tabs.begin());
	return at != tabs.end();
}

void __cdecl ApiRedraw(int handle) noexcept {
	Side* side  = nullptr;
	int   index = 0;
	if (Place(handle, side, index)) {
		side->dirty.store(true);
		if (side->open.load()) {
			AskJob();
		}
	}
}

int __cdecl ApiIsShown(int handle) noexcept {
	Side* side  = nullptr;
	int   index = 0;
	return Place(handle, side, index) && side->open.load() && side->current == index ? 1 : 0;
}

void __cdecl ApiOpen(int handle) noexcept {
	Side* side  = nullptr;
	int   index = 0;
	if (Place(handle, side, index)) {
		side->wantTab.store(index);
		side->wantOpen.store(1);
		AskJob();
	}
}

void __cdecl ApiToggle(int handle) noexcept {
	Side* side  = nullptr;
	int   index = 0;
	if (Place(handle, side, index)) {
		if (ApiIsShown(handle) != 0) {
			side->wantOpen.store(3);
		} else {
			side->wantTab.store(index);
			side->wantOpen.store(1);
		}
		AskJob();
	}
}

void __cdecl ApiClose(int side) noexcept {
	if (side == modbook::kLeft || side == modbook::kRight) {
		g_sides[side]->wantOpen.store(2);
		AskJob();
	}
}

// ---------------------------------------------------------------- the Terms tab

// The words of properties and stats. The Mod Book's own tab, in the right side's General group.
struct Term {
	const char* word;
	const char* text;
};
constexpr Term kTerms[] = {
	{ "stat", "A number the game keeps on a unit (a character, a monster, an item): strength, maxhp, fireresist. Stats are what actually does something. One row of itemstatcost.txt each; saved on items by its number." },
	{ "property", "A recipe for putting stats on an item when the item is MADE: 'res-all' = roll one number and add it to four resist stats. One row of properties.txt with up to 7 lines (func, stat, set, val). An item never holds a property, only the stats it left." },
	{ "property code", "A property's name as other files use it: res-all, dmg%, allskills. Cube recipes, unique items, runewords and affixes name properties by code." },
	{ "func", "In a property line: HOW the line puts its stat on the item (1 = add the rolled value, 21 = a group of skills ...). The Funcs tab lists every func the game has." },
	{ "val", "In a property line: a fixed number some funcs use instead of a roll (func 21: which group of skills)." },
	{ "set", "In a property line: 1 on lines the game marks so (it sets the stat instead of adding to it for some funcs). Leave it as the game has it unless you know the func." },
	{ "param / par", "A property's extra number where it is USED (a recipe's 'mod 1 param', an affix's 'par'): which skill, how many charges, a per-level factor. Its meaning depends on the property's func." },
	{ "min / max", "Where a property is used: the range its number is rolled from. min = max gives a fixed number." },
	{ "op", "In a stat's row: what the stat does to OTHER stats. Op 2 = add (this stat x op base) >> op param to the op stats: 'life per level' is op 2 with base level, added to maxhp. 13 ops exist." },
	{ "op base", "The stat an op multiplies by (level, maxhp ...)." },
	{ "op param", "The op's divider: the result is divided by 2 to this power (param 3 = / 8)." },
	{ "op stat 1-3", "The stats an op's result is added to." },
	{ "live property", "Live Properties' own kind: 'N% of stat A is added to stat B', made and changed while the game runs (console: prop set)." },
	{ "val shift / kept x256", "Some stats are kept 256 times larger than the number shown (life, mana, stamina: val shift 8). Mixing such a stat with a plain one in an op gives numbers 256 times off." },
	{ "save bits", "How many bits a stat's number takes on a saved item: 9 bits hold 0-511. Changing it makes items that already hold the stat unreadable - the reason for the item checks." },
	{ "save add", "Added to a stat's number before saving so a negative number fits in the bits." },
	{ "signed", "The saved number may be negative (half of the bits' range goes below zero)." },
	{ "desc func / desc val", "How a stat's line is written on an item: '+N text', 'N% text' ... and where the number stands. Desc func 0 = the stat shows no line." },
	{ "desc priority", "Where a stat's line stands among an item's lines: higher = further up." },
	{ "string key", "The name of a text in the game's string files (ModStr1a). A stat's line names its text by key." },
	{ "row / empty row", "A line of one of the game's tables in memory. An empty row is a stat number nothing uses; a new stat is made of one." },
	{ "data version", "The game holds three sets of tables: Classic (1), Lord of Destruction (2) and Reign of the Warlock (3). A change goes to every version that has the same row." },
	{ "the game's own row", "A table row as the game read it from the mod's files, before any change here. Every card can go back to it." },
};

struct TermsView {
	int                 handle = 0;
	std::string         words;
	std::vector<size_t> found;
	size_t              top = 0;
};
TermsView        g_terms; // UI thread only
constexpr size_t kTermRows = 5;

void TermsFilter() {
	std::string words;
	for (const char letter : g_terms.words) {
		words += static_cast<char>(tolower(static_cast<unsigned char>(letter)));
	}
	g_terms.found.clear();
	for (size_t i = 0; i < sizeof(kTerms) / sizeof(kTerms[0]); ++i) {
		std::string all = std::string(kTerms[i].word) + " " + kTerms[i].text;
		for (char& letter : all) {
			letter = static_cast<char>(tolower(static_cast<unsigned char>(letter)));
		}
		bool   has = true;
		size_t at  = 0;
		while (has && at < words.size()) { // every typed word must be in it
			size_t end = words.find(' ', at);
			end        = end == std::string::npos ? words.size() : end;
			has        = end == at || all.find(words.substr(at, end - at)) != std::string::npos;
			at         = end + 1;
		}
		if (has) {
			g_terms.found.push_back(i);
		}
	}
	const size_t last = g_terms.found.size() > kTermRows ? g_terms.found.size() - kTermRows : 0;
	g_terms.top       = g_terms.top > last ? last : g_terms.top;
}

void __cdecl TermsDraw(void*, modbook::Page* page) noexcept {
	if (g_terms.found.empty() && g_terms.words.empty()) {
		TermsFilter();
	}
	ApiImage(page, "SearchSlot", 210, 138, "PANEL\\Modals\\InputSlot");
	ApiLabel(page, "Search", 250, 166, 660, 56, g_terms.words.empty() ? "$FontColorGoldYellow" : "$FontColorWhite", "left", 0, g_terms.words.empty() ? "Type to search: stat, func, op" : (g_terms.words + "_").c_str());
	for (size_t i = 0; i < kTermRows; ++i) {
		const size_t      at    = g_terms.top + i;
		const bool        there = at < g_terms.found.size();
		const int         y     = 268 + static_cast<int>(i) * 196;
		const std::string n     = std::to_string(i);
		ApiLabel(page, ("Word" + n).c_str(), 82, y, 990, 36, "$FontColorLightGold", "left", 0, there ? kTerms[g_terms.found[at]].word : "");
		ApiLabel(page, ("Text" + n).c_str(), 120, y + 36, 950, 160, "$FontColorGoldYellow", "left", 1, there ? kTerms[g_terms.found[at]].text : "");
	}
	char text[80];
	if (g_terms.found.empty()) {
		snprintf(text, sizeof(text), "no term has all of these words");
	} else {
		snprintf(text, sizeof(text), "%zu - %zu of %zu", g_terms.top + 1, g_terms.top + kTermRows < g_terms.found.size() ? g_terms.top + kTermRows : g_terms.found.size(), g_terms.found.size());
	}
	ApiBar(page, 268, g_terms.top, kTermRows, g_terms.found.size());
	ApiPager(page, g_terms.top > 0 ? 1 : 0, g_terms.top + kTermRows < g_terms.found.size() ? 1 : 0, text);
}
void __cdecl TermsClick(void*, const char* what) noexcept {
	if (what != nullptr && strcmp(what, "Escape") == 0) {
		ApiClose(modbook::kRight);
	}
}
void __cdecl TermsKeys(void*, const char* typed) noexcept {
	for (const char* at = typed; at != nullptr && *at != '\0'; ++at) {
		if (*at == modbook::kKeyBackspace) {
			if (!g_terms.words.empty()) {
				g_terms.words.pop_back();
			}
		} else if (static_cast<unsigned char>(*at) >= 0x20 && *at != modbook::kKeyDelete && *at != '"' && *at != '\\' && g_terms.words.size() < 34) {
			g_terms.words += *at;
		}
	}
	g_terms.top = 0;
	TermsFilter();
	ApiRedraw(g_terms.handle);
}
void __cdecl TermsMove(void*, int pages, int rows, int jump) noexcept {
	const long long count = static_cast<long long>(g_terms.found.size());
	const long long last  = count > static_cast<long long>(kTermRows) ? count - static_cast<long long>(kTermRows) : 0;
	long long       top   = static_cast<long long>(g_terms.top) + pages * static_cast<long long>(kTermRows) + rows;
	top                   = jump < 0 ? 0 : jump > 0 ? last : top;
	g_terms.top           = static_cast<size_t>(top < 0 ? 0 : top > last ? last : top);
	ApiRedraw(g_terms.handle);
}

void AddTermsTab() {
	const modbook::Tab tab { sizeof(modbook::Tab), modbook::kRight, "terms", "Property/Stat Terms", nullptr, TermsDraw, TermsClick, TermsKeys, TermsMove, nullptr, nullptr, "General" };
	g_terms.handle = ApiAddTab(&tab);
}

constexpr modbook::Api kApi {
	sizeof(modbook::Api), modbook::kApiVersion, ApiAddTab, ApiRedraw, ApiOpen, ApiToggle, ApiClose, ApiIsShown, ApiLabel, ApiSpot, ApiButton, ApiImage, ApiBar, ApiPager, ApiTitle,
};

// ---------------------------------------------------------------- set-up

void __cdecl ToggleLeft(const D2RL::PluginContext*, void*) noexcept {
	SideToggle(g_left, 3);
}
void __cdecl ToggleRight(const D2RL::PluginContext*, void*) noexcept {
	SideToggle(g_right, 3);
}

D2RL::Input::ActionResult __cdecl OnAction(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void* which) noexcept {
	if (event != nullptr && event->kind == D2RL::Input::ActionEventKind::Pressed && g_threads != nullptr) {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, which == &g_right ? ToggleRight : ToggleLeft, nullptr));
	}
	return D2RL::Input::ActionResult::Handled;
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	const bool right = context != nullptr && context->args != nullptr && context->argsLength != 0 && std::string(context->args, context->argsLength).find("right") != std::string::npos;
	if (g_threads != nullptr) {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, right ? ToggleRight : ToggleLeft, nullptr));
	} else {
		g_ctx->WriteConsoleMessage(("Mod Book: no panels - " + g_off).c_str());
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// At plugin start: the pools of panels with their layouts, the buttons' messages, the keys.
void SetUp() {
	const D2RL::ResourceService*    resources = nullptr;
	const D2RL::PanelService*       panels    = nullptr;
	const D2RL::WidgetService*      widgets   = nullptr;
	const D2RL::SharedEventService* events    = nullptr;
	const auto r1 = g_ctx->QueryService(&resources);
	const auto r2 = g_ctx->QueryService(&panels);
	const auto r3 = g_ctx->QueryService(&widgets);
	const auto r4 = g_ctx->QueryService(&events);
	char       seen[300];
	snprintf(seen, sizeof(seen), "resource service: answer %u, size %u (needs %u); panel: %u, %u (%u); widget: %u, %u (%u); UI message: %u, %u (%u)", static_cast<unsigned>(r1),
		resources != nullptr ? resources->serviceSize : 0, D2RL::ResourceServiceRequiredSize, static_cast<unsigned>(r2), panels != nullptr ? panels->serviceSize : 0, D2RL::PanelServiceClosePanelFieldEnd,
		static_cast<unsigned>(r3), widgets != nullptr ? widgets->serviceSize : 0, D2RL::WidgetServiceRequiredSize, static_cast<unsigned>(r4), events != nullptr ? events->serviceSize : 0,
		D2RL::SharedEventServiceRequiredSize);
	if (g_threads == nullptr || !D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize) || !D2RL::HasPanelServiceField(panels, D2RL::PanelServiceClosePanelFieldEnd)
		|| !D2RL::HasWidgetServiceField(widgets, D2RL::WidgetServiceRequiredSize) || !D2RL::HasSharedEventServiceField(events, D2RL::SharedEventServiceRequiredSize)) {
		g_off = std::string("the loader did not give the plugin a service the panels need (") + seen + "; answer 0 = given)";
		return;
	}
	g_resources = resources;
	// a 1 x 1 see-through picture for the hover buttons (the header MapSense's blank sprite has)
	static const std::string blank = [] {
		std::string  bytes(44, '\0');
		const uint8_t head[] = { 'S', 'p', 'A', '1', 31, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 };
		memcpy(bytes.data(), head, sizeof(head));
		return bytes;
	}();
	if (!RegisterSprite("data/hd/global/ui/d2rloader/mod-book/blank.sprite", blank) || !RegisterSprite("data/hd/global/ui/d2rloader/mod-book/blank.lowend.sprite", blank)) {
		g_off = "the hover picture could not be registered";
		return;
	}
	ArtSetUp();
	for (Side* side : g_sides) {
		for (int i = 0; i < kSlots; ++i) {
			Slot& slot = side->slots[i];
			char  id[40];
			snprintf(id, sizeof(id), "%s%02d", side->id, i);
			slot.name = std::string("mod-book/") + id;
			slot.path = std::string("data/global/ui/layouts/mod-book/") + id + "hd.json";
			if (!SlotRegister(slot, Layout(SidePage(*side), slot.name))) {
				g_off = "a panel's layout could not be registered";
				return;
			}
			D2RL::Panels::PanelRegistration registration {};
			registration.structSize = D2RL::Panels::PanelRegistrationSize;
			registration.flags      = D2RL::Panels::PanelFlags::CloseOnEscape;
			registration.localId    = id;
			if (const auto result = panels->registerPanel(g_ctx, &registration, &slot.panel); result != D2RL::Panels::Result::Success) {
				g_off = "a panel could not be registered (result " + std::to_string(static_cast<unsigned>(result)) + ")";
				return;
			}
		}
	}
	D2RL::SharedEvents::UiMessageListener listener {};
	listener.structSize = D2RL::SharedEvents::UiMessageListenerSize;
	listener.callback   = OnMessage;
	D2RL::SharedEvents::ListenerHandle listenerHandle = 0;
	if (const auto result = events->registerUiMessageListener(g_ctx, &listener, &listenerHandle); result != D2RL::SharedEvents::Result::Success) {
		g_off = "the panels' buttons could not be listened to (result " + std::to_string(static_cast<unsigned>(result)) + ")";
		return;
	}
	g_widgets = widgets;
	g_panels  = panels;
	g_off.clear();
	InPlaceSetUp();
	AddTermsTab();

	const D2RL::InputService* input = nullptr;
	std::string               keys  = "no keys (this D2RLoader has no input service)";
	if (g_ctx->QueryService(&input) == D2RL::ServiceQueryResult::Success && D2RL::HasInputServiceField(input, D2RL::InputServiceRequiredSize)) {
		const auto add = [&](const char* id, const char* name, D2RL::Input::Key key, Side* side) {
			D2RL::Input::ActionRegistration action {};
			action.structSize       = D2RL::Input::ActionRegistrationSize;
			action.logicalId        = id;
			action.displayName      = name;
			action.category         = "Mod Book";
			action.defaultPrimary   = { key, D2RL::Input::Modifier::Control };
			action.defaultSecondary = { D2RL::Input::Key::None, D2RL::Input::Modifier::None };
			action.callback         = OnAction;
			action.userData         = side;
			D2RL::Input::ActionHandle handle = D2RL::Input::InvalidHandle;
			return input->registerAction(g_ctx, &action, &handle) == D2RL::Input::Result::Success;
		};
		const bool first = add("book", "Mod Book (left side)", D2RL::Input::Key::B, &g_left), second = add("legend", "Mod Book legend (right side)", D2RL::Input::Key::L, &g_right);
		keys = std::string(first ? "Ctrl+B the book" : "the book's key was not registered") + ", " + (second ? "Ctrl+L the legend" : "the legend's key was not registered") + " (unless changed in the game's key settings)";
	}
	LogF("panels: ready - console command modbook [right]; %s (%s)", keys.c_str(), seen);
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "mod-book",
	.name        = "Mod Book",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "Two side panels with tabs (Ctrl+B, Ctrl+L) that other plugins put their pages into: Live Cube Recipes, Live Properties.",
	.flags       = D2RL::PluginFlags::Shared, // as Live Cube Recipes was when these panels were its own (known to get the UI services)
	.reserved    = {},
};

} // namespace

// The Api, for the plugins that put tabs in (mod_book_api.h: modbook::Find).
D2RL_PLUGIN_EXPORT const modbook::Api* __cdecl ModBookGetApi(uint32_t version) noexcept {
	return version == modbook::kApiVersion && g_ctx != nullptr ? &kApi : nullptr;
}

// What the keyboard and mouse hooks do with a key or the wheel while a side is open. Exported
// for the test hosts, which have no keyboard to press. shift: bit 0 Shift, bit 1 Ctrl.
D2RL_PLUGIN_EXPORT bool ModBookKey(unsigned key, int shift) noexcept {
	return Key(key, (shift & 2) != 0 ? 0 : PlainChar(key, (shift & 1) != 0), (shift & 1) != 0, (shift & 2) != 0);
}
D2RL_PLUGIN_EXPORT void ModBookWheel(int notches) noexcept {
	Wheel(notches);
}
// Which side the cursor is over, for the test hosts: 0 left, 1 right, -1 ask the real cursor.
D2RL_PLUGIN_EXPORT void ModBookSide(int side) noexcept {
	g_testSide.store(side);
}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) {
		return false;
	}
	g_ctx = context;
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads = nullptr;
	}
	SetUp();
	static_cast<void>(context->RegisterConsoleCommand("modbook", OnConsole, "Mod Book: open or close the book (left side); modbook right = the legend"));
	// stay loaded: other plugins hold the Api, and the hooks' code is here
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Job), &self);
	if (g_off.empty()) {
		LogF("Mod Book %s: on (D2R %s)", kVersion, context->buildVersion != nullptr ? context->buildVersion : "?");
	} else {
		LogF("Mod Book %s: no panels - %s", kVersion, g_off.c_str());
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_left.open.store(false);
	g_right.open.store(false);
	g_hooksWanted.store(false);
	if (g_hookThread != nullptr) { // the keyboard goes back to the game
		PostThreadMessageW(GetThreadId(g_hookThread), WM_NULL, 0, 0);
		WaitForSingleObject(g_hookThread, 2000);
	}
}
