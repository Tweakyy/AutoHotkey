///////////////////////////////////////////////////////////////////////////////
//
// AutoIt
//
// Copyright (C)1999-2006:
//		- Jonathan Bennett <jon@hiddensoft.com>
//		- Others listed at http://www.autoitscript.com/autoit3/docs/credits.htm
//      - Chris Mallett (support@autohotkey.com): various enhancements and
//        adaptation of this file's functions to interface with AutoHotkey.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h" // pre-compiled headers
#include <winsock.h>  // for WSADATA.  This also requires wsock32.lib to be linked in.
#include <tlhelp32.h> // For the ProcessExist routines.
#include <wininet.h> // For URLDownloadToFile().
#include "script.h"
#include "globaldata.h" // for g_ErrorLevel and probably other globals.
#include "window.h" // For ControlExist().
#include "application.h" // For SLEEP_WITHOUT_INTERRUPTION and MsgSleep().


ResultType Script::DoRunAs(char *aCommandLine, char *aWorkingDir, bool aDisplayErrors, WORD aShowWindow
	, Var *aOutputVar, PROCESS_INFORMATION &aPI, bool &aSuccess // Output parameters we set for caller.
	, HANDLE &aNewProcess, char *aSystemErrorText)              // Same.  Caller must ensure aSystemErrorText is at least 512 in size.
{
	typedef BOOL (WINAPI *MyCreateProcessWithLogonW)(
		LPCWSTR lpUsername,                 // user's name
		LPCWSTR lpDomain,                   // user's domain
		LPCWSTR lpPassword,                 // user's password
		DWORD dwLogonFlags,                 // logon option
		LPCWSTR lpApplicationName,          // executable module name
		LPWSTR lpCommandLine,               // command-line string
		DWORD dwCreationFlags,              // creation flags
		LPVOID lpEnvironment,               // new environment block
		LPCWSTR lpCurrentDirectory,         // current directory name
		LPSTARTUPINFOW lpStartupInfo,       // startup information
		LPPROCESS_INFORMATION lpProcessInfo // process information
		);
	// Get a handle to the DLL module that contains CreateProcessWithLogonW
	HINSTANCE hinstLib = LoadLibrary("advapi32");
	if (!hinstLib)
	{
		if (aDisplayErrors)
			ScriptError("RunAs: Missing advapi32.dll." ERR_ABORT);
		return FAIL;
	}
	MyCreateProcessWithLogonW lpfnDLLProc = (MyCreateProcessWithLogonW)GetProcAddress(hinstLib, "CreateProcessWithLogonW");
	if (!lpfnDLLProc)
	{
		FreeLibrary(hinstLib);
		if (aDisplayErrors)
			ScriptError("CreateProcessWithLogonW." ERR_ABORT); // Short msg since it probably never happens.
		return FAIL;
	}
	// Set up wide char version that we need for CreateProcessWithLogon
	// init structure for running programs (wide char version)
	STARTUPINFOW wsi = {0};
	wsi.cb			= sizeof(STARTUPINFOW);
	wsi.dwFlags		= STARTF_USESHOWWINDOW;
	wsi.wShowWindow = aShowWindow;
	// The following are left initialized to 0/NULL (initialized earlier above):
	//wsi.lpReserved	= NULL;
	//wsi.lpDesktop	= NULL;
	//wsi.lpTitle		= NULL;
	//wsi.cbReserved2	= 0;
	//wsi.lpReserved2	= NULL;

	// Convert to wide character format:
	wchar_t command_line_wide[LINE_SIZE], working_dir_wide[MAX_PATH];
	mbstowcs(command_line_wide, aCommandLine, sizeof(command_line_wide));
	if (aWorkingDir && *aWorkingDir)
		mbstowcs(working_dir_wide, aWorkingDir, sizeof(working_dir_wide));
	else
		*working_dir_wide = 0;  // wide-char terminator.

	if (lpfnDLLProc(mRunAsUser, mRunAsDomain, mRunAsPass, LOGON_WITH_PROFILE, 0
		, command_line_wide, 0, 0, *working_dir_wide ? working_dir_wide : NULL, &wsi, &aPI))
	{
		aSuccess = true;
		if (aPI.hThread)
			CloseHandle(aPI.hThread); // Required to avoid memory leak.
		aNewProcess = aPI.hProcess;
		if (aOutputVar)
			aOutputVar->Assign(aPI.dwProcessId);
	}
	else
		GetLastErrorText(aSystemErrorText, 512);  // Caller has ensured that aSystemErrorText is at least this size.
	FreeLibrary(hinstLib);
	return OK;
}



VarSizeType Script::GetIP(int aAdapterIndex, char *aBuf)
{
	// aaa.bbb.ccc.ddd = 15, but allow room for larger IP's in the future.
	#define IP_ADDRESS_SIZE 32 // The maximum size of any of the strings we return, including terminator.
	if (!aBuf)
		return IP_ADDRESS_SIZE - 1;  // -1 since we're returning the length of the var's contents, not the size.

	WSADATA wsadata;
	if (WSAStartup(MAKEWORD(1, 1), &wsadata)) // Failed (it returns 0 on success).
	{
		*aBuf = '\0';
		return 0;
	}

	char host_name[256];
	gethostname(host_name, sizeof(host_name));
	HOSTENT *lpHost = gethostbyname(host_name);

	// au3: How many adapters have we?
	int adapter_count = 0;
	while (lpHost->h_addr_list[adapter_count])
		++adapter_count;

	if (aAdapterIndex >= adapter_count)
		strlcpy(aBuf, "0.0.0.0", IP_ADDRESS_SIZE);
	else
	{
		IN_ADDR inaddr;
		memcpy(&inaddr, lpHost->h_addr_list[aAdapterIndex], 4);
		strlcpy(aBuf, (char *)inet_ntoa(inaddr), IP_ADDRESS_SIZE);
	}

	WSACleanup();
	return (VarSizeType)strlen(aBuf);
}



VarSizeType Script::GetIsAdmin(char *aBuf)
{
	if (!aBuf)
		return 1;  // The length of the string "1" or "0".
	char result = '0';  // Default.
	if (g_os.IsWin9x())
		result = '1';
	else
	{
		SC_HANDLE h = OpenSCManager(NULL, NULL, SC_MANAGER_LOCK);
		if (h)
		{
			SC_LOCK lock = LockServiceDatabase(h);
			if (lock)
			{
				UnlockServiceDatabase(lock);
				result = '1'; // Current user is admin.
			}
			else
			{
				DWORD lastErr = GetLastError();
				if (lastErr == ERROR_SERVICE_DATABASE_LOCKED)
					result = '1'; // Current user is admin.
			}
			CloseServiceHandle(h);
		}
	}
	aBuf[0] = result;
	aBuf[1] = '\0';
	return 1; // Length of aBuf.
}



VarSizeType Script::ScriptGetCursor(char *aBuf)
{
	if (!aBuf)
		return SMALL_STRING_LENGTH;  // we're returning the length of the var's contents, not the size.

	POINT point;
	GetCursorPos(&point);
	HWND target_window = WindowFromPoint(point);

	// MSDN docs imply that threads must be attached for GetCursor() to work.
	// A side-effect of attaching threads or of GetCursor() itself is that mouse double-clicks
	// are interfered with, at least if this function is called repeatedly at a high frequency.
	ATTACH_THREAD_INPUT
	HCURSOR current_cursor = GetCursor();
	DETACH_THREAD_INPUT

	if (!current_cursor)
	{
		#define CURSOR_UNKNOWN "Unknown"
		strlcpy(aBuf, CURSOR_UNKNOWN, SMALL_STRING_LENGTH + 1);
		return (VarSizeType)strlen(aBuf);
	}

	// Static so that it's initialized on first use (should help performance after the first time):
	static HCURSOR sCursor[] = {LoadCursor(0,IDC_APPSTARTING), LoadCursor(0,IDC_ARROW), LoadCursor(0,IDC_CROSS)
		, LoadCursor(0,IDC_HELP), LoadCursor(0,IDC_IBEAM), LoadCursor(0,IDC_ICON), LoadCursor(0,IDC_NO)
		, LoadCursor(0,IDC_SIZE), LoadCursor(0,IDC_SIZEALL), LoadCursor(0,IDC_SIZENESW), LoadCursor(0,IDC_SIZENS)
		, LoadCursor(0,IDC_SIZENWSE), LoadCursor(0,IDC_SIZEWE), LoadCursor(0,IDC_UPARROW), LoadCursor(0,IDC_WAIT)};
	// The order in the below array must correspond to the order in the above array:
	static char *sCursorName[] = {"AppStarting", "Arrow", "Cross"
		, "Help", "IBeam", "Icon", "No"
		, "Size", "SizeAll", "SizeNESW", "SizeNS"  // NESW = NorthEast or SouthWest
		, "SizeNWSE", "SizeWE", "UpArrow", "Wait", CURSOR_UNKNOWN};  // The last item is used to mark end-of-array.
	static int cursor_count = sizeof(sCursor) / sizeof(HCURSOR);

	int a;
	for (a = 0; a < cursor_count; ++a)
		if (sCursor[a] == current_cursor)
			break;

	strlcpy(aBuf, sCursorName[a], SMALL_STRING_LENGTH + 1);  // If a is out-of-bounds, "Unknown" will be used.
	return (VarSizeType)strlen(aBuf);
}



ResultType Line::PixelGetColor(int aX, int aY, bool aUseRGB)
{
	Var *output_var = ResolveVarOfArg(0);
	if (!output_var)
		return FAIL;
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.
	output_var->Assign(); // Init to empty string regardless of whether we succeed here.
	if (!(g.CoordMode & COORD_MODE_PIXEL)) // Using relative vs. screen coordinates.
	{
		// Convert from relative to absolute (screen) coordinates:
		RECT rect;
		if (!GetWindowRect(GetForegroundWindow(), &rect))
			return OK;  // Let ErrorLevel tell the story.
		aX += rect.left;
		aY += rect.top;
	}
	HDC hdc = GetDC(NULL);
	if (!hdc)
		return OK;  // Let ErrorLevel tell the story.
	// Assign the value as an 32-bit int to match Window Spy reports color values.
	// Update for v1.0.21: Assigning in hex format seems much better, since it's easy to
	// look at a hex BGR value to get some idea of the hue.  In addition, the result
	// is zero padded to make it easier to convert to RGB and more consistent in
	// appearance:
	COLORREF color = GetPixel(hdc, aX, aY);
	ReleaseDC(NULL, hdc);

	char buf[32];
	sprintf(buf, "0x%06X", aUseRGB ? bgr_to_rgb(color) : color);
	g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
	return output_var->Assign(buf);
}



ResultType Line::SplashTextOn(int aWidth, int aHeight, char *aTitle, char *aText)
{
	// Add some caption and frame size to window:
	aWidth += GetSystemMetrics(SM_CXFIXEDFRAME) * 2;
	int min_height = GetSystemMetrics(SM_CYCAPTION) + (GetSystemMetrics(SM_CXFIXEDFRAME) * 2);
	if (g_script.mIsAutoIt2)
	{
		// I think this is probably something like how AutoIt2 does things:
		if (aHeight < min_height)
			aHeight = min_height;
	}
	else // Use the new method that seems more friendly.
		aHeight += min_height;

	POINT pt = CenterWindow(aWidth, aHeight); // Determine how to center the window in the region that excludes the task bar.

	// My: Probably not too much overhead to do this, though it probably would perform better to resize and
	// "re-text" the existing window rather than recreating it like this:
	DESTROY_SPLASH

	// Doesn't seem necessary to have it owned by the main window, but neither
	// does doing so seem to cause any harm.  Feels safer to have it be
	// an independent window.  Update: Must make it owned by the parent window
	// otherwise it will get its own task-bar icon, which is usually undesirable.
	// In addition, making it an owned window should automatically cause it to be
	// destroyed when it's parent window is destroyed:
	g_hWndSplash = CreateWindowEx(WS_EX_TOPMOST, WINDOW_CLASS_SPLASH, aTitle, WS_DISABLED|WS_POPUP|WS_CAPTION
		, pt.x, pt.y, aWidth, aHeight, g_hWnd, (HMENU)NULL, g_hInstance, NULL);

	RECT rect;
	GetClientRect(g_hWndSplash, &rect);	// get the client size

	// CREATE static label full size of client area.
	HWND static_win = CreateWindowEx(0, "static", aText, WS_CHILD|WS_VISIBLE|SS_CENTER
		, 0, 0, rect.right - rect.left, rect.bottom - rect.top, g_hWndSplash, (HMENU)NULL, g_hInstance, NULL);

	if (!g_hFontSplash)
	{
		char default_font_name[65];
		int CyPixels, nSize = 12, nWeight = FW_NORMAL;
		HDC hdc = CreateDC("DISPLAY", NULL, NULL, NULL);
		SelectObject(hdc, (HFONT)GetStockObject(DEFAULT_GUI_FONT));		// Get Default Font Name
		GetTextFace(hdc, sizeof(default_font_name) - 1, default_font_name); // -1 just in case, like AutoIt3.
		CyPixels = GetDeviceCaps(hdc, LOGPIXELSY);			// For Some Font Size Math
		DeleteDC(hdc);
		//strcpy(default_font_name,vParams[7].szValue());	// Font Name
		//nSize = vParams[8].nValue();		// Font Size
		//if ( vParams[9].nValue() >= 0 && vParams[9].nValue() <= 1000 )
		//	nWeight = vParams[9].nValue();			// Font Weight
		g_hFontSplash = CreateFont(0-(nSize*CyPixels)/72,0,0,0,nWeight,0,0,0,DEFAULT_CHARSET,
			OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,PROOF_QUALITY,FF_DONTCARE,default_font_name);	// Create Font
		// The font is deleted when by g_script's destructor.
	}

	SendMessage(static_win, WM_SETFONT, (WPARAM)g_hFontSplash, MAKELPARAM(TRUE, 0));	// Do Font
	ShowWindow(g_hWndSplash, SW_SHOWNOACTIVATE);				// Show the Splash
	// Doesn't help with the brief delay in updating the window that happens when
	// something like URLDownloadToFile is used immediately after SplashTextOn:
	//InvalidateRect(g_hWndSplash, NULL, TRUE);
	// But this does, but for now it seems unnecessary since the user can always do
	// a manual sleep in the extremely rare cases this ever happens (even when it does
	// happen, the window updates eventually, after the download starts, at least on
	// my system.  Update: Might as well do it since it's a little nicer this way
	// (the text appears more quickly when the command after the splash is something
	// that might keep our thread tied up and unable to check messages).
	SLEEP_WITHOUT_INTERRUPTION(-1)
	// UpdateWindow() would probably achieve the same effect as the above, but it feels safer to do
	// the above because it ensures that our message queue is empty prior to returning to our caller.
	return OK;
}



ResultType Line::WinMenuSelectItem(char *aTitle, char *aText, char *aMenu1, char *aMenu2
	, char *aMenu3, char *aMenu4, char *aMenu5, char *aMenu6, char *aMenu7
	, char *aExcludeTitle, char *aExcludeText)
{
	// Set up a temporary array make it easier to traverse nested menus & submenus
	// in a loop.  Also add a NULL at the end to simplify the loop a little:
	char *menu_param[] = {aMenu1, aMenu2, aMenu3, aMenu4, aMenu5, aMenu6, aMenu7, NULL};

	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.
	HWND target_window = DetermineTargetWindow(aTitle, aText, aExcludeTitle, aExcludeText);
	if (!target_window)
		return OK;  // Let ErrorLevel tell the story.

	HMENU hMenu = GetMenu(target_window);
	if (!hMenu) // Window has no menu bar.
		return OK;  // Let ErrorLevel tell the story.

	int menu_item_count = GetMenuItemCount(hMenu);
	if (menu_item_count < 1) // Menu bar has no menus.
		return OK;  // Let ErrorLevel tell the story.
	
	#define MENU_ITEM_IS_SUBMENU 0xFFFFFFFF
	UINT menu_id = MENU_ITEM_IS_SUBMENU;
	char menu_text[1024];
	bool match_found;
	size_t menu_param_length;
	int pos, target_menu_pos;
	for (int i = 0; menu_param[i] && *menu_param[i]; ++i)
	{
		if (!hMenu)  // The nesting of submenus ended prior to the end of the list of menu search terms.
			return OK;  // Let ErrorLevel tell the story.
		menu_param_length = strlen(menu_param[i]);
		target_menu_pos = (menu_param[i][menu_param_length - 1] == '&') ? ATOI(menu_param[i]) - 1 : -1;
		if (target_menu_pos > -1)
		{
			if (target_menu_pos >= menu_item_count)  // Invalid menu position (doesn't exist).
				return OK;  // Let ErrorLevel tell the story.
			#define UPDATE_MENU_VARS(menu_pos) \
			menu_id = GetMenuItemID(hMenu, menu_pos);\
			if (menu_id == MENU_ITEM_IS_SUBMENU)\
				menu_item_count = GetMenuItemCount(hMenu = GetSubMenu(hMenu, menu_pos));\
			else\
			{\
				menu_item_count = 0;\
				hMenu = NULL;\
			}
			UPDATE_MENU_VARS(target_menu_pos)
		}
		else // Searching by text rather than numerical position.
		{
			for (match_found = false, pos = 0; pos < menu_item_count; ++pos)
			{
				GetMenuString(hMenu, pos, menu_text, sizeof(menu_text) - 1, MF_BYPOSITION);
				match_found = !strnicmp(menu_text, menu_param[i], strlen(menu_param[i]));
				//match_found = strcasestr(menu_text, menu_param[i]);
				if (!match_found)
				{
					// Try again to find a match, this time without the ampersands used to indicate
					// a menu item's shortcut key:
					StrReplaceAll(menu_text, "&", "", true);
					match_found = !strnicmp(menu_text, menu_param[i], strlen(menu_param[i]));
					//match_found = strcasestr(menu_text, menu_param[i]);
				}
				if (match_found)
				{
					UPDATE_MENU_VARS(pos)
					break;
				}
			} // inner for()
			if (!match_found) // The search hierarchy (nested menus) specified in the params could not be found.
				return OK;  // Let ErrorLevel tell the story.
		} // else
	} // outer for()

	// This would happen if the outer loop above had zero iterations due to aMenu1 being NULL or blank,
	// or if the caller specified a submenu as the target (which doesn't seem valid since an app would
	// next expect to ever receive a message for a submenu?):
	if (menu_id == MENU_ITEM_IS_SUBMENU)
		return OK;  // Let ErrorLevel tell the story.

	// Since the above didn't return, the specified search hierarchy was completely found.
	PostMessage(target_window, WM_COMMAND, (WPARAM)menu_id, 0);
	return g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
}



ResultType Line::Control(char *aCmd, char *aValue, char *aControl, char *aTitle, char *aText
	, char *aExcludeTitle, char *aExcludeText)
// ATTACH_THREAD_INPUT has been tested to see if they help any of these work with controls
// in MSIE (whose Internet Explorer_TridentCmboBx2 does not respond to "Control Choose" but
// does respond to "Control Focus").  But it didn't help.
{
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR);  // Set default since there are many points of return.
	ControlCmds control_cmd = ConvertControlCmd(aCmd);
	// Since command names are validated at load-time, this only happens if the command name
	// was contained in a variable reference.  Since that is very rare, just set ErrorLevel
	// and return:
	if (control_cmd == CONTROL_CMD_INVALID)
		return OK;  // Let ErrorLevel tell the story.

	HWND target_window = DetermineTargetWindow(aTitle, aText, aExcludeTitle, aExcludeText);
	if (!target_window)
		return OK;  // Let ErrorLevel tell the story.
	HWND control_window = ControlExist(target_window, aControl);
	if (!control_window)
		return OK;  // Let ErrorLevel tell the story.

	HWND immediate_parent;  // Possibly not the same as target_window since controls can themselves have children.
	int control_id, control_index;
	DWORD dwResult, new_button_state;
	UINT msg, x_msg, y_msg;
	RECT rect;
	LPARAM lparam;
	vk_type vk;
	int key_count;

	switch(control_cmd)
	{
	case CONTROL_CMD_CHECK: // au3: Must be a Button
	case CONTROL_CMD_UNCHECK:
	{ // Need braces for ATTACH_THREAD_INPUT macro.
		new_button_state = (control_cmd == CONTROL_CMD_CHECK) ? BST_CHECKED : BST_UNCHECKED;
		if (!SendMessageTimeout(control_window, BM_GETCHECK, 0, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;  // Let ErrorLevel tell the story.
		if (dwResult == new_button_state) // It's already in the right state, so don't press it.
			break;
		// MSDN docs for BM_CLICK (and au3 author says it applies to this situation also):
		// "If the button is in a dialog box and the dialog box is not active, the BM_CLICK message
		// might fail. To ensure success in this situation, call the SetActiveWindow function to activate
		// the dialog box before sending the BM_CLICK message to the button."
		ATTACH_THREAD_INPUT
		SetActiveWindow(target_window);
		if (!GetWindowRect(control_window, &rect))	// au3: Code to primary click the centre of the control
			rect.bottom = rect.left = rect.right = rect.top = 0;
		lparam = MAKELPARAM((rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2);
		PostMessage(control_window, WM_LBUTTONDOWN, MK_LBUTTON, lparam);
		PostMessage(control_window, WM_LBUTTONUP, 0, lparam);
		DETACH_THREAD_INPUT
		break;
	}

	case CONTROL_CMD_ENABLE:
		EnableWindow(control_window, TRUE);
		break;

	case CONTROL_CMD_DISABLE:
		EnableWindow(control_window, FALSE);
		break;

	case CONTROL_CMD_SHOW:
		ShowWindow(control_window, SW_SHOWNOACTIVATE); // SW_SHOWNOACTIVATE has been seen in some example code for this purpose.
		break;

	case CONTROL_CMD_HIDE:
		ShowWindow(control_window, SW_HIDE);
		break;

	case CONTROL_CMD_STYLE:
	case CONTROL_CMD_EXSTYLE:
	{
		if (!*aValue)
			return OK; // Seems best not to treat an explicit blank as zero.  Let ErrorLevel tell the story. 
		int style_index = (control_cmd == CONTROL_CMD_STYLE) ? GWL_STYLE : GWL_EXSTYLE;
		DWORD new_style, orig_style = GetWindowLong(control_window, style_index);
		// +/-/^ are used instead of |&^ because the latter is confusing, namely that & really means &=~style, etc.
		if (!strchr("+-^", *aValue))  // | and & are used instead of +/- to allow +/- to have their native function.
			new_style = ATOU(aValue); // No prefix, so this new style will entirely replace the current style.
		else
		{
			++aValue; // Won't work combined with next line, due to next line being a macro that uses the arg twice.
			DWORD style_change = ATOU(aValue);
			switch(aValue[-1])
			{
			case '+': new_style = orig_style | style_change; break;
			case '-': new_style = orig_style & ~style_change; break;
			case '^': new_style = orig_style ^ style_change; break;
			}
		}
		// Currently, BM_SETSTYLE is not done when GetClassName() says that the control is a button/checkbox/groupbox.
		// This is because the docs for BM_SETSTYLE don't contain much, if anything, that anyone would ever
		// want to change.
		SetLastError(0); // Prior to SetWindowLong(), as recommended by MSDN.
		if (SetWindowLong(control_window, style_index, new_style) || !GetLastError()) // This is the precise way to detect success according to MSDN.
		{
			// Even if it indicated success, sometimes it failed anyway.  Find out for sure:
			if (GetWindowLong(control_window, style_index) != orig_style) // Even a partial change counts as a success.
			{
				InvalidateRect(control_window, NULL, TRUE); // Quite a few styles require this to become visibly manifest.
				return g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
			}
		}
		return OK; // Let ErrorLevel tell the story. As documented, DoControlDelay is not done for these.
	}

	case CONTROL_CMD_SHOWDROPDOWN:
	case CONTROL_CMD_HIDEDROPDOWN:
		// CB_SHOWDROPDOWN: Although the return value (dwResult) is always TRUE, SendMessageTimeout()
		// will return failure if it times out:
		if (!SendMessageTimeout(control_window, CB_SHOWDROPDOWN
			, (WPARAM)(control_cmd == CONTROL_CMD_SHOWDROPDOWN ? TRUE : FALSE)
			, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;  // Let ErrorLevel tell the story.
		break;

	case CONTROL_CMD_TABLEFT:
	case CONTROL_CMD_TABRIGHT: // must be a Tab Control
		key_count = *aValue ? ATOI(aValue) : 1;
		vk = (control_cmd == CONTROL_CMD_TABLEFT) ? VK_LEFT : VK_RIGHT;
		lparam = (LPARAM)(vk_to_sc(vk) << 16);
		for (int i = 0; i < key_count; ++i)
		{
			// DoControlDelay isn't done for every iteration because it seems likely that
			// the Sleep(0) will take care of things.
			PostMessage(control_window, WM_KEYDOWN, vk, lparam | 0x00000001);
			SLEEP_WITHOUT_INTERRUPTION(0); // Au3 uses a Sleep(0).
			PostMessage(control_window, WM_KEYUP, vk, lparam | 0xC0000001);
		}
		break;

	case CONTROL_CMD_ADD:
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. !strnicmp for TListBox/TComboBox.
			msg = CB_ADDSTRING;
		else if (strcasestr(aControl, "List"))
			msg = LB_ADDSTRING;
		else
			return OK;  // Must be ComboBox or ListBox.  Let ErrorLevel tell the story.
		if (!SendMessageTimeout(control_window, msg, 0, (LPARAM)aValue, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;  // Let ErrorLevel tell the story.
		if (dwResult == CB_ERR || dwResult == CB_ERRSPACE) // General error or insufficient space to store it.
			// CB_ERR == LB_ERR
			return OK;  // Let ErrorLevel tell the story.
		break;

	case CONTROL_CMD_DELETE:
		if (!*aValue)
			return OK;
		control_index = ATOI(aValue) - 1;
		if (control_index < 0)
			return OK;
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. strnicmp for TListBox/TComboBox.
			msg = CB_DELETESTRING;
		else if (strcasestr(aControl, "List"))
			msg = LB_DELETESTRING;
		else
			return OK;  // Must be ComboBox or ListBox.  Let ErrorLevel tell the story.
		if (!SendMessageTimeout(control_window, msg, (WPARAM)control_index, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;  // Let ErrorLevel tell the story.
		if (dwResult == CB_ERR)  // CB_ERR == LB_ERR
			return OK;  // Let ErrorLevel tell the story.
		break;

	case CONTROL_CMD_CHOOSE:
		if (!*aValue)
			return OK;
		control_index = ATOI(aValue) - 1;
		if (control_index < 0)
			return OK;  // Let ErrorLevel tell the story.
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. strnicmp for TListBox/TComboBox.
		{
			msg = CB_SETCURSEL;
			x_msg = CBN_SELCHANGE;
			y_msg = CBN_SELENDOK;
		}
		else if (strcasestr(aControl, "List"))
		{
			if (GetWindowLong(control_window, GWL_STYLE) & (LBS_EXTENDEDSEL|LBS_MULTIPLESEL))
				msg = LB_SETSEL;
			else // single-select listbox
				msg = LB_SETCURSEL;
			x_msg = LBN_SELCHANGE;
			y_msg = LBN_DBLCLK;
		}
		else
			return OK;  // Must be ComboBox or ListBox.  Let ErrorLevel tell the story.
		if (msg == LB_SETSEL) // Multi-select, so use the cumulative method.
		{
			if (!SendMessageTimeout(control_window, msg, TRUE, control_index, SMTO_ABORTIFHUNG, 2000, &dwResult))
				return OK;  // Let ErrorLevel tell the story.
		}
		else // ComboBox or single-select ListBox.
			if (!SendMessageTimeout(control_window, msg, control_index, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
				return OK;  // Let ErrorLevel tell the story.
		if (dwResult == CB_ERR)  // CB_ERR == LB_ERR
			return OK;
		if (   !(immediate_parent = GetParent(control_window))   )
			return OK;
		if (   !(control_id = GetDlgCtrlID(control_window))   )
			return OK;
		if (!SendMessageTimeout(immediate_parent, WM_COMMAND, (WPARAM)MAKELONG(control_id, x_msg)
			, (LPARAM)control_window, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;
		if (!SendMessageTimeout(immediate_parent, WM_COMMAND, (WPARAM)MAKELONG(control_id, y_msg)
			, (LPARAM)control_window, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;
		// Otherwise break and do the end-function processing.
		break;

	case CONTROL_CMD_CHOOSESTRING:
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. strnicmp for TListBox/TComboBox.
		{
			msg = CB_SELECTSTRING;
			x_msg = CBN_SELCHANGE;
			y_msg = CBN_SELENDOK;
		}
		else if (strcasestr(aControl, "List"))
		{
			if (GetWindowLong(control_window, GWL_STYLE) & (LBS_EXTENDEDSEL|LBS_MULTIPLESEL))
				msg = LB_FINDSTRING;
			else // single-select listbox
				msg = LB_SELECTSTRING;
			x_msg = LBN_SELCHANGE;
			y_msg = LBN_DBLCLK;
		}
		else
			return OK;  // Must be ComboBox or ListBox.  Let ErrorLevel tell the story.
		if (msg == LB_FINDSTRING) // Multi-select ListBox (LB_SELECTSTRING is not supported by these).
		{
			DWORD item_index;
			if (!SendMessageTimeout(control_window, msg, -1, (LPARAM)aValue, SMTO_ABORTIFHUNG, 2000, &item_index)
				|| item_index == LB_ERR
				|| !SendMessageTimeout(control_window, LB_SETSEL, TRUE, item_index, SMTO_ABORTIFHUNG, 2000, &dwResult)
				|| dwResult == LB_ERR) // Relies on short-circuit boolean.
				return OK;  // Let ErrorLevel tell the story.
		}
		else // ComboBox or single-select ListBox.
			if (!SendMessageTimeout(control_window, msg, 1, (LPARAM)aValue, SMTO_ABORTIFHUNG, 2000, &dwResult)
				|| dwResult == CB_ERR) // CB_ERR == LB_ERR
				return OK;  // Let ErrorLevel tell the story.
		if (   !(immediate_parent = GetParent(control_window))   )
			return OK;
		if (   !(control_id = GetDlgCtrlID(control_window))   )
			return OK;
		if (!SendMessageTimeout(immediate_parent, WM_COMMAND, (WPARAM)MAKELONG(control_id, x_msg)
			, (LPARAM)control_window, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;
		if (!SendMessageTimeout(immediate_parent, WM_COMMAND, (WPARAM)MAKELONG(control_id, y_msg)
			, (LPARAM)control_window, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;
		// Otherwise break and do the end-function processing.
		break;

	case CONTROL_CMD_EDITPASTE:
		if (!SendMessageTimeout(control_window, EM_REPLACESEL, TRUE, (LPARAM)aValue, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return OK;  // Let ErrorLevel tell the story.
		// Note: dwResult is not used by EM_REPLACESEL since it doesn't return a value.
		break;
	} // switch()

	DoControlDelay;  // Seems safest to do this for all of these commands.
	return g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
}



ResultType Line::ControlGet(char *aCmd, char *aValue, char *aControl, char *aTitle, char *aText
	, char *aExcludeTitle, char *aExcludeText)
{
	Var *output_var = ResolveVarOfArg(0);
	if (!output_var)
		return FAIL;
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR);  // Set default since there are many points of return.
	ControlGetCmds control_cmd = ConvertControlGetCmd(aCmd);
	// Since command names are validated at load-time, this only happens if the command name
	// was contained in a variable reference.  Since that is very rare, just set ErrorLevel
	// and return:
	if (control_cmd == CONTROLGET_CMD_INVALID)
		return output_var->Assign();  // Let ErrorLevel tell the story.

	HWND target_window = DetermineTargetWindow(aTitle, aText, aExcludeTitle, aExcludeText);
	if (!target_window)
		return output_var->Assign();  // Let ErrorLevel tell the story.
	HWND control_window = ControlExist(target_window, aControl);
	if (!control_window)
		return output_var->Assign();  // Let ErrorLevel tell the story.

	DWORD dwResult, index, length, item_length, start, end, u, item_count;
	UINT msg, x_msg, y_msg;
	int control_index;
	char *cp, *dyn_buf, buf[32768];  // 32768 is the size Au3 uses for GETLINE and such.

	switch(control_cmd)
	{
	case CONTROLGET_CMD_CHECKED: //Must be a Button
		if (!SendMessageTimeout(control_window, BM_GETCHECK, 0, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return output_var->Assign();
		output_var->Assign(dwResult == BST_CHECKED ? "1" : "0");
		break;

	case CONTROLGET_CMD_ENABLED:
		output_var->Assign(IsWindowEnabled(control_window) ? "1" : "0");
		break;

	case CONTROLGET_CMD_VISIBLE:
		output_var->Assign(IsWindowVisible(control_window) ? "1" : "0");
		break;

	case CONTROLGET_CMD_TAB: // must be a Tab Control
		if (!SendMessageTimeout(control_window, TCM_GETCURSEL, 0, 0, SMTO_ABORTIFHUNG, 2000, &index))
			return output_var->Assign();
		if (index == -1)
			return output_var->Assign();
		output_var->Assign(index + 1);
		break;

	case CONTROLGET_CMD_FINDSTRING:
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. strnicmp for TListBox/TComboBox.
			msg = CB_FINDSTRINGEXACT;
		else if (strcasestr(aControl, "List"))
			msg = LB_FINDSTRINGEXACT;
		else // Must be ComboBox or ListBox
			return output_var->Assign();  // Let ErrorLevel tell the story.
		if (!SendMessageTimeout(control_window, msg, 1, (LPARAM)aValue, SMTO_ABORTIFHUNG, 2000, &index))
			return output_var->Assign();
		if (index == CB_ERR)  // CB_ERR == LB_ERR
			return output_var->Assign();
		output_var->Assign(index + 1);
		break;

	case CONTROLGET_CMD_CHOICE:
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. strnicmp for TListBox/TComboBox.
		{
			msg = CB_GETCURSEL;
			x_msg = CB_GETLBTEXTLEN;
			y_msg = CB_GETLBTEXT;
		}
		else if (strcasestr(aControl, "List"))
		{
			msg = LB_GETCURSEL;
			x_msg = LB_GETTEXTLEN;
			y_msg = LB_GETTEXT;
		}
		else // Must be ComboBox or ListBox
			return output_var->Assign();  // Let ErrorLevel tell the story.
		if (!SendMessageTimeout(control_window, msg, 0, 0, SMTO_ABORTIFHUNG, 2000, &index))
			return output_var->Assign();
		if (index == CB_ERR)  // CB_ERR == LB_ERR.  There is no selection (or very rarely, some other type of problem).
			return output_var->Assign();
		if (!SendMessageTimeout(control_window, x_msg, (WPARAM)index, 0, SMTO_ABORTIFHUNG, 2000, &length))
			return output_var->Assign();
		if (length == CB_ERR)  // CB_ERR == LB_ERR
			return output_var->Assign();
		// In unusual cases, MSDN says the indicated length might be longer than it actually winds up
		// being when the item's text is retrieved.  This should be harmless, since there are many
		// other precedents where a variable is sized to something larger than it winds up carrying.
		// Set up the var, enlarging it if necessary.  If the output_var is of type VAR_CLIPBOARD,
		// this call will set up the clipboard for writing:
		if (output_var->Assign(NULL, (VarSizeType)length) != OK) // It already displayed the error.
			return FAIL;
		if (!SendMessageTimeout(control_window, y_msg, (WPARAM)index, (LPARAM)output_var->Contents()
			, SMTO_ABORTIFHUNG, 2000, &length))
		{
			output_var->Close(); // In case it's the clipboard.
			return output_var->Assign(); // Let ErrorLevel tell the story.
		}
		output_var->Close(); // In case it's the clipboard.
		if (length == CB_ERR)  // Probably impossible given the way it was called above.  Also, CB_ERR == LB_ERR
			return output_var->Assign(); // Let ErrorLevel tell the story.
		output_var->Length() = length;  // Update to actual vs. estimated length.
		break;

	case CONTROLGET_CMD_LIST:
		if (!strnicmp(aControl, "SysListView32", 13)) // Tried strcasestr(aControl, "ListView") to get it to work with IZArc's Delphi TListView1, but none of the modes or options worked.
			return ControlGetListView(*output_var, control_window, aValue); // It will also set ErrorLevel to "success" if successful.
		// This is done here as the special LIST sub-command rather than just being built into
		// ControlGetText because ControlGetText already has a function for ComboBoxes: it fetches
		// the current selection.  Although ListBox does not have such a function, it seem best
		// to consolidate both methods here.
		if (strcasestr(aControl, "Combo")) // v1.0.42: Changed to strcasestr vs. strnicmp for TListBox/TComboBox.
		{
			msg = CB_GETCOUNT;
			x_msg = CB_GETLBTEXTLEN;
			y_msg = CB_GETLBTEXT;
		}
		else if (strcasestr(aControl, "List"))
		{
			msg = LB_GETCOUNT;
			x_msg = LB_GETTEXTLEN;
			y_msg = LB_GETTEXT;
		}
		else // Must be ComboBox or ListBox
			return output_var->Assign();  // Let ErrorLevel tell the story.
		if (!(SendMessageTimeout(control_window, msg, 0, 0, SMTO_ABORTIFHUNG, 5000, &item_count))
			|| item_count < 1) // No items in ListBox/ComboBox or there was a problem getting the count.
			return output_var->Assign();  // Let ErrorLevel tell the story.
		// Calculate the length of delimited list of items.  Length is initialized to provide enough
		// room for each item's delimiter (the last item does not have a delimiter).
		for (length = item_count - 1, u = 0; u < item_count; ++u)
		{
			if (!SendMessageTimeout(control_window, x_msg, u, 0, SMTO_ABORTIFHUNG, 5000, &item_length)
				|| item_length == LB_ERR) // Note that item_length is legitimately zero for a blank item in the list.
				return output_var->Assign();  // Let ErrorLevel tell the story.
			length += item_length;
		}
		// In unusual cases, MSDN says the indicated length might be longer than it actually winds up
		// being when the item's text is retrieved.  This should be harmless, since there are many
		// other precedents where a variable is sized to something larger than it winds up carrying.
		// Set up the var, enlarging it if necessary.  If the output_var is of type VAR_CLIPBOARD,
		// this call will set up the clipboard for writing:
		if (output_var->Assign(NULL, (VarSizeType)length) != OK)
			return FAIL;  // It already displayed the error.
		for (cp = output_var->Contents(), length = item_count - 1, u = 0; u < item_count; ++u)
		{
			if (SendMessageTimeout(control_window, y_msg, (WPARAM)u, (LPARAM)cp, SMTO_ABORTIFHUNG, 5000, &item_length)
				&& item_length != LB_ERR)
			{
				length += item_length; // Accumulate actual vs. estimated length.
				cp += item_length;  // Point it to the terminator in preparation for the next write.
			}
			//else do nothing, just consider this to be a blank item so that the process can continue.
			if (u < item_count - 1)
				*cp++ = '\n'; // Add delimiter after each item except the last (helps parsing loop).
			// Above: In this case, seems better to use \n rather than pipe as default delimiter in case
			// the listbox/combobox contains any real pipes.
		}
		output_var->Length() = (VarSizeType)length;  // Update it to the actual length, which can vary from the estimate.
		output_var->Close(); // In case it's the clipboard.
		break;

	case CONTROLGET_CMD_LINECOUNT:  //Must be an Edit
		// MSDN: "If the control has no text, the return value is 1. The return value will never be less than 1."
		if (!SendMessageTimeout(control_window, EM_GETLINECOUNT, 0, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return output_var->Assign();
		output_var->Assign(dwResult);
		break;

	case CONTROLGET_CMD_CURRENTLINE:
		if (!SendMessageTimeout(control_window, EM_LINEFROMCHAR, -1, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return output_var->Assign();
		output_var->Assign(dwResult + 1);
		break;

	case CONTROLGET_CMD_CURRENTCOL:
	{
		if (!SendMessageTimeout(control_window, EM_GETSEL, (WPARAM)&start, (LPARAM)&end, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return output_var->Assign();
		// The dwResult from the above is not useful and is not checked.
		DWORD line_number;
		if (!SendMessageTimeout(control_window, EM_LINEFROMCHAR, (WPARAM)start, 0, SMTO_ABORTIFHUNG, 2000, &line_number))
			return output_var->Assign();
		if (!line_number) // Since we're on line zero, the column number is simply start+1.
		{
			output_var->Assign(start + 1);  // +1 to convert from zero based.
			break;
		}
		// Au3: Decrement the character index until the row changes.  Difference between this
		// char index and original is the column:
		DWORD start_orig = start;  // Au3: the character index
		for (;;)
		{
			if (!SendMessageTimeout(control_window, EM_LINEFROMCHAR, (WPARAM)start, 0, SMTO_ABORTIFHUNG, 2000, &dwResult))
				return output_var->Assign();
			if (dwResult != line_number)
				break;
			--start;
		}
		output_var->Assign((int)(start_orig - start));
		break;
	}

	case CONTROLGET_CMD_LINE:
		if (!*aValue)
			return output_var->Assign();
		control_index = ATOI(aValue) - 1;
		if (control_index < 0)
			return output_var->Assign();  // Let ErrorLevel tell the story.
		*((LPINT)buf) = sizeof(buf);  // EM_GETLINE requires first word of string to be set to its size.
		if (!SendMessageTimeout(control_window, EM_GETLINE, (WPARAM)control_index, (LPARAM)buf, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return output_var->Assign();
		if (!dwResult) // due to the specified line number being greater than the number of lines in the edit control.
			return output_var->Assign();
		buf[dwResult] = '\0'; // Ensure terminated since the API might not do it in some cases.
		output_var->Assign(buf);
		break;

	case CONTROLGET_CMD_SELECTED: // Must be an Edit.
		// Note: The RichEdit controls of certain apps such as Metapad don't return the right selection
		// with this technique.  Au3 has the same problem with them, so for now it's just documented here
		// as a limitation.
		if (!SendMessageTimeout(control_window, EM_GETSEL, (WPARAM)&start,(LPARAM)&end, SMTO_ABORTIFHUNG, 2000, &dwResult))
			return output_var->Assign();
		// The above sets start to be the zero-based position of the start of the selection (similar for end).
		// If there is no selection, start and end will be equal, at least in the edit controls I tried it with.
		// The dwResult from the above is not useful and is not checked.
		if (start == end) // Unlike Au3, it seems best to consider a blank selection to be a non-error.
		{
			output_var->Assign();
			break;
		}
		if (!SendMessageTimeout(control_window, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 2000, &length))
			return output_var->Assign();
		if (!length)
			// Since the above didn't return for start == end, this is an error because
			// we have a selection of non-zero length, but no text to go with it!
			return output_var->Assign();
		// Uses calloc() because must get all the control's text so that just the selected region
		// can be cropped out and assigned to the output variable.  Otherwise, output_var might
		// have to be sized much larger than it would need to be:
		if (   !(dyn_buf = (char *)calloc(256 + length, 1))   )
			return output_var->Assign();
		if (!SendMessageTimeout(control_window, WM_GETTEXT, (WPARAM)(length + 1), (LPARAM)dyn_buf, SMTO_ABORTIFHUNG, 2000, &length))
		{
			free(dyn_buf);
			return output_var->Assign();
		}
		if (!length || end > length)
		{
			// The first check above is reveals a problem (ErrorLevel = 1) since the length
			// is unexpectedly zero (above implied it shouldn't be).  The second check is also
			// a problem because the end of the selection should not be beyond length of text
			// that was retrieved.
			free(dyn_buf);
			return output_var->Assign();
		}
		dyn_buf[end] = '\0'; // Terminate the string at the end of the selection.
		output_var->Assign(dyn_buf + start);
		free(dyn_buf);
		break;

	case CONTROLGET_CMD_STYLE:
		// Seems best to always format as hex, since it has more human-readable meaning then:
		sprintf(buf, "0x%08X", GetWindowLong(control_window, GWL_STYLE));
		output_var->Assign(buf);
		break;

	case CONTROLGET_CMD_EXSTYLE:
		// Seems best to always format as hex, since it has more human-readable meaning then:
		sprintf(buf, "0x%08X", GetWindowLong(control_window, GWL_EXSTYLE));
		output_var->Assign(buf);
		break;
	}

	// Note that ControlDelay is not done for the Get type commands, because it seems unnecessary.
	return g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
}



void Line::MouseMove(int aX, int aY, int aSpeed, bool aMoveRelative)
{
	POINT ptCur;
	int xCur, yCur;
	int delta;
	const int	nMinSpeed = 32;
	RECT rect;

	if (aSpeed < 0) // This can happen during script's runtime due to MouseClick's speed being a var containing a neg.
		aSpeed = 0;  // 0 is the fastest.
	else
		if (aSpeed > MAX_MOUSE_SPEED)
			aSpeed = MAX_MOUSE_SPEED;

	if (aMoveRelative)  // We're moving the mouse cursor relative to its current position.
	{
		GetCursorPos(&ptCur);
		aX += ptCur.x;
		aY += ptCur.y;
	}
	else if (!(g.CoordMode & COORD_MODE_MOUSE))  // Moving mouse relative to the active window (rather than screen).
	{
		HWND fore = GetForegroundWindow();
		// Revert to screen coordinates if the foreground window is minimized.  Although it might be
		// impossible for a visible window to be both foreground and minmized, it seems that hidden
		// windows -- such as the script's own main window when activated for the purpose of showing
		// a popup menu -- can be foreground while simulateously being minimized.  This fixes an
		// issue where the mouse will move to the upper-left corner of the screen rather than the
		// intended coordinates (v1.0.17):
		if (fore && !IsIconic(fore) && GetWindowRect(fore, &rect))
		{
			aX += rect.left;
			aY += rect.top;
		}
	}

	// AutoIt3: Get size of desktop
	if (!GetWindowRect(GetDesktopWindow(), &rect)) // Might fail if there is no desktop (e.g. user not logged in).
		rect.bottom = rect.left = rect.right = rect.top = 0;  // Arbitrary defaults.

	// AutoIt3: Convert our coords to MOUSEEVENTF_ABSOLUTE coords
	// v1.0.21: No actual change was made, but the below comments were added:
	// Jack Horsfield reports that MouseMove works properly on his multi-monitor system which has
	// its secondary display to the right of the primary.  He said that MouseClick is able to click on
	// windows that exist entirely on the secondary display. That's a bit baffling because the below
	// formula should yield a value greater than 65535 when the destination coordinates are to
	// the right of the primary display (due to the fact that GetDesktopWindow() called above yields
	// the rect for the primary display only).  Chances are, mouse_event() in Win2k/XP (and possibly
	// Win98, but that is far more doubtful given some of the things mentioned on MSDN) has been
	// enhanced (undocumented) to support values greater than 65535 (and perhaps even negative by
	// taking advantage of DWORD overflow?)
	aX = ((65535 * aX) / (rect.right - 1)) + 1;
	aY = ((65535 * aY) / (rect.bottom - 1)) + 1;

	// AutoIt3: Are we slowly moving or insta-moving?
	if (aSpeed == 0)
	{
		MouseEvent(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, aX, aY);
		DoMouseDelay(); // Should definitely do this in case the action immediately after this is a click.
		return;
	}

	// AutoIt3: Sanity check for speed
	if (aSpeed < 0 || aSpeed > MAX_MOUSE_SPEED)
		aSpeed = g.DefaultMouseSpeed;

	// AutoIt3: So, it's a more gradual speed that is needed :)
	GetCursorPos(&ptCur);
	// Convert to MOUSEEVENTF_ABSOLUTE coords
	xCur = ((ptCur.x * 65535) / (rect.right - 1)) + 1;
	yCur = ((ptCur.y * 65535) / (rect.bottom - 1)) + 1;

	while (xCur != aX || yCur != aY)
	{
		if (xCur < aX)
		{
			delta = (aX - xCur) / aSpeed;
			if (delta == 0 || delta < nMinSpeed)
				delta = nMinSpeed;
			if ((xCur + delta) > aX)
				xCur = aX;
			else
				xCur += delta;
		} 
		else 
			if (xCur > aX)
			{
				delta = (xCur - aX) / aSpeed;
				if (delta == 0 || delta < nMinSpeed)
					delta = nMinSpeed;
				if ((xCur - delta) < aX)
					xCur = aX;
				else
					xCur -= delta;
			}

		if (yCur < aY)
		{
			delta = (aY - yCur) / aSpeed;
			if (delta == 0 || delta < nMinSpeed)
				delta = nMinSpeed;
			if ((yCur + delta) > aY)
				yCur = aY;
			else
				yCur += delta;
		} 
		else 
			if (yCur > aY)
			{
				delta = (yCur - aY) / aSpeed;
				if (delta == 0 || delta < nMinSpeed)
					delta = nMinSpeed;
				if ((yCur - delta) < aY)
					yCur = aY;
				else
					yCur -= delta;
			}

		MouseEvent(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, xCur, yCur);
		DoMouseDelay();
	}
}



ResultType Line::URLDownloadToFile(char *aURL, char *aFilespec)
{
	// Check that we have IE3 and access to wininet.dll
	HINSTANCE hinstLib = LoadLibrary("wininet");
	if (!hinstLib)
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);

	typedef HINTERNET (WINAPI *MyInternetOpen)(LPCTSTR, DWORD, LPCTSTR, LPCTSTR, DWORD dwFlags);
	typedef HINTERNET (WINAPI *MyInternetOpenUrl)(HINTERNET hInternet, LPCTSTR, LPCTSTR, DWORD, DWORD, LPDWORD);
	typedef BOOL (WINAPI *MyInternetCloseHandle)(HINTERNET);
	typedef BOOL (WINAPI *MyInternetReadFileEx)(HINTERNET, LPINTERNET_BUFFERS, DWORD, DWORD);

	#ifndef INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY
		#define INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY 4
	#endif

	// Get the address of all the functions we require.  It's done this way in case the system
	// lacks MSIE v3.0+, in which case the app would probably refuse to launch at all:
 	MyInternetOpen lpfnInternetOpen = (MyInternetOpen)GetProcAddress(hinstLib, "InternetOpenA");
	MyInternetOpenUrl lpfnInternetOpenUrl = (MyInternetOpenUrl)GetProcAddress(hinstLib, "InternetOpenUrlA");
	MyInternetCloseHandle lpfnInternetCloseHandle = (MyInternetCloseHandle)GetProcAddress(hinstLib, "InternetCloseHandle");
	MyInternetReadFileEx lpfnInternetReadFileEx = (MyInternetReadFileEx)GetProcAddress(hinstLib, "InternetReadFileExA");
	if (!lpfnInternetOpen || !lpfnInternetOpenUrl || !lpfnInternetCloseHandle || !lpfnInternetReadFileEx)
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);

	// Open the internet session
	HINTERNET hInet = lpfnInternetOpen(NULL, INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY, NULL, NULL, 0);
	if (!hInet)
	{
		FreeLibrary(hinstLib);
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}

	// Open the required URL
	HINTERNET hFile = lpfnInternetOpenUrl(hInet, aURL, NULL, 0, 0, 0);
	if (!hFile)
	{
		lpfnInternetCloseHandle(hInet);
		FreeLibrary(hinstLib);
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}

	// Open our output file
	FILE *fptr = fopen(aFilespec, "wb");	// Open in binary write/destroy mode
	if (!fptr)
	{
		lpfnInternetCloseHandle(hFile);
		lpfnInternetCloseHandle(hInet);
		FreeLibrary(hinstLib);
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}

	BYTE bufData[1024 * 8];
	INTERNET_BUFFERS buffers = {0};
	buffers.dwStructSize = sizeof(INTERNET_BUFFERS);
	buffers.lpvBuffer = bufData;
	buffers.dwBufferLength = sizeof(bufData);

	LONG_OPERATION_INIT

	// Read the file.  I don't think synchronous transfers typically generate the pseudo-error
	// ERROR_IO_PENDING, so that is not checked here.  That's probably just for async transfers.
	// IRF_NO_WAIT is used to avoid requiring the call to block until the buffer is full.  By
	// having it return the moment there is any data in the buffer, the program is made more
	// responsive, especially when the download is very slow and/or one of the hooks is installed:
	BOOL result;
	while (result = lpfnInternetReadFileEx(hFile, &buffers, IRF_NO_WAIT, NULL)) // Assign
	{
		if (!buffers.dwBufferLength) // Transfer is complete.
			break;
		LONG_OPERATION_UPDATE  // Done in between the net-read and the file-write to improve avg. responsiveness.
		fwrite(bufData, buffers.dwBufferLength, 1, fptr);
		buffers.dwBufferLength = sizeof(bufData);  // Reset buffer capacity for next iteration.
	}

	// Close internet session:
	lpfnInternetCloseHandle(hFile);
	lpfnInternetCloseHandle(hInet);
	FreeLibrary(hinstLib); // Only after the above.
	// Close output file:
	fclose(fptr);

	if (result)
		return g_ErrorLevel->Assign(ERRORLEVEL_NONE);  // Indicate success.
	else // An error occurred during the transfer.
	{
		DeleteFile(aFilespec);  // delete damaged/incomplete file
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}
}



int CALLBACK FileSelectFolderCallback(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData)
{
	if (uMsg == BFFM_INITIALIZED) // Caller has ensured that lpData isn't NULL by having set a valid lParam value.
		SendMessage(hwnd, BFFM_SETSELECTION, TRUE, lpData);
	// In spite of the quote below, the behavior does not seem to vary regardless of what value is returned
	// upon receipt of BFFM_VALIDATEFAILED, at least on XP.  But in case it matters on other OSes, preserve
	// compatibilty with versions older than 1.0.36.03 by keeping the dialog displayed even if the user enters
	// an invalid folder:
	// MSDN: "Returns zero except in the case of BFFM_VALIDATEFAILED. For that flag, returns zero to dismiss
	// the dialog or nonzero to keep the dialog displayed."
	return uMsg == BFFM_VALIDATEFAILED; // i.e. zero should be returned in almost every case.
}



ResultType Line::FileSelectFolder(char *aRootDir, char *aOptions, char *aGreeting)
{
	Var *output_var = ResolveVarOfArg(0);
	if (!output_var)
		return FAIL;

	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.
	if (!output_var->Assign())  // Initialize the output variable.
		return FAIL;

	if (g_nFolderDialogs >= MAX_FOLDERDIALOGS)
	{
		// Have a maximum to help prevent runaway hotkeys due to key-repeat feature, etc.
		MsgBox("The maximum number of Folder Dialogs has been reached." ERR_ABORT);
		return FAIL;
	}

	LPMALLOC pMalloc;
    if (SHGetMalloc(&pMalloc) != NOERROR)	// Initialize
		return OK;  // Let ErrorLevel tell the story.

	// v1.0.36.03: Support initial folder, which is different than the root folder because the root only
	// controls the origin point (above which the control cannot navigate).
	char *initial_folder;
	char root_dir[MAX_PATH*2 + 5];  // Up to two paths might be present inside, including an asterisk and spaces between them.
	strlcpy(root_dir, aRootDir, sizeof(root_dir)); // Make a modifiable copy.
	if (initial_folder = strchr(root_dir, '*'))
	{
		*initial_folder = '\0'; // Terminate so that root_dir becomes an isolated string.
		// Must eliminate the trailing whitespace or it won't work.  However, only up to one space or tab
		// so that path names that really do end in literal spaces can be used:
		if (initial_folder > root_dir && IS_SPACE_OR_TAB(initial_folder[-1]))
			initial_folder[-1] = '\0';
		// In case absolute paths can ever have literal leading whitespace, preserve that whitespace
		// by incremently by only one and not calling omit_leading_whitespace().  This has been documented.
		++initial_folder;
	}
	else
		initial_folder = NULL;
	if (!*(omit_leading_whitespace(root_dir))) // Count all-whitespace as a blank string, but retain leading whitespace if there is also non-whitespace inside.
		*root_dir = '\0';

	BROWSEINFO bi;
	if (initial_folder)
	{
		bi.lpfn = FileSelectFolderCallback;
		bi.lParam = (LPARAM)initial_folder;  // Used by the callback above.
	}
	else
		bi.lpfn = NULL;  // It will ignore the value of bi.lParam when lpfn is NULL.

	if (*root_dir)
	{
		IShellFolder *pDF;
		if (SHGetDesktopFolder(&pDF) == NOERROR)
		{
			LPITEMIDLIST pIdl = NULL;
			ULONG        chEaten;
			ULONG        dwAttributes;
			OLECHAR olePath[MAX_PATH];			// wide-char version of path name
			MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, root_dir, -1, olePath, sizeof(olePath));
			pDF->ParseDisplayName(NULL, NULL, olePath, &chEaten, &pIdl, &dwAttributes);
			pDF->Release();
			bi.pidlRoot = pIdl;
		}
	}
	else // No root directory.
		bi.pidlRoot = NULL;  // Make it use "My Computer" as the root dir.

	int iImage = 0;
	bi.iImage = iImage;
	bi.hwndOwner = THREAD_DIALOG_OWNER; // Can be NULL, which is used rather than main window since no need to have main window forced into the background by this.
	char greeting[1024];
	if (aGreeting && *aGreeting)
		strlcpy(greeting, aGreeting, sizeof(greeting));
	else
		snprintf(greeting, sizeof(greeting), "Select Folder - %s", g_script.mFileName);
	bi.lpszTitle = greeting;

	DWORD options = *aOptions ? ATOI(aOptions) : FSF_ALLOW_CREATE;
	bi.ulFlags = 0x0040 | ((options & FSF_ALLOW_CREATE) ? 0 : 0x200) | ((options & (DWORD)FSF_EDITBOX) ? BIF_EDITBOX : 0);

	char Result[2048];
	bi.pszDisplayName = Result;  // This will hold the user's choice.

	// At this point, we know a dialog will be displayed.  See macro's comments for details:
	DIALOG_PREP
	POST_AHK_DIALOG(0) // Do this only after the above.  Must pass 0 for timeout in this case.

	++g_nFolderDialogs;
	LPITEMIDLIST lpItemIDList = SHBrowseForFolder(&bi);  // Spawn Dialog
	--g_nFolderDialogs;

	DIALOG_END
	if (!lpItemIDList)
		return OK;  // Let ErrorLevel tell the story.

	*Result = '\0';  // Reuse this var, this time to old the result of the below:
	SHGetPathFromIDList(lpItemIDList, Result);
	pMalloc->Free(lpItemIDList);
	pMalloc->Release();

	g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
	return output_var->Assign(Result);
}



ResultType Line::FileGetShortcut(char *aShortcutFile) // Credited to Holger <Holger.Kotsch at GMX de>.
{
	// These might be omitted in the parameter list, so it's okay if they resolve to NULL.
	Var *output_var_target = ResolveVarOfArg(1);
	Var *output_var_dir = ResolveVarOfArg(2);
	Var *output_var_arg = ResolveVarOfArg(3);
	Var *output_var_desc = ResolveVarOfArg(4);
	Var *output_var_icon = ResolveVarOfArg(5);
	Var *output_var_icon_idx = ResolveVarOfArg(6);
	Var *output_var_show_state = ResolveVarOfArg(7);

	// For consistency with the behavior of other commands, the output variables are initialized to blank
	// so that there is another way to detect failure:
	if (output_var_target) output_var_target->Assign();
	if (output_var_dir) output_var_dir->Assign();
	if (output_var_arg) output_var_arg->Assign();
	if (output_var_desc) output_var_desc->Assign();
	if (output_var_icon) output_var_icon->Assign();
	if (output_var_icon_idx) output_var_icon_idx->Assign();
	if (output_var_show_state) output_var_show_state->Assign();

	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.

	if (!Util_DoesFileExist(aShortcutFile))
		return OK;  // Let ErrorLevel tell the story.

	CoInitialize(NULL);
	IShellLink *psl;

	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID *)&psl)))
	{
		IPersistFile *ppf;
		if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (LPVOID *)&ppf)))
		{
			WORD wsz[MAX_PATH+1];
			MultiByteToWideChar(CP_ACP, 0, aShortcutFile, -1, (LPWSTR)wsz, MAX_PATH);
			if (SUCCEEDED(ppf->Load((const WCHAR*)wsz, 0)))
			{
				char buf[MAX_PATH+1];
				int icon_index, show_cmd;

				if (output_var_target)
				{
					psl->GetPath(buf, MAX_PATH, NULL, SLGP_UNCPRIORITY);
					output_var_target->Assign(buf);
				}
				if (output_var_dir)
				{
					psl->GetWorkingDirectory(buf, MAX_PATH);
					output_var_dir->Assign(buf);
				}
				if (output_var_arg)
				{
					psl->GetArguments(buf, MAX_PATH);
					output_var_arg->Assign(buf);
				}
				if (output_var_desc)
				{
					psl->GetDescription(buf, MAX_PATH); // Testing shows that the OS limits it to 260 characters.
					output_var_desc->Assign(buf);
				}
				if (output_var_icon || output_var_icon_idx)
				{
					psl->GetIconLocation(buf, MAX_PATH, &icon_index);
					if (output_var_icon)
						output_var_icon->Assign(buf);
					if (output_var_icon_idx)
						if (*buf)
							output_var_icon_idx->Assign(icon_index + 1);  // Convert from 0-based to 1-based for consistency with the Menu command, etc.
						else
							output_var_icon_idx->Assign(); // Make it blank to indicate that there is none.
				}
				if (output_var_show_state)
				{
					psl->GetShowCmd(&show_cmd);
					output_var_show_state->Assign(show_cmd);
					// For the above, decided not to translate them to Max/Min/Normal since other
					// show-state numbers might be supported in the future (or are already).  In other
					// words, this allows the flexibilty to specify some number other than 1/3/7 when
					// creating the shortcut in case it happens to work.  Of course, that applies only
					// to FileCreateShortcut, not here.  But it's done here so that this command is
					// compatible with that one.
				}
				g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
			}
			ppf->Release();
		}
		psl->Release();
	}
	CoUninitialize();

	return OK;  // ErrorLevel might still indicate failture if one of the above calls failed.
}



ResultType Line::FileCreateShortcut(char *aTargetFile, char *aShortcutFile, char *aWorkingDir, char *aArgs
	, char *aDescription, char *aIconFile, char *aHotkey, char *aIconNumber, char *aRunState)
{
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.
	CoInitialize(NULL);
	IShellLink *psl;

	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID *)&psl)))
	{
		psl->SetPath(aTargetFile);
		if (*aWorkingDir)
			psl->SetWorkingDirectory(aWorkingDir);
		if (*aArgs)
			psl->SetArguments(aArgs);
		if (*aDescription)
			psl->SetDescription(aDescription);
		if (*aIconFile)
			psl->SetIconLocation(aIconFile, *aIconNumber ? ATOI(aIconNumber) - 1 : 0); // Doesn't seem necessary to validate aIconNumber as not being negative, etc.
		if (*aHotkey)
		{
			// If badly formatted, it's not a critical error, just continue.
			// Currently, only shortcuts with a CTRL+ALT are supported.
			// AutoIt3 note: Make sure that CTRL+ALT is selected (otherwise invalid)
			vk_type vk = TextToVK(aHotkey);
			if (vk)
				// Vk in low 8 bits, mods in high 8:
				psl->SetHotkey(   (WORD)vk | ((WORD)(HOTKEYF_CONTROL | HOTKEYF_ALT) << 8)   );
		}
		if (*aRunState)
			psl->SetShowCmd(ATOI(aRunState)); // No validation is done since there's a chance other numbers might be valid now or in the future.

		IPersistFile *ppf;
		if(SUCCEEDED(psl->QueryInterface(IID_IPersistFile,(LPVOID *)&ppf)))
		{
			WORD wsz[MAX_PATH];
			MultiByteToWideChar(CP_ACP, 0, aShortcutFile, -1, (LPWSTR)wsz, MAX_PATH);
			if (SUCCEEDED(ppf->Save((LPCWSTR)wsz, TRUE)))
				g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
			ppf->Release();
		}
		psl->Release();
	}

	CoUninitialize();
	return OK; // ErrorLevel indicates whether or not it succeeded.
}



ResultType Line::FileRecycle(char *aFilePattern)
{
	if (!aFilePattern || !*aFilePattern)
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);  // Since this is probably not what the user intended.

	SHFILEOPSTRUCT FileOp;
	char szFileTemp[_MAX_PATH+2];

	// au3: Get the fullpathname - required for UNDO to work
	Util_GetFullPathName(aFilePattern, szFileTemp);

	// au3: We must also make it a double nulled string *sigh*
	szFileTemp[strlen(szFileTemp)+1] = '\0';	

	// au3: set to known values - Corrects crash
	FileOp.hNameMappings = NULL;
	FileOp.lpszProgressTitle = NULL;
	FileOp.fAnyOperationsAborted = FALSE;
	FileOp.hwnd = NULL;
	FileOp.pTo = NULL;

	FileOp.pFrom = szFileTemp;
	FileOp.wFunc = FO_DELETE;
	FileOp.fFlags = FOF_SILENT | FOF_ALLOWUNDO | FOF_NOCONFIRMATION;

	// SHFileOperation() returns 0 on success:
	return g_ErrorLevel->Assign(SHFileOperation(&FileOp) ? ERRORLEVEL_ERROR : ERRORLEVEL_NONE);
}



ResultType Line::FileRecycleEmpty(char *aDriveLetter)
{
	// Not using GetModuleHandle() because there is doubt that SHELL32 (unlike USER32/KERNEL32), is
	// always automatically present in every process (e.g. if shell is something other than Explorer):
	HINSTANCE hinstLib = LoadLibrary("shell32");
	if (!hinstLib)
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	// au3: Get the address of all the functions we require
	typedef HRESULT (WINAPI *MySHEmptyRecycleBin)(HWND, LPCTSTR, DWORD);
 	MySHEmptyRecycleBin lpfnEmpty = (MySHEmptyRecycleBin)GetProcAddress(hinstLib, "SHEmptyRecycleBinA");
	if (!lpfnEmpty)
	{
		FreeLibrary(hinstLib);
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}
	const char *szPath = *aDriveLetter ? aDriveLetter : NULL;
	if (lpfnEmpty(NULL, szPath, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND) != S_OK)
	{
		FreeLibrary(hinstLib);
		return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}
	FreeLibrary(hinstLib);
	return g_ErrorLevel->Assign(ERRORLEVEL_NONE);
}



ResultType Line::FileGetVersion(char *aFilespec)
{
	Var *output_var = ResolveVarOfArg(0);
	if (!output_var)
		return FAIL;
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default
	output_var->Assign(); // Init to be blank, in case of failure.

	if (!aFilespec || !*aFilespec)
		return OK;  // Let ErrorLevel indicate an error, since this is probably not what the user intended.

	DWORD dwUnused, dwSize;
	if (   !(dwSize = GetFileVersionInfoSize(aFilespec, &dwUnused))   )
		return OK;  // Let ErrorLevel tell the story.

	BYTE *pInfo = (BYTE*)malloc(dwSize);  // Allocate the size retrieved by the above.

	// Read the version resource
	GetFileVersionInfo((LPSTR)aFilespec, 0, dwSize, (LPVOID)pInfo);

	// Locate the fixed information
	VS_FIXEDFILEINFO *pFFI;
	UINT uSize;
	if (!VerQueryValue(pInfo, "\\", (LPVOID *)&pFFI, &uSize))
	{
		free(pInfo);
		return OK;  // Let ErrorLevel tell the story.
	}

	// extract the fields you want from pFFI
	UINT iFileMS = (UINT)pFFI->dwFileVersionMS;
	UINT iFileLS = (UINT)pFFI->dwFileVersionLS;
	char version_string[128];  // AutoIt3: 43+1 is the maximum size, but leave a little room to increase confidence.
	snprintf(version_string, sizeof(version_string), "%u.%u.%u.%u"
		, (iFileMS >> 16), (iFileMS & 0xFFFF), (iFileLS >> 16), (iFileLS & 0xFFFF));

	free(pInfo);

    g_ErrorLevel->Assign(ERRORLEVEL_NONE);  // Indicate success.
	return output_var->Assign(version_string);
}



bool Line::Util_CopyDir (const char *szInputSource, const char *szInputDest, bool bOverwrite)
{
	SHFILEOPSTRUCT	FileOp;
	char			szSource[_MAX_PATH+2];
	char			szDest[_MAX_PATH+2];

	// Get the fullpathnames and strip trailing \s
	Util_GetFullPathName(szInputSource, szSource);
	Util_GetFullPathName(szInputDest, szDest);

	// Ensure source is a directory
	if (Util_IsDir(szSource) == false)
		return false;							// Nope

	// Does the destination dir exist?
	if (Util_IsDir(szDest))
	{
		if (bOverwrite == false)
			return false;
	}
	else
	{
		// We must create the top level directory
		if (!Util_CreateDir(szDest))
			return false;
	}

	// To work under old versions AND new version of shell32.dll the source must be specifed
	// as "dir\*.*" and the destination directory must already exist... Godamn Microsoft and their APIs...
	strcat(szSource, "\\*.*");

	// We must also make source\dest double nulled strings for the SHFileOp API
	szSource[strlen(szSource)+1] = '\0';	
	szDest[strlen(szDest)+1] = '\0';	

	// Setup the struct
	FileOp.pFrom					= szSource;
	FileOp.pTo						= szDest;
	FileOp.hNameMappings			= NULL;
	FileOp.lpszProgressTitle		= NULL;
	FileOp.fAnyOperationsAborted	= FALSE;
	FileOp.hwnd						= NULL;

	FileOp.wFunc	= FO_COPY;
	FileOp.fFlags	= FOF_SILENT | FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_NOERRORUI;

	if ( SHFileOperation(&FileOp) )
		return false;								

	return true;

}



bool Line::Util_MoveDir (const char *szInputSource, const char *szInputDest, int OverwriteMode)
{
	SHFILEOPSTRUCT	FileOp;
	char			szSource[_MAX_PATH+2];
	char			szDest[_MAX_PATH+2];

	// Get the fullpathnames and strip trailing \s
	Util_GetFullPathName(szInputSource, szSource);
	Util_GetFullPathName(szInputDest, szDest);

	// Ensure source is a directory
	if (Util_IsDir(szSource) == false)
		return false;							// Nope

	// Does the destination dir exist?
	bool dest_exists_as_dir = Util_IsDir(szDest);  // v1.0.38
	if (dest_exists_as_dir) // Untested: If a file vs. dir of this name exists, SHFileOperation() will fail later below.
	{
		if (OverwriteMode != 1 && OverwriteMode != 2) // Strict validation for safety.
			return false;
	}

	// Now, if the source and dest are on different volumes then we must copy rather than move
	// as move in this case only works on some OSes
	if (Util_IsDifferentVolumes(szSource, szDest))
	{
		// Copy and delete (poor man's move)
		if (Util_CopyDir(szSource, szDest, true) == false)
			return false;
		if (Util_RemoveDir(szSource, true) == false)
			return false;
		else
			return true;
	}

	// Since above didn't return, source and dest are on same volume.
	if (dest_exists_as_dir && OverwriteMode == 2) // v1.0.38: New behavior only when OverwriteMode==2 to preserve backward compatibility.
	{
		// Since szDest exists as a directory (not a file, since that should automatically produce
		// the expected failure in SHFileOperation()), alter szDest to be the parent directory
		// so that the operation overwrite the target directory rather than moving the source
		// directory into it.  Since szDest shouldn't be a root directory in this mode, it should
		// always have a parent directory.
		char *last_backslash = strrchr(szDest, '\\'); // Util_GetFullPathName() has already ensured that szDest doesn't end in a backslash.
		if (last_backslash)
			*last_backslash = '\0';
	}

	// We must also make source\dest double nulled strings for the SHFileOp API
	szSource[strlen(szSource)+1] = '\0';
	szDest[strlen(szDest)+1] = '\0';

	// Setup the struct
	FileOp.pFrom					= szSource;
	FileOp.pTo						= szDest;
	FileOp.hNameMappings			= NULL;
	FileOp.lpszProgressTitle		= NULL;
	FileOp.fAnyOperationsAborted	= FALSE;
	FileOp.hwnd						= NULL;

	FileOp.wFunc	= FO_MOVE;
	FileOp.fFlags	= FOF_SILENT | FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_NOERRORUI;

	return !SHFileOperation(&FileOp);

}



bool Line::Util_RemoveDir (const char *szInputSource, bool bRecurse)
{
	SHFILEOPSTRUCT	FileOp;
	char			szSource[_MAX_PATH+2];

	// Get the fullpathnames and strip trailing \s
	Util_GetFullPathName(szInputSource, szSource);

	// Ensure source is a directory
	if (Util_IsDir(szSource) == false)
		return false;							// Nope

	// If recursion not on just try a standard delete on the directory (the SHFile function WILL
	// delete a directory even if not empty no matter what flags you give it...)
	if (bRecurse == false)
	{
		if (!RemoveDirectory(szSource))
			return false;
		else
			return true;
	}

	// We must also make double nulled strings for the SHFileOp API
	szSource[strlen(szSource)+1] = '\0';	

	// Setup the struct
	FileOp.pFrom					= szSource;
	FileOp.pTo						= NULL;
	FileOp.hNameMappings			= NULL;
	FileOp.lpszProgressTitle		= NULL;
	FileOp.fAnyOperationsAborted	= FALSE;
	FileOp.hwnd						= NULL;

	FileOp.wFunc	= FO_DELETE;
	FileOp.fFlags	= FOF_SILENT | FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_NOERRORUI;
	
	return !SHFileOperation(&FileOp);
}



///////////////////////////////////////////////////////////////////////////////
// Util_CopyFile()
// (moves files too)
// Returns the number of files that could not be copied or moved due to error.
///////////////////////////////////////////////////////////////////////////////
int Line::Util_CopyFile(const char *szInputSource, const char *szInputDest, bool bOverwrite, bool bMove)
{
	char			szSource[_MAX_PATH+1];
	char			szDest[_MAX_PATH+1];
	char			szExpandedDest[MAX_PATH+1];
	char			szTempPath[_MAX_PATH+1];
	char			szDrive[_MAX_PATH+1];
	char			szDir[_MAX_PATH+1];
	char			szFile[_MAX_PATH+1];
	char			szExt[_MAX_PATH+1];

	// Get local version of our source/dest with full path names, strip trailing \s
	Util_GetFullPathName(szInputSource, szSource);
	Util_GetFullPathName(szInputDest, szDest);

	// If the source or dest is a directory then add *.* to the end
	if (Util_IsDir(szSource))
		strcat(szSource, "\\*.*");
	if (Util_IsDir(szDest))
		strcat(szDest, "\\*.*");

	// Split source into file and extension (we need this info in the loop below to recontstruct the path)
	_splitpath( szSource, szDrive, szDir, szFile, szExt );

	// Note we now rely on the SOURCE being the contents of szDrive, szDir, szFile, etc.

	LONG_OPERATION_INIT

	bool is_found;
	int failure_count;
	WIN32_FIND_DATA	findData;
	HANDLE hSearch = FindFirstFile(szSource, &findData);

	for (failure_count = 0, is_found = (hSearch != INVALID_HANDLE_VALUE)
		; is_found
		; is_found = FindNextFile(hSearch, &findData))
	{
		LONG_OPERATION_UPDATE

		// Make sure the returned handle is a file and not a directory before we
		// try and do copy type things on it!
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) // dwFileAttributes should never be invalid (0xFFFFFFFF) in this case.
			continue;

		// Expand the destination based on this found file
		Util_ExpandFilenameWildcard(findData.cFileName, szDest, szExpandedDest);
	
		// The find struct only returns the file NAME, we need to reconstruct the path!
		strcpy(szTempPath, szDrive);	
		strcat(szTempPath, szDir);
		strcat(szTempPath, findData.cFileName);

		// Fixed for v1.0.36.01: This section has been revised to avoid unnecessary calls; but more
		// importantly, it now avoids the deletion and complete loss of a file when it is copied or
		// moved onto itself.  That used to happen because any existing destination file used to be
		// deleted prior to attempting the move/copy.
		if (bMove)  // Move vs. copy mode.
		{
			// Note that MoveFile() is capable of moving a file to a different volume, regardless of
			// operating system version.  That's enough for what we need because this function never
			// moves directories, only files.

			// The following call will report success if source and dest are the same file, even if
			// source is something like "..\Folder\Filename.txt" and dest is something like
			// "C:\Folder\Filename.txt" (or if source is an 8.3 filename and dest is the long name
			// of the same file).  This is good because it avoids the need to devise code
			// to determine whether two different path names refer to the same physical file
			// (note that GetFullPathName() has shown itself to be inadequate for this purpose due
			// to problems with short vs. long names, UNC vs. mapped drive, and possibly NTFS hard
			// links (aliases) that might all cause two different filenames to point to the same
			// physical file on disk (hopefully MoveFile handles all of these correctly by indicating
			// success [below] when a file is moved onto itself, though it has only been tested for
			// basic cases of relative vs. absolute path).
			if (!MoveFile(szTempPath, szExpandedDest))
			{
				// If overwrite mode was not specified by the caller, or it was but the existing
				// destination file cannot be deleted (perhaps because it is a folder rather than
				// a file), or it can be deleted but the source cannot be moved, indicate a failure.
				// But by design, continue the operation.  The following relies heavily on
				// short-circuit boolean evaluation order:
				if (   !(bOverwrite && DeleteFile(szExpandedDest) && MoveFile(szTempPath, szExpandedDest))   )
					++failure_count; // At this stage, any of the above 3 being false is cause for failure.
				//else everything succeeded, so nothing extra needs to be done.  In either case,
				// continue on to the next file.
			}
		}
		else // The mode is "Copy" vs. "Move"
			if (!CopyFile(szTempPath, szExpandedDest, !bOverwrite)) // Force it to fail if bOverwrite==false.
				++failure_count;
	} // for() each file.

	FindClose(hSearch);
	return failure_count;

}



void Line::Util_ExpandFilenameWildcard(const char *szSource, const char *szDest, char *szExpandedDest)
{
	// copy one.two.three  *.txt     = one.two   .txt
	// copy one.two.three  *.*.txt   = one.two   .three  .txt
	// copy one.two.three  *.*.*.txt = one.two   .three  ..txt
	// copy one.two		   test      = test

	char	szFileTemp[_MAX_PATH+1];
	char	szExtTemp[_MAX_PATH+1];

	char	szSrcFile[_MAX_PATH+1];
	char	szSrcExt[_MAX_PATH+1];

	char	szDestDrive[_MAX_PATH+1];
	char	szDestDir[_MAX_PATH+1];
	char	szDestFile[_MAX_PATH+1];
	char	szDestExt[_MAX_PATH+1];

	// If the destination doesn't include a wildcard, send it back vertabim
	if (strchr(szDest, '*') == NULL)
	{
		strcpy(szExpandedDest, szDest);
		return;
	}

	// Split source and dest into file and extension
	_splitpath( szSource, szDestDrive, szDestDir, szSrcFile, szSrcExt );
	_splitpath( szDest, szDestDrive, szDestDir, szDestFile, szDestExt );

	// Source and Dest ext will either be ".nnnn" or "" or ".*", remove the period
	if (szSrcExt[0] == '.')
		strcpy(szSrcExt, &szSrcExt[1]);
	if (szDestExt[0] == '.')
		strcpy(szDestExt, &szDestExt[1]);

	// Start of the destination with the drive and dir
	strcpy(szExpandedDest, szDestDrive);
	strcat(szExpandedDest, szDestDir);

	// Replace first * in the destext with the srcext, remove any other *
	Util_ExpandFilenameWildcardPart(szSrcExt, szDestExt, szExtTemp);

	// Replace first * in the destfile with the srcfile, remove any other *
	Util_ExpandFilenameWildcardPart(szSrcFile, szDestFile, szFileTemp);

	// Concat the filename and extension if req
	if (szExtTemp[0] != '\0')
	{
		strcat(szFileTemp, ".");
		strcat(szFileTemp, szExtTemp);	
	}
	else
	{
		// Dest extension was blank SOURCE MIGHT NOT HAVE BEEN!
		if (szSrcExt[0] != '\0')
		{
			strcat(szFileTemp, ".");
			strcat(szFileTemp, szSrcExt);	
		}
	}

	// Now add the drive and directory bit back onto the dest
	strcat(szExpandedDest, szFileTemp);

}



void Line::Util_ExpandFilenameWildcardPart(const char *szSource, const char *szDest, char *szExpandedDest)
{
	char	*lpTemp;
	int		i, j, k;

	// Replace first * in the dest with the src, remove any other *
	i = 0; j = 0; k = 0;
	lpTemp = strchr(szDest, '*');
	if (lpTemp != NULL)
	{
		// Contains at least one *, copy up to this point
		while(szDest[i] != '*')
			szExpandedDest[j++] = szDest[i++];
		// Skip the * and replace in the dest with the srcext
		while(szSource[k] != '\0')
			szExpandedDest[j++] = szSource[k++];
		// Skip any other *
		i++;
		while(szDest[i] != '\0')
		{
			if (szDest[i] == '*')
				i++;
			else
				szExpandedDest[j++] = szDest[i++];
		}
		szExpandedDest[j] = '\0';
	}
	else
	{
		// No wildcard, straight copy of destext
		strcpy(szExpandedDest, szDest);
	}
}



bool Line::Util_CreateDir(const char *szDirName) // Recursive directory creation function.
{
	DWORD	dwTemp;
	bool	bRes;
	char	*szTemp = NULL;
	char	*psz_Loc = NULL;

	dwTemp = GetFileAttributes(szDirName);

	if (dwTemp == 0xffffffff) 
	{	// error getting attribute - what was the error?
		switch (GetLastError())
		{
		case ERROR_PATH_NOT_FOUND:
			// Create path
			szTemp = new char[strlen(szDirName)+1];
			strcpy(szTemp, szDirName);
			psz_Loc = strrchr(szTemp, '\\');	/* find last \ */
			if (psz_Loc == NULL)				// not found
			{
				delete [] szTemp;
				return false;
			}
			else 
			{
				*psz_Loc = '\0';				// remove \ and everything after
				bRes = Util_CreateDir(szTemp);
				delete [] szTemp;
				if (bRes)
				{
					if (CreateDirectory(szDirName, NULL))
						bRes = true;
					else
						bRes = false;
				}

				return bRes;
			}
			// All paths above "return".
		case ERROR_FILE_NOT_FOUND:
			// Create directory
			return CreateDirectory(szDirName, NULL);
		// Otherwise, it's some unforeseen error, so fall through to the end, which reports failure.
		} // switch()
	}
	else // The specified name already exists as a file or directory.
		if (dwTemp & FILE_ATTRIBUTE_DIRECTORY) // Fixed for v1.0.36.01 (previously it used == vs &).
			return true;							// Directory exists, yay!
		//else it exists, but it's a file! Not allowed, so fall through and report failure.
			
	return false;

}



bool Line::Util_DoesFileExist(const char *szFilename)  // Returns true if file or directory exists.
{
	if ( strchr(szFilename,'*')||strchr(szFilename,'?') )
	{
		WIN32_FIND_DATA	wfd;
		HANDLE			hFile;

	    hFile = FindFirstFile(szFilename, &wfd);

		if ( hFile == INVALID_HANDLE_VALUE )
			return false;

		FindClose(hFile);
		return true;
	}
    else
	{
		DWORD dwTemp;

		dwTemp = GetFileAttributes(szFilename);
		if ( dwTemp != 0xffffffff )
			return true;
		else
			return false;
	}
}



bool Line::Util_IsDir(const char *szPath) // Returns true if the path is a directory
{
	DWORD dwTemp = GetFileAttributes(szPath);
	return dwTemp != 0xffffffff && (dwTemp & FILE_ATTRIBUTE_DIRECTORY);
}



void Line::Util_GetFullPathName(const char *szIn, char *szOut)
// Returns the full pathname and strips any trailing \s.  Assumes output is _MAX_PATH in size.
{
	char	*szFilePart;
	GetFullPathName(szIn, _MAX_PATH, szOut, &szFilePart);
	Util_StripTrailingDir(szOut);
}



void Line::Util_StripTrailingDir(char *szPath)
// Makes sure a filename does not have a trailing //
{
	size_t index = strlen(szPath) - 1;
	if (szPath[index] == '\\')
		szPath[index] = '\0';
}



bool Line::Util_IsDifferentVolumes(const char *szPath1, const char *szPath2)
// Checks two paths to see if they are on the same volume.
{
	char			szP1Drive[_MAX_DRIVE+1];
	char			szP2Drive[_MAX_DRIVE+1];

	char			szDir[_MAX_DIR+1];
	char			szFile[_MAX_FNAME+1];
	char			szExt[_MAX_EXT+1];
	
	char			szP1[_MAX_PATH+1];	
	char			szP2[_MAX_PATH+1];

	// Get full pathnames
	Util_GetFullPathName(szPath1, szP1);
	Util_GetFullPathName(szPath2, szP2);

	// Split the target into bits
	_splitpath( szP1, szP1Drive, szDir, szFile, szExt );
	_splitpath( szP2, szP2Drive, szDir, szFile, szExt );

	if (szP1Drive[0] == '\0' || szP2Drive[0] == '\0')
		// One or both paths is a UNC - assume different volumes
		return true;
	else
		return stricmp(szP1Drive, szP2Drive);
}



bool Util_Shutdown(int nFlag)
// Shutdown or logoff the system.
// Returns false if the function could not get the rights to shutdown.
{
/* 
flags can be a combination of:
#define EWX_LOGOFF           0
#define EWX_SHUTDOWN         0x00000001
#define EWX_REBOOT           0x00000002
#define EWX_FORCE            0x00000004
#define EWX_POWEROFF         0x00000008 */

	HANDLE				hToken; 
	TOKEN_PRIVILEGES	tkp; 

	// If we are running NT/2k/XP, make sure we have rights to shutdown
	if (g_os.IsWinNT()) // NT/2k/XP/2003 and family
	{
		// Get a token for this process.
 		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) 
			return false;						// Don't have the rights
 
		// Get the LUID for the shutdown privilege.
 		LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid); 
 
		tkp.PrivilegeCount = 1;  /* one privilege to set */
		tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; 
 
		// Get the shutdown privilege for this process.
 		AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, (PTOKEN_PRIVILEGES)NULL, 0); 
 
		// Cannot test the return value of AdjustTokenPrivileges.
 		if (GetLastError() != ERROR_SUCCESS) 
			return false;						// Don't have the rights
	}

	// if we are forcing the issue, AND this is 95/98 terminate all windows first
	if ( g_os.IsWin9x() && (nFlag & EWX_FORCE) ) 
	{
		nFlag ^= EWX_FORCE;	// remove this flag - not valid in 95
		EnumWindows((WNDENUMPROC) Util_ShutdownHandler, 0);
	}

	// ExitWindows
	if (ExitWindowsEx(nFlag, 0))
		return true;
	else
		return false;

}



BOOL Util_ShutdownHandler(HWND hwnd, DWORD lParam)
{
	// if the window is me, don't terminate!
	if (hwnd != g_hWnd && hwnd != g_hWndSplash)
		Util_WinKill(hwnd);

	// Continue the enumeration.
	return TRUE;

}



void Util_WinKill(HWND hWnd)
{
	DWORD dwResult;
	// Use WM_CLOSE vs. SC_CLOSE in this case, since the target window is slightly more likely to
	// respond to that:
	if (!SendMessageTimeout(hWnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 500, &dwResult)) // Wait up to 500ms.
	{
		// Use more force - Mwuahaha
		DWORD pid = GetWindowThreadProcessId(hWnd, NULL);
		HANDLE hProcess = pid ? OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid) : NULL;
		if (hProcess)
		{
			TerminateProcess(hProcess, 0);
			CloseHandle(hProcess);
		}
	}
}



////////////////////
// PROCESS ROUTINES
////////////////////

DWORD ProcessExist9x2000(char *aProcess, char *aProcessName)
{
	if (aProcessName) // Init this output variable in case of early return.
		*aProcessName = '\0';

	// We must dynamically load the function or program will probably not launch at all on NT4.
	typedef BOOL (WINAPI *PROCESSWALK)(HANDLE hSnapshot, LPPROCESSENTRY32 lppe);
	typedef HANDLE (WINAPI *CREATESNAPSHOT)(DWORD dwFlags, DWORD th32ProcessID);

	static CREATESNAPSHOT lpfnCreateToolhelp32Snapshot = (CREATESNAPSHOT)GetProcAddress(GetModuleHandle("kernel32"), "CreateToolhelp32Snapshot");
    static PROCESSWALK lpfnProcess32First = (PROCESSWALK)GetProcAddress(GetModuleHandle("kernel32"), "Process32First");
    static PROCESSWALK lpfnProcess32Next = (PROCESSWALK)GetProcAddress(GetModuleHandle("kernel32"), "Process32Next");

	if (!lpfnCreateToolhelp32Snapshot || !lpfnProcess32First || !lpfnProcess32Next)
		return 0;

	PROCESSENTRY32 proc;
    proc.dwSize = sizeof(proc);
	HANDLE snapshot = lpfnCreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	lpfnProcess32First(snapshot, &proc);

	// Determine the PID if aProcess is a pure, non-negative integer (any negative number
	// is more likely to be the name of a process [with a leading dash], rather than the PID).
	DWORD specified_pid = IsPureNumeric(aProcess) ? ATOU(aProcess) : 0;
	char szDrive[_MAX_PATH+1], szDir[_MAX_PATH+1], szFile[_MAX_PATH+1], szExt[_MAX_PATH+1];

	while (lpfnProcess32Next(snapshot, &proc))
	{
		if (specified_pid && specified_pid == proc.th32ProcessID)
		{
			if (aProcessName) // Caller wanted process name also.
			{
				// For consistency in results, use _splitpath() both here and below rather than
				// something that just checks for a rightmost backslash.
				_splitpath(proc.szExeFile, szDrive, szDir, aProcessName, szExt);
				strcat(aProcessName, szExt);
			}
			CloseHandle(snapshot);
			return specified_pid;
		}
		// Otherwise, check for matching name even if aProcess is purely numeric (i.e. a number might
		// also be a valid name?):
		// It seems that proc.szExeFile never contains a path, just the executable name.
		// But in case it ever does, ensure consistency by removing the path:
		_splitpath(proc.szExeFile, szDrive, szDir, szFile, szExt);
		strcat(szFile, szExt);
		if (!stricmp(szFile, aProcess))
		{
			if (aProcessName) // Caller wanted process name also.
				strcpy(aProcessName, szFile);
			CloseHandle(snapshot);
			return proc.th32ProcessID;
		}
	}
	CloseHandle(snapshot);
	return 0;  // Not found.
}



DWORD ProcessExistNT4(char *aProcess, char *aProcessName)
{
	if (aProcessName) // Init this output variable in case of early return.
		*aProcessName = '\0';
	//BOOL EnumProcesses(
	//  DWORD *lpidProcess,  // array of process identifiers
	//  DWORD cb,            // size of array
	//  DWORD *cbNeeded      // number of bytes returned
	//);
	typedef BOOL (WINAPI *MyEnumProcesses)(DWORD*, DWORD, DWORD*);

	//BOOL EnumProcessModules(
	//  HANDLE hProcess,      // handle to process
	//  HMODULE *lphModule,   // array of module handles
	//  DWORD cb,             // size of array
	//  LPDWORD lpcbNeeded    // number of bytes required
	//);
	typedef BOOL (WINAPI *MyEnumProcessModules)(HANDLE, HMODULE*, DWORD, LPDWORD);

	//DWORD GetModuleBaseName(
	//  HANDLE hProcess,    // handle to process
	//  HMODULE hModule,    // handle to module
	//  LPTSTR lpBaseName,  // base name buffer
	//  DWORD nSize         // maximum characters to retrieve
	//);
	typedef DWORD (WINAPI *MyGetModuleBaseName)(HANDLE, HMODULE, LPTSTR, DWORD);

	// We must dynamically load the function or program will probably not launch at all on Win95.
    // Get a handle to the DLL module that contains EnumProcesses
	HINSTANCE hinstLib = LoadLibrary("psapi");
	if (!hinstLib)
		return 0;

	// Not static in this case, since address can change with each new load of the library:
  	MyEnumProcesses lpfnEnumProcesses = (MyEnumProcesses)GetProcAddress(hinstLib, "EnumProcesses");
	MyEnumProcessModules lpfnEnumProcessModules = (MyEnumProcessModules)GetProcAddress(hinstLib, "EnumProcessModules");
	MyGetModuleBaseName lpfnGetModuleBaseName = (MyGetModuleBaseName)GetProcAddress(hinstLib, "GetModuleBaseNameA");

	DWORD idProcessArray[512];		// 512 processes max
	DWORD cbNeeded;					// Bytes returned
	if (!lpfnEnumProcesses || !lpfnEnumProcessModules || !lpfnGetModuleBaseName
		|| !lpfnEnumProcesses(idProcessArray, sizeof(idProcessArray), &cbNeeded))
	{
		FreeLibrary(hinstLib);
		return 0;
	}

	// Get the count of PIDs in the array
	DWORD cProcesses = cbNeeded / sizeof(DWORD);
	// Determine the PID if aProcess is a pure, non-negative integer (any negative number
	// is more likely to be the name of a process [with a leading dash], rather than the PID).
	DWORD specified_pid = IsPureNumeric(aProcess) ? ATOU(aProcess) : 0;
	char szDrive[_MAX_PATH+1], szDir[_MAX_PATH+1], szFile[_MAX_PATH+1], szExt[_MAX_PATH+1];
	char szProcessName[_MAX_PATH+1];
	HMODULE hMod;
	HANDLE hProcess;

	for (UINT i = 0; i < cProcesses; ++i)
	{
		if (specified_pid && specified_pid == idProcessArray[i])
		{
			if (aProcessName) // Caller wanted process name also.
			{
				if (hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, idProcessArray[i])) // Assign
				{
					lpfnEnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded);
					if (lpfnGetModuleBaseName(hProcess, hMod, szProcessName, _MAX_PATH))
					{
						// For consistency in results, use _splitpath() both here and below rather than
						// something that just checks for a rightmost backslash.
						_splitpath(szProcessName, szDrive, szDir, aProcessName, szExt);
						strcat(aProcessName, szExt);
					}
					CloseHandle(hProcess);
				}
			}
			FreeLibrary(hinstLib);
			return specified_pid;
		}
		// Otherwise, check for matching name even if aProcess is purely numeric (i.e. a number might
		// also be a valid name?):
		if (hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, idProcessArray[i])) // Assign
		{
			lpfnEnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded);
			if (lpfnGetModuleBaseName(hProcess, hMod, szProcessName, _MAX_PATH))
			{
				_splitpath(szProcessName, szDrive, szDir, szFile, szExt);
				strcat(szFile, szExt);
				if (!stricmp(szFile, aProcess))
				{
					if (aProcessName) // Caller wanted process name also.
						strcpy(aProcessName, szProcessName);
					CloseHandle(hProcess);
					FreeLibrary(hinstLib);
					return idProcessArray[i];  // The PID.
				}
			}
			CloseHandle(hProcess);
		}
	}
	FreeLibrary(hinstLib);
	return 0;  // Not found.
}