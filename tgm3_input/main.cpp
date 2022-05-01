// i dont tas okay
#define WIN32_LEAN_AND_MEAN
#include "../config.h"
#include "demo.h"
#include "practice.h"
#include "keyboard.h"
#include "joystick.h"
#include <memory>
#include <Windows.h>
#include <detours.h>
#include <intrin.h>

keyboard keyboard_device;
joystick joystick_device;
base_input *devices[] = {
	&keyboard_device,
	&joystick_device
};

const config cfg("tgm3.cfg");

static char default_username[3];

// Each password char takes up 4 bits, A=0x8 B=0x4 C=0x2 D=0x1
static unsigned char default_password[3];

static const unsigned char PASSWORD_A = 0x08;
static const unsigned char PASSWORD_B = 0x04;
static const unsigned char PASSWORD_C = 0x02;
static const unsigned char PASSWORD_D = 0x01;
static const unsigned char PASSWORD_INVALID = 0x00;

using window_proc_t = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
static window_proc_t orig_window_proc;
/**
 * hook_window_proc - Window proc hook for raw input
 * @wnd:	Window handle
 * @msg:	Window message type
 * @wparam:	Additional message info
 * @lparam:	Additional message info
 *
 * Initialize and register raw input device on the first WM_PAINT. If msg is
 * WM_INPUT, grab the raw input data and pass it to the input device handlers.
 */
static LRESULT hook_window_proc(
	HWND wnd,
	UINT msg,
	WPARAM wparam,
	LPARAM lparam
) {
	static auto once = false;
	if (msg == WM_PAINT && !once) {
		for (auto &device : devices) {
			for (auto &usage : device->get_usage()) {
				RAWINPUTDEVICE rid;
				rid.usUsagePage = 1;
				rid.usUsage = usage;
				rid.dwFlags = 0;
				rid.hwndTarget = wnd;
				RegisterRawInputDevices(&rid, 1, sizeof(rid));
			}
		}
		once = true;
	} else if (msg == WM_KILLFOCUS) {
		// Make sure buttons don't get stuck
		for (auto &device : devices)
			device->clear_buttons();
	}
	
	if (msg != WM_INPUT)
		return orig_window_proc(wnd, msg, wparam, lparam);

	// Get required buffer size
	unsigned int buf_size;
	GetRawInputData(
		(HRAWINPUT)(lparam),
		RID_INPUT,
		nullptr,
		&buf_size,
		sizeof(RAWINPUTHEADER));

	// Never access input_buf directly or you'll violate strict aliasing
	auto input_buf = std::make_unique<char[]>(buf_size);
	auto *input = (RAWINPUT*)(input_buf.get());

	GetRawInputData(
		(HRAWINPUT)(lparam),
		RID_INPUT, 
		input,
		&buf_size,
		sizeof(RAWINPUTHEADER));

	for (auto &device : devices)
		device->update(input);

	return 0;
}

using get_jvs_data_t = char*(*)(int);
static get_jvs_data_t orig_get_jvs_data;
/**
 * hook_get_jvs_data - TGM3 input hook
 * @unknown:	Always 1
 *
 * Pass the data acquired from raw input to TGM3.
 */
static char *hook_get_jvs_data(const int unknown)
{
	auto *data = orig_get_jvs_data(unknown);
	auto *buttons_1p = (unsigned short*)(data + 0x184);
	auto *buttons_2p = (unsigned short*)(data + 0x186);

	*buttons_1p = 0;
	*buttons_2p = 0;
	for (const auto &device : devices) {
		*buttons_1p |= device->get_buttons_1p();
		*buttons_2p |= device->get_buttons_2p();
	}

	return data;
}

/**
 * hook_set_play_mode - Hook for function that changes play mode
 *
 * Set free play mode
 */
static void hook_set_play_mode()
{
	*(int*)(0x6418EC) = 3;
}

/**
 * hook_set_sprite_scale - Sprite scale initialization hook
 * @sprite:	Pointer to sprite data
 * @scale:	Scale to set
 *
 * Rescale sprites for different resolutions because Arika used GL_POINTS
 */
void hook_set_sprite_scale(char *sprite, const float scale)
{
	const auto res_y = *(int*)(0x40D160);
	*(float*)(sprite + 0x22C) = scale * .5F * ((float)(res_y) / 480.F);
}

struct monitor_info {
	RECT r;
	HMONITOR handle;
};

BOOL CALLBACK monitor_enum(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM param)
{
	((std::vector<monitor_info>*)(param))->push_back({ *rect, monitor });
	return TRUE;
}

using position_window_t = void(*)(int);
position_window_t orig_position_window;
/**
 * hook_position_window - Position the window to a different monitor
 * @fullscreen: Whether or not to go into borderless mode
 *
 * Use EnumDisplayMonitors to get a rect for each monitor and sort by X offset,
 * then move the window to the monitor index specified in the config
 **/
void hook_position_window(const char fullscreen)
{
	std::vector<monitor_info> monitors;
	EnumDisplayMonitors(nullptr, nullptr, monitor_enum, (LPARAM)(&monitors));

	std::sort(monitors.begin(), monitors.end(),
		[](const monitor_info &m1, const monitor_info &m2)
	{
		return m1.r.left < m2.r.left;
	});

	auto idx = cfg.value_int(0, "patches.monitor");
	if (idx >= monitors.size())
		idx = 0;

	const auto res_x = cfg.value_int(640, "patches.resolution_x");
	const auto res_y = cfg.value_int(480, "patches.resolution_y");
	auto *already_fullscreen = (int*)(0x486A68);

	if (!fullscreen && *already_fullscreen != 1)
		return;

	*already_fullscreen = fullscreen;

	const auto window = *(HWND*)(0x6415D4);

	const auto style = GetWindowLong(window, GWL_STYLE);
	static long orig_style = 0;
	if (fullscreen) {
		orig_style = style;
		SetWindowLong(window, GWL_STYLE, style & ~(WS_SYSMENU | WS_CAPTION | WS_THICKFRAME) | WS_POPUP);
	} else {
		SetWindowLong(window, GWL_STYLE, orig_style);
	}

	MoveWindow(
		window,
		monitors[idx].r.left, monitors[idx].r.top,
		res_x, res_y,
		TRUE);

	ShowCursor(!fullscreen);

	if (!fullscreen)
		return;

	MONITORINFOEX info;
	info.cbSize = sizeof(info);
	GetMonitorInfo(monitors[idx].handle, &info);

	DEVMODE dev_mode;
	dev_mode.dmSize = sizeof(dev_mode);
	if (!EnumDisplaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &dev_mode))
		return;

	dev_mode.dmPelsWidth = res_x;
	dev_mode.dmPelsHeight = res_y;
	dev_mode.dmDisplayFrequency = 60;
	dev_mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
	ChangeDisplaySettingsEx(info.szDevice, &dev_mode, nullptr, CDS_FULLSCREEN, nullptr);
}

struct name_entry
{
	char name[3];
	char unk_0x3;
	char unk_0x4;
	char unk_0x5;
	char unk_0x6;
	char unk_0x7;
	unsigned int unk_0x8;
	unsigned short unk_0xc;
	char num_entered_chars;
	char selected_char_index;
	char unk_0x10;
	char unk_0x11;
};

using initialize_name_entry_t = void (*)(name_entry*, unsigned int);
static initialize_name_entry_t orig_initialize_name_entry;

static void hook_initialize_name_entry(name_entry* entry, unsigned int param2)
{
	orig_initialize_name_entry(entry, param2);

	// Set our name
	entry->name[0] = default_username[0];
	entry->name[1] = default_username[1];
	entry->name[2] = default_username[2];

	// Say that we've entered 3 chars
	entry->num_entered_chars = 3;

	// Set the currently entered char to END (not really necessary, but it looks nicer)
	entry->selected_char_index = 0x29;
}

__declspec(naked) static void hook_verify_username()
{
	_asm {
		// We clobber the CL and EAX registers, but that's OK since our clobbered values aren't read beyond this hook

		// Jump to `skip` if the 1st username char isn't the 1st default username char
		mov cl, [ebp + 0x18 + 0]
		cmp cl, default_username[0]
		jne skip

		// Jump to `skip` if the 2nd username char isn't the 2nd default username char
		mov cl, [ebp + 0x18 + 1]
		cmp cl, default_username[1]
		jne skip

		// Jump to `skip` if the 3rd username char isn't the 3rd default username char
		mov cl, [ebp + 0x18 + 2]
		cmp cl, default_username[2]
		jne skip

		// Set our password
		// unknown.password_entry->password[0] = default_password[0];
		mov cl, default_password[0]
		mov [ebp + 0x2C + 0], cl
		// unknown.password_entry->password[1] = default_password[1];
		mov cl, default_password[1]
		mov [ebp + 0x2C + 1], cl
		// unknown.password_entry->password[2] = default_password[2];
		mov cl, default_password[2]
		mov [ebp + 0x2C + 2], cl

		// Say that we've entered 6 chars
		// unknown.password_entry->num_entered_chars = 6;
		mov [ebp + 0x2C + 6], 6

		skip:

		// Jump back to the end of the `switch` where we came from (since we overwrite a `break` statement/`jmp` instruction)
		mov eax, 0x0041593E
		jmp eax
	}
}

static void patch_ptr(unsigned int offset, unsigned int value)
{
	DWORD old_protect;
	VirtualProtect((void*)(offset), 4, PAGE_READWRITE, &old_protect);

	*((unsigned int*)offset) = value;

	VirtualProtect((void*)(offset), 4, old_protect, &old_protect);
}

static unsigned char convert_password_char(char c) {
	if (c == 'A' || c == 'a') {
		return PASSWORD_A;
	} else if (c == 'B' || c == 'b') {
		return PASSWORD_B;
	} else if (c == 'C' || c == 'c') {
		return PASSWORD_C;
	} else if (c == 'D' || c == 'd') {
		return PASSWORD_D;
	} else {
		return PASSWORD_INVALID;
	}
}

/**
 * hook_WinMain - Custom WinMain
 * @inst:	Current instance of application
 * @prev_inst:	Always null
 * @cmdline:	Console command line
 * @show_cmd:	How the window is to be shown
 *
 * Set up hooks and demo playback
 */
__declspec(dllexport) // Intel C++ was optimizing this function away for no reason
int CALLBACK hook_WinMain(
	const HINSTANCE inst,
	const HINSTANCE prev_inst,
	const char *cmdline,
	int show_cmd
) {
	for (auto &device : devices)
		device->init(cfg);

	bool has_default_username = false;
	bool has_default_password = false;
	std::string default_username_str = cfg.value_str("", "patches.default_username");
	if (default_username_str.size() > 0) {
		// Ensure the string is padded to at least 3 chars long
		default_username_str += "  ";

		has_default_username = true;
		default_username[0] = default_username_str[0];
		default_username[1] = default_username_str[1];
		default_username[2] = default_username_str[2];

		std::string default_password_str = cfg.value_str("", "patches.default_password");
		if (default_password_str.size() == 6) {
			unsigned char char1 = convert_password_char(default_password_str[0]);
			unsigned char char2 = convert_password_char(default_password_str[1]);
			unsigned char char3 = convert_password_char(default_password_str[2]);
			unsigned char char4 = convert_password_char(default_password_str[3]);
			unsigned char char5 = convert_password_char(default_password_str[4]);
			unsigned char char6 = convert_password_char(default_password_str[5]);

			if (
				char1 != PASSWORD_INVALID &&
				char2 != PASSWORD_INVALID &&
				char3 != PASSWORD_INVALID &&
				char4 != PASSWORD_INVALID &&
				char5 != PASSWORD_INVALID &&
				char6 != PASSWORD_INVALID
			) {
				has_default_password = true;
				default_password[0] = char1 | (char2 << 4);
				default_password[1] = char3 | (char4 << 4);
				default_password[2] = char5 | (char6 << 4);
			}
		}
	}

	DetourFunction((BYTE*)(0x452CE0), (BYTE*)(hook_set_play_mode));
	DetourFunction((BYTE*)(0x434E00), (BYTE*)(hook_set_sprite_scale));

	orig_position_window = (position_window_t)(DetourFunction(
		(BYTE*)(0x450E50), (BYTE*)(hook_position_window)));
	orig_window_proc = (window_proc_t)(DetourFunction(
		(BYTE*)(0x451400), (BYTE*)(hook_window_proc)));
	orig_get_jvs_data = (get_jvs_data_t)(DetourFunction(
		(BYTE*)(0x45D490), (BYTE*)(hook_get_jvs_data)));

	if (has_default_username) {
		orig_initialize_name_entry = (initialize_name_entry_t)(DetourFunction(
			(BYTE*)(0x40E950), (BYTE*)(hook_initialize_name_entry)));
	}
	if (has_default_password) {
		// This hooks a `break` (jmp) statement in a switch, that we then jump back to at the end of the hook function
		DetourFunction((BYTE*)(0x415604), (BYTE*)(hook_verify_username));
	}


	// Demo playback
	if (cmdline != nullptr && *cmdline != '\0')
		setup_playback(cmdline);
	else
		setup_recording();

	init_practice(cfg);

	// Skip the legal screen
	if (cfg.value_bool(true, "patches.skip_legal")) {
		// patch_ptr(0x00406758, 0x00406710); // Make the first screen the ARIKA screen
		patch_ptr(0x00406758, 0x0040671B); // Make the first screen the title screen
	}

	// Call the original startup function
	((void(*)())(0x42ED30))();

	return 0;
}

/**
 * DllMain - DLL entry point
 * @inst:	Handle to this module
 * @reason:	Reason for entry point call
 *
 * Create the initialization thread
 */
BOOL WINAPI DllMain(
	const HINSTANCE inst,
	const DWORD reason,
	const void *reserved
) {
	if (reason != DLL_PROCESS_ATTACH)
		return false;

	// Patch in JMP rel32 to custom WinMain
	constexpr uintptr_t WinMain = 0x42ED40;

	DWORD old_protect;
	VirtualProtect((void*)(WinMain), 5, PAGE_READWRITE, &old_protect);
	*(char*)(WinMain) = '\xE9'; // opcode
	*(uintptr_t*)(WinMain + 1) = (uintptr_t)(hook_WinMain) - WinMain - 5;
	VirtualProtect((void*)(WinMain), 5, old_protect, &old_protect);

	return true;
}