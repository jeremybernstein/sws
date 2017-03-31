/******************************************************************************
/ SnM_Notes.cpp
/
/ Copyright (c) 2010 and later Jeffos
/
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/ 
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/ 
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/

//JFB
// - if REAPER >= v4.55pre2 use MarkProjectDirty(), otherwise creates undo point for each key stroke (!) to fix:
//   * SaveExtensionConfig() that is not called when there is no proj mods but some notes have been entered..
//   * missing updates on project switches
// - OSX: no action help support (SWELL's GetPrivateProfileSection assumes key=value)
// - clicking the empty area of the tcp does not remove focus (important for refresh)
// - undo does not restore caret position
// TODO?
// - take changed => title not updated
// - drag-drop text?
// - use action_help_t? (not finished according to Cockos)
// - handle concurent item/project notes updates?


#include "stdafx.h"
#include "SnM.h"
#include "SnM_Dlg.h"
#include "SnM_Notes.h"
#include "SnM_Project.h"
#include "SnM_Track.h"
#include "SnM_Util.h"
#include "SnM_Window.h"
#include "../reaper/localize.h"
#include "WDL/projectcontext.h"


#define NOTES_WND_ID				"SnMNotesHelp"
#define NOTES_INI_SEC				"Notes"
#define MAX_HELP_LENGTH				(64*1024) //JFB! instead of MAX_INI_SECTION (too large)
#define UPDATE_TIMER				1

enum {
#ifdef WANT_ACTION_HELP
  SET_ACTION_HELP_FILE_MSG = 0xF001,
  WRAP_MSG,
#else
  WRAP_MSG = 0xF001,
#endif
  LAST_MSG // keep as last item!
};

enum {
  BTNID_LOCK=LAST_MSG,
  CMBID_TYPE,
  TXTID_LABEL,
#ifdef WANT_ACTION_HELP
  BTNID_ALR,
  BTNID_ACTIONLIST,
#endif
  BTNID_IMPORT_SUB,
  BTNID_EXPORT_SUB,
  TXTID_BIG_NOTES
};

enum {
  REQUEST_REFRESH=0,
  NO_REFRESH
};


SNM_WindowManager<NotesWnd> g_notesWndMgr(NOTES_WND_ID);

SWSProjConfig<WDL_PtrList_DOD<SNM_TrackNotes> > g_SNM_TrackNotes;
SWSProjConfig<WDL_PtrList_DOD<SNM_RegionSubtitle> > g_pRegionSubs; // for markers too..
SWSProjConfig<WDL_FastString> g_prjNotes; // extra project notes

int g_notesType = -1;
int g_prevNotesType = -1;
bool g_locked = false;
char g_lastText[MAX_HELP_LENGTH] = "";
char g_lastImportSubFn[SNM_MAX_PATH] = "";
char g_lastExportSubFn[SNM_MAX_PATH] = "";
char g_actionHelpFn[SNM_MAX_PATH] = "";
char g_notesBigFontName[64] = SNM_DYN_FONT_NAME;
bool g_wrapText=false;

// used for action help updates tracking
#ifdef WANT_ACTION_HELP
//JFB TODO: cleanup when we'll be able to access all sections & custom ids
int g_lastActionListSel = -1;
int g_lastActionListCmd = 0;
char g_lastActionSection[SNM_MAX_SECTION_NAME_LEN] = "";
char g_lastActionCustId[SNM_MAX_ACTION_CUSTID_LEN] = "";
char g_lastActionDesc[SNM_MAX_ACTION_NAME_LEN] = "";
#endif

// other vars for updates tracking
double g_lastMarkerPos = -1.0;
int g_lastMarkerRegionId = -1;
MediaItem* g_mediaItemNote = NULL;
MediaTrack* g_trNote = NULL;

// to distinguish internal marker/region updates from external ones
bool g_internalMkrRgnChange = false;


///////////////////////////////////////////////////////////////////////////////
// NotesWnd
///////////////////////////////////////////////////////////////////////////////

// S&M windows lazy init: below's "" prevents registering the SWS' screenset callback
// (use the S&M one instead - already registered via SNM_WindowManager::Init())
NotesWnd::NotesWnd()
	: SWS_DockWnd(IDD_SNM_NOTES, __LOCALIZE("Notes","sws_DLG_152"), "", SWSGetCommandID(OpenNotes))
{
	m_id.Set(NOTES_WND_ID);
	// must call SWS_DockWnd::Init() to restore parameters and open the window if necessary
	Init();
}

NotesWnd::~NotesWnd()
{
}

void NotesWnd::OnInitDlg()
{
	m_resize.init_item(IDC_EDIT, 0.0, 0.0, 1.0, 1.0);
	SetWindowLongPtr(GetDlgItem(m_hwnd, IDC_EDIT), GWLP_USERDATA, 0xdeadf00b);
	SetWrapText(g_wrapText, true);

	LICE_CachedFont* font = SNM_GetThemeFont();

	m_vwnd_painter.SetGSC(WDL_STYLE_GetSysColor);
	m_parentVwnd.SetRealParent(m_hwnd);

	m_btnLock.SetID(BTNID_LOCK);
	m_parentVwnd.AddChild(&m_btnLock);

	m_cbType.SetID(CMBID_TYPE);
	m_cbType.SetFont(font);
	m_cbType.AddItem(__LOCALIZE("Track notes","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Item notes","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Project notes","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Extra project notes","sws_DLG_152"));
	m_cbType.AddItem("<SEP>");
	m_cbType.AddItem(__LOCALIZE("Marker names","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Region names","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Marker/Region names","sws_DLG_152"));
	m_cbType.AddItem("<SEP>");
	m_cbType.AddItem(__LOCALIZE("Marker subtitles","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Region subtitles","sws_DLG_152"));
	m_cbType.AddItem(__LOCALIZE("Marker/Region subtitles","sws_DLG_152"));
#if defined(_WIN32) && defined(WANT_ACTION_HELP)
	m_cbType.AddItem("<SEP>");
	m_cbType.AddItem(__LOCALIZE("Action help","sws_DLG_152"));
#endif
	m_parentVwnd.AddChild(&m_cbType);
	// ...the selected item is set through SetType() below

	m_txtLabel.SetID(TXTID_LABEL);
	m_txtLabel.SetFont(font);
	m_parentVwnd.AddChild(&m_txtLabel);
#ifdef WANT_ACTION_HELP
	m_btnAlr.SetID(BTNID_ALR);
	m_parentVwnd.AddChild(&m_btnAlr);

	m_btnActionList.SetID(BTNID_ACTIONLIST);
	m_parentVwnd.AddChild(&m_btnActionList);
#endif

	m_btnImportSub.SetID(BTNID_IMPORT_SUB);
	m_parentVwnd.AddChild(&m_btnImportSub);

	m_btnExportSub.SetID(BTNID_EXPORT_SUB);
	m_parentVwnd.AddChild(&m_btnExportSub);

	m_bigNotes.SetID(TXTID_BIG_NOTES);
	m_bigNotes.SetFontName(g_notesBigFontName);
	m_parentVwnd.AddChild(&m_bigNotes);

	g_prevNotesType = -1; // will force refresh
	SetType(BOUNDED(g_notesType, 0, m_cbType.GetCount()-1)); // + Update()

/* see OnTimer()
	RegisterToMarkerRegionUpdates(&m_mkrRgnListener);
*/
	SetTimer(m_hwnd, UPDATE_TIMER, NOTES_UPDATE_FREQ, NULL);
}

void NotesWnd::OnDestroy() 
{
	KillTimer(m_hwnd, UPDATE_TIMER);
/* see OnTimer()
	UnregisterToMarkerRegionUpdates(&m_mkrRgnListener);
*/
	g_prevNotesType = -1;
	m_cbType.Empty();
}

// note: no diff with current type, init would fail otherwise
void NotesWnd::SetType(int _type)
{
	g_notesType = _type;
	m_cbType.SetCurSel2(g_notesType);
	SendMessage(m_hwnd, WM_SIZE, 0, 0); // to update the bottom of the GUI

	// force an initial refresh (when IDC_EDIT has the focus, re-enabling the timer 
	// isn't enough: Update() is skipped, see OnTimer() & IsActive()
	Update();
}

void NotesWnd::SetText(const char* _str, bool _addRN) {
	if (_str) {
		if (_addRN) GetStringWithRN(_str, g_lastText, sizeof(g_lastText));
		else lstrcpyn(g_lastText, _str, sizeof(g_lastText));
		SetDlgItemText(m_hwnd, IDC_EDIT, g_lastText);
	}
}

void NotesWnd::RefreshGUI() 
{
	bool bHide = true;
	switch(g_notesType)
	{
		case SNM_NOTES_PROJECT:
		case SNM_NOTES_PROJECT_EXTRA:
			bHide = false;
			break;
		case SNM_NOTES_ITEM:
			if (g_mediaItemNote)
				bHide = false;
			break;
		case SNM_NOTES_TRACK:
			if (g_trNote)
				bHide = false;
			break;
		case SNM_NOTES_MKR_NAME:
		case SNM_NOTES_RGN_NAME:
		case SNM_NOTES_MKRRGN_NAME:
		case SNM_NOTES_MKR_SUB:
		case SNM_NOTES_RGN_SUB:
		case SNM_NOTES_MKRRGN_SUB:
			if (g_lastMarkerRegionId > 0)
				bHide = false;
			break;
#ifdef WANT_ACTION_HELP
		case SNM_NOTES_ACTION_HELP:
			if (g_lastActionListSel >= 0)
				bHide = false; // for copy/paste even if "Custom: ..."
			break;
#endif
	}
	ShowWindow(GetDlgItem(m_hwnd, IDC_EDIT), bHide || g_locked ? SW_HIDE : SW_SHOW);
	m_parentVwnd.RequestRedraw(NULL); // the meat!
}


#ifdef _WIN32

typedef void (*FNCHANGESTYLE)(long&,long&);
HWND replacecontrol(HWND h,const int idc,FNCHANGESTYLE fnchange)
{
  HWND      hwnd;
  HWND      prev;
  RECT      rc;
  POINT      pt = {0,0};
  HINSTANCE  hinst;
  long      style;
  long      exstyle;
  TCHAR      text[256];
  TCHAR      scls[256];
  HFONT      hfont;
  int        focus;

  hwnd    = GetDlgItem(h,idc);

  // control not exist
  if(!IsWindow(hwnd)) return 0;

  prev    = GetWindow(hwnd,GW_HWNDPREV);
  hinst   = (HINSTANCE)GetWindowLongPtr(hwnd,GWLP_HINSTANCE);
  style   = GetWindowLong(hwnd,GWL_STYLE);
  exstyle = GetWindowLong(hwnd,GWL_EXSTYLE);
  focus   = hwnd == GetFocus();
  hfont   = (HFONT)SendMessage(hwnd,WM_GETFONT,0,0);

  ClientToScreen(h,&pt);
  GetWindowRect(hwnd,&rc);
  GetClassName(hwnd,scls,sizeof(scls)/sizeof(scls[0]));
  GetWindowText(hwnd,text,sizeof(text)/sizeof(text[0]));

  DestroyWindow(hwnd);
  fnchange(style,exstyle);

  hwnd = CreateWindowEx
  (
    exstyle,
    scls,
    text,
    style,
    rc.left-pt.x,
    rc.top-pt.y,
    rc.right-rc.left,
    rc.bottom-rc.top,
    h,
    (HMENU)idc,
    hinst,
    0
  );

  SendMessage(hwnd,WM_SETFONT,(WPARAM)hfont,1);
  SetWindowPos(hwnd,prev,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  if(focus) SetFocus(hwnd);
  return hwnd;
}

void modES_AUTOHSCROLL(long& style,long& exstyle)
{
  if (g_wrapText) style &= ~ES_AUTOHSCROLL;
  else style |= ES_AUTOHSCROLL;
}

#endif


void NotesWnd::SetWrapText(bool _wrap, bool _isInit)
{
  g_wrapText = _wrap;
  SWS_ShowTextScrollbar(GetDlgItem(m_hwnd, IDC_EDIT), !g_wrapText);
#ifdef _WIN32
  RECT r1 = m_resize.get_item(IDC_EDIT)->real_orig;
  RECT r2 = m_resize.get_item(IDC_EDIT)->orig;
  m_resize.remove_item(IDC_EDIT);
  replacecontrol(m_hwnd,IDC_EDIT,modES_AUTOHSCROLL);
  m_resize.init_item(IDC_EDIT, 0.0, 0.0, 1.0, 1.0);
  m_resize.get_item(IDC_EDIT)->real_orig = r1;
  m_resize.get_item(IDC_EDIT)->orig = r2;
  SetWindowLongPtr(GetDlgItem(m_hwnd, IDC_EDIT), GWLP_USERDATA, 0xdeadf00b);
#else
  if (!_isInit)
  {
    SendMessage(m_hwnd, WM_COMMAND, IDCANCEL, 0);
    ScheduledJob::Schedule(new ReopenNotesJob());
  }
#endif
}

void NotesWnd::OnCommand(WPARAM wParam, LPARAM lParam)
{
	switch (LOWORD(wParam))
	{
		case IDC_EDIT:
			if (HIWORD(wParam)==EN_CHANGE)
				SaveCurrentText(g_notesType, MarkProjectDirty==NULL); // MarkProjectDirty() avail. since v4.55pre2
			break;
#ifdef WANT_ACTION_HELP
		case SET_ACTION_HELP_FILE_MSG:
			SetActionHelpFilename(NULL);
			break;
#endif
		case WRAP_MSG:
			SetWrapText(!g_wrapText);
			break;
		case BTNID_LOCK:
			if (!HIWORD(wParam))
				ToggleLock();
			break;
#ifdef WANT_ACTION_HELP
		case BTNID_ALR:
			if (!HIWORD(wParam) && *g_lastActionCustId && !IsMacroOrScript(g_lastActionDesc))
			{
				char link[256] = "";
				char sectionURL[SNM_MAX_SECTION_NAME_LEN] = "";
				if (GetSectionURL(true, g_lastActionSection, sectionURL, SNM_MAX_SECTION_NAME_LEN))
					if (_snprintfStrict(link, sizeof(link), "http://www.cockos.com/wiki/index.php/%s_%s", sectionURL, g_lastActionCustId) > 0)
						ShellExecute(m_hwnd, "open", link , NULL, NULL, SW_SHOWNORMAL);
			}
			else
				ShellExecute(m_hwnd, "open", "http://wiki.cockos.com/wiki/index.php/Action_List_Reference" , NULL, NULL, SW_SHOWNORMAL);
			break;
		case BTNID_ACTIONLIST:
			Main_OnCommand(40605, 0);
			break;
#endif
		case BTNID_IMPORT_SUB:
			ImportSubTitleFile(NULL);
			break;
		case BTNID_EXPORT_SUB:
			ExportSubTitleFile(NULL);
			break;
		case CMBID_TYPE:
			if (HIWORD(wParam)==CBN_SELCHANGE) {
				SetType(m_cbType.GetCurSel2());
				if (!g_locked)
					SetFocus(GetDlgItem(m_hwnd, IDC_EDIT));
			}
			break;
		default:
			Main_OnCommand((int)wParam, (int)lParam);
			break;
	}
}

// bWantEdit ignored: no list view in there
bool NotesWnd::IsActive(bool bWantEdit) {
	return (IsValidWindow() && (GetForegroundWindow() == m_hwnd || GetFocus() == GetDlgItem(m_hwnd, IDC_EDIT)));
}

HMENU NotesWnd::OnContextMenu(int x, int y, bool* wantDefaultItems)
{
  HMENU hMenu = CreatePopupMenu();
  AddToMenu(hMenu, __LOCALIZE("Wrap text","sws_DLG_152"), WRAP_MSG, -1, false, g_wrapText ? MFS_CHECKED : MFS_UNCHECKED);
#ifdef WANT_ACTION_HELP
	if (g_notesType == SNM_NOTES_ACTION_HELP)
	{
		AddToMenu(hMenu, __LOCALIZE("Set action help file...","sws_DLG_152"), SET_ACTION_HELP_FILE_MSG);
	}
#endif
  return hMenu;
}

// OSX fix/workaround (SWELL bug?)
#ifdef _SNM_SWELL_ISSUES
void OSXForceTxtChangeJob::Perform() {
	if (NotesWnd* w = g_notesWndMgr.Get())
		SendMessage(w->GetHWND(), WM_COMMAND, MAKEWPARAM(IDC_EDIT, EN_CHANGE), 0);
}
#endif

void ReopenNotesJob::Perform() {
	if (NotesWnd* w = g_notesWndMgr.Get()) w->Show(false, true);
}


// returns: 
// -1 = catch and send to the control 
//  0 = pass-thru to main window (then -666 in SWS_DockWnd::keyHandler())
//  1 = eat
int NotesWnd::OnKey(MSG* _msg, int _iKeyState)
{
	HWND h = GetDlgItem(m_hwnd, IDC_EDIT);
/*JFB not needed: IDC_EDIT is the single control of this window..
#ifdef _WIN32
	if (_msg->hwnd == h)
#else
	if (GetFocus() == h)
#endif
*/
	{
		if (g_locked)
		{
			_msg->hwnd = m_hwnd; // redirect to main window
			return 0;
		}
		else if (_msg->message == WM_KEYDOWN || _msg->message == WM_CHAR)
		{
			// ctrl+A => select all
			if (_msg->wParam == 'A' && _iKeyState == LVKF_CONTROL)
			{
				SetFocus(h);
				SendMessage(h, EM_SETSEL, 0, -1);
				return 1;
			}
			else
#ifndef _SNM_SWELL_ISSUES
			if (_msg->wParam == VK_RETURN)
				return -1; // send the return key to the edit control
#else
			// fix/workaround (SWELL bug?): EN_CHANGE is not always sent...
			{
				ScheduledJob::Schedule(new OSXForceTxtChangeJob());
				return -1; // send the return key to the edit control
			}
#endif
		}
	}
	return 0; 
}

void NotesWnd::OnTimer(WPARAM wParam)
{
	if (wParam == UPDATE_TIMER)
	{
		// register to marker and region updates only when needed (less stress for REAPER)
		if (g_notesType>=SNM_NOTES_MKR_NAME && g_notesType<=SNM_NOTES_MKRRGN_SUB)
			RegisterToMarkerRegionUpdates(&m_mkrRgnListener); // no-op if alreday registered
		else
			UnregisterToMarkerRegionUpdates(&m_mkrRgnListener);

		// no update when editing text or when the view is hidden (e.g. inactive docker tab).
		// when the view is active: update only for markers and if the view is locked 
		// => updates during playback, in other cases (e.g. item selection change) the main 
		// window will be the active one, not our NotesWnd
		if (g_notesType!=SNM_NOTES_PROJECT && g_notesType!=SNM_NOTES_PROJECT_EXTRA && 
		    IsWindowVisible(m_hwnd) && (!IsActive() || (g_locked && (g_notesType>=SNM_NOTES_MKR_NAME && g_notesType<=SNM_NOTES_MKRRGN_SUB))))
		{
			Update();
		}
	}
}

void NotesWnd::OnResize()
{
	if (g_notesType != g_prevNotesType)
	{
		// room for buttons?
		if (
#ifdef WANT_ACTION_HELP
        g_notesType==SNM_NOTES_ACTION_HELP || 
#endif
        (g_notesType>=SNM_NOTES_MKR_SUB && g_notesType<=SNM_NOTES_MKRRGN_SUB))
			m_resize.get_item(IDC_EDIT)->orig.bottom = m_resize.get_item(IDC_EDIT)->real_orig.bottom - 41; //JFB!! 41 is tied to the current .rc!
		else
			m_resize.get_item(IDC_EDIT)->orig = m_resize.get_item(IDC_EDIT)->real_orig;
		InvalidateRect(m_hwnd, NULL, 0);
	}
}

void NotesWnd::DrawControls(LICE_IBitmap* _bm, const RECT* _r, int* _tooltipHeight)
{
	int h=SNM_GUI_TOP_H;
	if (_tooltipHeight)
		*_tooltipHeight = h;

	// "big" notes (dynamic font size)
	// drawn first so that it is displayed even with tiny sizing..
	if (g_locked)
	{
		// work on a copy rather than g_lastText (will get modified)
		char buf[MAX_HELP_LENGTH] = "";
		GetDlgItemText(m_hwnd, IDC_EDIT, buf, MAX_HELP_LENGTH);
		if (*buf)
		{
			if (g_notesType>=SNM_NOTES_MKR_NAME && g_notesType<=SNM_NOTES_MKRRGN_NAME)
				ShortenStringToFirstRN(buf, true);

			RECT r = *_r;
			r.top += h;
			m_bigNotes.SetPosition(&r);
			m_bigNotes.SetText(buf);
			m_bigNotes.SetVisible(true);
		}
	}
	// clear last "big notes"
	else
		LICE_FillRect(_bm,0,h,_bm->getWidth(),_bm->getHeight()-h,WDL_STYLE_GetSysColor(COLOR_WINDOW),0.0,LICE_BLIT_MODE_COPY);


	// 1st row of controls

	IconTheme* it = SNM_GetIconTheme();
	int x0=_r->left+SNM_GUI_X_MARGIN;

	SNM_SkinButton(&m_btnLock, it ? &it->toolbar_lock[!g_locked] : NULL, g_locked ? __LOCALIZE("Unlock","sws_DLG_152") : __LOCALIZE("Lock","sws_DLG_152"));
	if (SNM_AutoVWndPosition(DT_LEFT, &m_btnLock, NULL, _r, &x0, _r->top, h))
	{
		if (SNM_AutoVWndPosition(DT_LEFT, &m_cbType, NULL, _r, &x0, _r->top, h))
		{
			char str[512];
			lstrcpyn(str, __LOCALIZE("No selection!","sws_DLG_152"), sizeof(str));
			switch(g_notesType)
			{
				case SNM_NOTES_PROJECT:
				case SNM_NOTES_PROJECT_EXTRA:
				{
					char buf[SNM_MAX_PATH];
					EnumProjects(-1, buf, sizeof(buf));
					lstrcpyn(str, GetFilenameWithExt(buf), sizeof(str));
					break;
				}
				case SNM_NOTES_ITEM:
					if (g_mediaItemNote)
					{
						MediaItem_Take* tk = GetActiveTake(g_mediaItemNote);
						char* tkName= tk ? (char*)GetSetMediaItemTakeInfo(tk, "P_NAME", NULL) : NULL;
						lstrcpyn(str, tkName?tkName:"", sizeof(str));
					}
					break;
				case SNM_NOTES_TRACK:
					if (g_trNote)
					{
						int id = CSurf_TrackToID(g_trNote, false);
						if (id > 0) {
							char* trName = (char*)GetSetMediaTrackInfo(g_trNote, "P_NAME", NULL);
							_snprintfSafe(str, sizeof(str), "[%d] \"%s\"", id, trName?trName:"");
						}
						else if (id == 0)
							strcpy(str, __LOCALIZE("[MASTER]","sws_DLG_152"));
					}
					break;

				case SNM_NOTES_MKR_NAME:
				case SNM_NOTES_MKR_SUB:
					if (g_lastMarkerRegionId <= 0 || EnumMarkerRegionDescById(NULL, g_lastMarkerRegionId, str, sizeof(str), SNM_MARKER_MASK, true, g_notesType==SNM_NOTES_MKR_SUB, true)<0 || !*str)
						lstrcpyn(str, __LOCALIZE("No marker at play/edit cursor!","sws_DLG_152"), sizeof(str));
					break;
				case SNM_NOTES_RGN_NAME:
				case SNM_NOTES_RGN_SUB:
					if (g_lastMarkerRegionId <= 0 || EnumMarkerRegionDescById(NULL, g_lastMarkerRegionId, str, sizeof(str), SNM_REGION_MASK, true, g_notesType==SNM_NOTES_RGN_SUB, true)<0 || !*str)
						lstrcpyn(str, __LOCALIZE("No region at play/edit cursor!","sws_DLG_152"), sizeof(str));
					break;
				case SNM_NOTES_MKRRGN_NAME:
				case SNM_NOTES_MKRRGN_SUB:
					if (g_lastMarkerRegionId <= 0 || EnumMarkerRegionDescById(NULL, g_lastMarkerRegionId, str, sizeof(str), SNM_MARKER_MASK|SNM_REGION_MASK, true, g_notesType==SNM_NOTES_MKRRGN_SUB, true)<0 || !*str)
						lstrcpyn(str, __LOCALIZE("No marker or region at play/edit cursor!","sws_DLG_152"), sizeof(str));
					break;
#ifdef WANT_ACTION_HELP
				case SNM_NOTES_ACTION_HELP:
					if (*g_lastActionDesc && *g_lastActionSection)
						_snprintfSafe(str, sizeof(str), " [%s] %s", g_lastActionSection, g_lastActionDesc);
/*JFB!!! API LIMITATION: use smthg like that when we will be able to access all sections
						lstrcpyn(str, kbd_getTextFromCmd(g_lastActionListCmd, NULL), 512);
*/
					break;
#endif
			}

			m_txtLabel.SetText(str);
			if (SNM_AutoVWndPosition(DT_LEFT, &m_txtLabel, NULL, _r, &x0, _r->top, h))
				SNM_AddLogo(_bm, _r, x0, h);
		}
	}


	// 2nd row of controls
	if (g_locked)
		return;

	x0 = _r->left+SNM_GUI_X_MARGIN; h=SNM_GUI_BOT_H;
	int y0 = _r->bottom-h;

	// import/export buttons
	if (g_notesType>=SNM_NOTES_MKR_SUB && g_notesType<=SNM_NOTES_MKRRGN_SUB)
	{
		SNM_SkinToolbarButton(&m_btnImportSub, __LOCALIZE("Import...","sws_DLG_152"));
		if (!SNM_AutoVWndPosition(DT_LEFT, &m_btnImportSub, NULL, _r, &x0, y0, h, 4))
			return;

		SNM_SkinToolbarButton(&m_btnExportSub, __LOCALIZE("Export...","sws_DLG_152"));
		if (!SNM_AutoVWndPosition(DT_LEFT, &m_btnExportSub, NULL, _r, &x0, y0, h))
			return;
	}

#ifdef WANT_ACTION_HELP
	// online help & action list buttons
	if (g_notesType == SNM_NOTES_ACTION_HELP)
	{
		SNM_SkinToolbarButton(&m_btnActionList, __LOCALIZE("Action list...","sws_DLG_152"));
		if (!SNM_AutoVWndPosition(DT_LEFT, &m_btnActionList, NULL, _r, &x0, y0, h, 4))
			return;

		SNM_SkinToolbarButton(&m_btnAlr, __LOCALIZE("Online help...","sws_DLG_152"));
		if (!SNM_AutoVWndPosition(DT_LEFT, &m_btnAlr, NULL, _r, &x0, y0, h))
			return;
	}
#endif
}

bool NotesWnd::GetToolTipString(int _xpos, int _ypos, char* _bufOut, int _bufOutSz)
{
	if (WDL_VWnd* v = m_parentVwnd.VirtWndFromPoint(_xpos,_ypos,1))
	{
		switch (v->GetID())
		{
			case BTNID_LOCK:
				lstrcpyn(_bufOut, g_locked ? __LOCALIZE("Text locked ('Big font' mode)", "sws_DLG_152") : __LOCALIZE("Text unlocked", "sws_DLG_152"), _bufOutSz);
				return true;
			case CMBID_TYPE:
				lstrcpyn(_bufOut, __LOCALIZE("Notes type","sws_DLG_152"), _bufOutSz);
				return true;
			case TXTID_LABEL:
				lstrcpyn(_bufOut, ((WDL_VirtualStaticText*)v)->GetText(), _bufOutSz);
				return true;
		}
	}
	return false;
}

void NotesWnd::ToggleLock()
{
	g_locked = !g_locked;
	RefreshToolbar(SWSGetCommandID(ToggleNotesLock));
	if (g_notesType>=SNM_NOTES_MKR_NAME && g_notesType<=SNM_NOTES_MKRRGN_SUB)
		Update(true); // play vs edit cursor when unlocking
	else
		RefreshGUI();

	if (!g_locked)
		SetFocus(GetDlgItem(m_hwnd, IDC_EDIT));
}


///////////////////////////////////////////////////////////////////////////////

void NotesWnd::SaveCurrentText(int _type, bool _wantUndo) 
{
	switch(_type) 
	{
		case SNM_NOTES_PROJECT:
			SaveCurrentProjectNotes(_wantUndo);
			break;
		case SNM_NOTES_PROJECT_EXTRA:
			SaveCurrentExtraProjectNotes(_wantUndo);
			break;
		case SNM_NOTES_ITEM: 
			SaveCurrentItemNotes(_wantUndo); 
			break;
		case SNM_NOTES_TRACK:
			SaveCurrentTrackNotes(_wantUndo);
			break;
		case SNM_NOTES_MKR_NAME:
		case SNM_NOTES_RGN_NAME:
		case SNM_NOTES_MKRRGN_NAME:
		case SNM_NOTES_MKR_SUB:
		case SNM_NOTES_RGN_SUB:
		case SNM_NOTES_MKRRGN_SUB:
			SaveCurrentMkrRgnNameOrSub(_type, _wantUndo);
			break;
#ifdef WANT_ACTION_HELP
		case SNM_NOTES_ACTION_HELP:
			SaveCurrentHelp();
			break;
#endif
	}
}

void NotesWnd::SaveCurrentProjectNotes(bool _wantUndo)
{
	GetDlgItemText(m_hwnd, IDC_EDIT, g_lastText, sizeof(g_lastText));
	GetSetProjectNotes(NULL, true, g_lastText, sizeof(g_lastText));
/* project notes are out of the undo system's scope, MarkProjectDirty is the best thing we can do...
	if (_wantUndo)
		Undo_OnStateChangeEx2(NULL, __LOCALIZE("Edit project notes","sws_undo"), UNDO_STATE_ALL, -1);
	else
*/
	if (MarkProjectDirty)
		MarkProjectDirty(NULL);
}

void NotesWnd::SaveCurrentExtraProjectNotes(bool _wantUndo)
{
	GetDlgItemText(m_hwnd, IDC_EDIT, g_lastText, sizeof(g_lastText));
	g_prjNotes.Get()->Set(g_lastText); // CRLF removed only when saving the project..
	if (_wantUndo)
		Undo_OnStateChangeEx2(NULL, __LOCALIZE("Edit exta project notes","sws_undo"), UNDO_STATE_MISCCFG, -1);
	else
		MarkProjectDirty(NULL);
}

void NotesWnd::SaveCurrentItemNotes(bool _wantUndo)
{
	if (g_mediaItemNote && GetMediaItem_Track(g_mediaItemNote))
	{
		GetDlgItemText(m_hwnd, IDC_EDIT, g_lastText, sizeof(g_lastText));
		if (GetSetMediaItemInfo(g_mediaItemNote, "P_NOTES", g_lastText))
		{
//				UpdateItemInProject(g_mediaItemNote);
			UpdateTimeline(); // for the item's note button 
			if (_wantUndo)
				Undo_OnStateChangeEx2(NULL, __LOCALIZE("Edit item notes","sws_undo"), UNDO_STATE_ALL, -1); //JFB TODO? -1 to replace? UNDO_STATE_ITEMS?
			else
				MarkProjectDirty(NULL);
		}
	}
}

void NotesWnd::SaveCurrentTrackNotes(bool _wantUndo)
{
	if (g_trNote && CSurf_TrackToID(g_trNote, false) >= 0)
	{
		GetDlgItemText(m_hwnd, IDC_EDIT, g_lastText, sizeof(g_lastText));
		bool found = false;
		for (int i=0; i < g_SNM_TrackNotes.Get()->GetSize(); i++) 
		{
			if (g_SNM_TrackNotes.Get()->Get(i)->GetTrack() == g_trNote)
			{
				g_SNM_TrackNotes.Get()->Get(i)->m_notes.Set(g_lastText); // CRLF removed only when saving the project..
				found = true;
				break;
			}
		}
		if (!found)
			g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(TrackToGuid(g_trNote), g_lastText));
		if (_wantUndo)
			Undo_OnStateChangeEx2(NULL, __LOCALIZE("Edit track notes","sws_undo"), UNDO_STATE_MISCCFG, -1); //JFB TODO? -1 to replace?
		else
			MarkProjectDirty(NULL);
	}
}

void NotesWnd::SaveCurrentMkrRgnNameOrSub(int _type, bool _wantUndo)
{
	if (g_lastMarkerRegionId > 0)
	{
		GetDlgItemText(m_hwnd, IDC_EDIT, g_lastText, sizeof(g_lastText));

		// set marker/region name?
		if (_type>=SNM_NOTES_MKR_NAME && _type<=SNM_NOTES_MKRRGN_NAME)
		{
			double pos, end; int num, color; bool isRgn;
			if (EnumMarkerRegionById(NULL, g_lastMarkerRegionId, &isRgn, &pos, &end, NULL, &num, &color)>=0)
			{
				ShortenStringToFirstRN(g_lastText);

				// track marker/region name update => reentrant notif
				// via SNM_MarkerRegionListener.NotifyMarkerRegionUpdate()
				g_internalMkrRgnChange = true;

				if (SetProjectMarker4(NULL, num, isRgn, pos, end, g_lastText, color, !*g_lastText ? 1 : 0))
				{
					UpdateTimeline();
					if (_wantUndo)
						Undo_OnStateChangeEx2(NULL, isRgn ? __LOCALIZE("Edit region name","sws_undo") : __LOCALIZE("Edit marker name","sws_undo"), UNDO_STATE_ALL, -1);
					else
						MarkProjectDirty(NULL);
				}
			}
		}
		// subtitles
		else
		{
			// CRLF removed only when saving the project..
			bool found = false;
			for (int i=0; i < g_pRegionSubs.Get()->GetSize(); i++) 
			{
				if (g_pRegionSubs.Get()->Get(i)->m_id == g_lastMarkerRegionId) {
					g_pRegionSubs.Get()->Get(i)->m_notes.Set(g_lastText);
					found = true;
					break;
				}
			}
			if (!found)
				g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(g_lastMarkerRegionId, g_lastText));
			if (_wantUndo)
				Undo_OnStateChangeEx2(NULL, IsRegion(g_lastMarkerRegionId) ? __LOCALIZE("Edit region subtitle","sws_undo") : __LOCALIZE("Edit marker subtitle","sws_undo"), UNDO_STATE_MISCCFG, -1);
			else
				MarkProjectDirty(NULL);
		}
	}
}

#ifdef WANT_ACTION_HELP
void NotesWnd::SaveCurrentHelp()
{
	if (*g_lastActionCustId) {
		GetDlgItemText(m_hwnd, IDC_EDIT, g_lastText, sizeof(g_lastText));
		SaveHelp(g_lastActionCustId, g_lastText);
	}
}
#endif

///////////////////////////////////////////////////////////////////////////////

void NotesWnd::Update(bool _force)
{
	static bool sRecurseCheck = false;
	if (sRecurseCheck)
		return;

	sRecurseCheck = true;

	// force refresh if needed
	if (_force || g_notesType != g_prevNotesType)
	{
		g_prevNotesType = g_notesType;
#ifdef WANT_ACTION_HELP
		g_lastActionListSel = -1;
		*g_lastActionCustId = '\0';
		*g_lastActionDesc = '\0';
		g_lastActionListCmd = 0;
		*g_lastActionSection = '\0';
#endif
		g_mediaItemNote = NULL;
		g_trNote = NULL;
		g_lastMarkerPos = -1.0;
		g_lastMarkerRegionId = -1;
		_force = true; // trick for RefreshGUI() below..
	}

	// update
	int refreshType = NO_REFRESH;
	switch(g_notesType)
	{
		case SNM_NOTES_PROJECT:
		{
			char buf[MAX_HELP_LENGTH];
			GetSetProjectNotes(NULL, false, buf, sizeof(buf));
			SetText(buf);
			refreshType = REQUEST_REFRESH;
			break;
		}
		case SNM_NOTES_PROJECT_EXTRA:
			SetText(g_prjNotes.Get()->Get());
			refreshType = REQUEST_REFRESH;
			break;
		case SNM_NOTES_ITEM:
			refreshType = UpdateItemNotes();
			break;
		case SNM_NOTES_TRACK:
			refreshType = UpdateTrackNotes();
			break;
		case SNM_NOTES_MKR_NAME:
		case SNM_NOTES_RGN_NAME:
		case SNM_NOTES_MKRRGN_NAME:
		case SNM_NOTES_MKR_SUB:
		case SNM_NOTES_RGN_SUB:
		case SNM_NOTES_MKRRGN_SUB:
			refreshType = UpdateMkrRgnNameOrSub(g_notesType);
			break;
#ifdef WANT_ACTION_HELP
		case SNM_NOTES_ACTION_HELP:
			refreshType = UpdateActionHelp();
			break;
#endif
	}
	
	if (_force || refreshType == REQUEST_REFRESH)
		RefreshGUI();

	sRecurseCheck = false;
}

#ifdef WANT_ACTION_HELP
int NotesWnd::UpdateActionHelp()
{
	int refreshType = NO_REFRESH;
	int iSel = GetSelectedAction(
		g_lastActionSection, SNM_MAX_SECTION_NAME_LEN, &g_lastActionListCmd, 
		g_lastActionCustId, SNM_MAX_ACTION_CUSTID_LEN, 
		g_lastActionDesc, SNM_MAX_ACTION_NAME_LEN);

	if (iSel >= 0)
	{
		if (iSel != g_lastActionListSel)
		{
			g_lastActionListSel = iSel;
			if (*g_lastActionCustId && *g_lastActionDesc)
			{
				char buf[MAX_HELP_LENGTH] = "";
				LoadHelp(g_lastActionCustId, buf, MAX_HELP_LENGTH);
				SetText(buf);
				refreshType = REQUEST_REFRESH;
			}
		}
	}
	else if (g_lastActionListSel>=0 || *g_lastText) // *g_lastText: when switching note types w/o nothing selected for the new type
	{
		g_lastActionListSel = -1;
		*g_lastActionCustId = '\0';
		*g_lastActionDesc = '\0';
		g_lastActionListCmd = 0;
		*g_lastActionSection = '\0';
		SetText("");
		refreshType = REQUEST_REFRESH;
	}
	return refreshType;
}
#endif

int NotesWnd::UpdateItemNotes()
{
	int refreshType = NO_REFRESH;
	if (MediaItem* selItem = GetSelectedMediaItem(NULL, 0))
	{
		if (selItem != g_mediaItemNote)
		{
			g_mediaItemNote = selItem;
			if (char* notes = (char*)GetSetMediaItemInfo(g_mediaItemNote, "P_NOTES", NULL))
				SetText(notes, false);
			refreshType = REQUEST_REFRESH;
		} 
	}
	else if (g_mediaItemNote || *g_lastText)
	{
		g_mediaItemNote = NULL;
		SetText("");
		refreshType = REQUEST_REFRESH;
	}
	return refreshType;
}

int NotesWnd::UpdateTrackNotes()
{
	int refreshType = NO_REFRESH;
	if (MediaTrack* selTr = SNM_GetSelectedTrack(NULL, 0, true))
	{
		if (selTr != g_trNote)
		{
			g_trNote = selTr;

			for (int i=0; i < g_SNM_TrackNotes.Get()->GetSize(); i++)
				if (g_SNM_TrackNotes.Get()->Get(i)->GetTrack() == g_trNote) {
					SetText(g_SNM_TrackNotes.Get()->Get(i)->m_notes.Get());
					return REQUEST_REFRESH;
				}

			g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(TrackToGuid(g_trNote), ""));
			SetText("");
			refreshType = REQUEST_REFRESH;
		} 
	}
	else if (g_trNote || *g_lastText)
	{
		g_trNote = NULL;
		SetText("");
		refreshType = REQUEST_REFRESH;
	}
	return refreshType;
}

int NotesWnd::UpdateMkrRgnNameOrSub(int _type)
{
	int refreshType = NO_REFRESH;

	double dPos=GetCursorPositionEx(NULL), accuracy=SNM_FUDGE_FACTOR;
	if (g_locked && GetPlayStateEx(NULL)) { // playing & update required?
		dPos = GetPlayPositionEx(NULL);
		accuracy = 0.1; // do not stress REAPER
	}

	if (fabs(g_lastMarkerPos-dPos) > accuracy)
	{
		g_lastMarkerPos = dPos;
		int mask = 0;
		if (_type!=SNM_NOTES_MKR_NAME && _type!=SNM_NOTES_MKR_SUB)
			mask |= SNM_REGION_MASK;
		if (_type!=SNM_NOTES_RGN_NAME && _type!=SNM_NOTES_RGN_SUB)
			mask |= SNM_MARKER_MASK;

		int id, idx = FindMarkerRegion(NULL, dPos, mask, &id);
		if (id > 0)
		{
			if (id != g_lastMarkerRegionId)
			{
				g_lastMarkerRegionId = id;

				// update name?
				if (_type>=SNM_NOTES_MKR_NAME && _type<=SNM_NOTES_MKRRGN_NAME)
				{
					const char* name = NULL;
					EnumProjectMarkers2(NULL, idx, NULL, NULL, NULL, &name, NULL);
					SetText(name ? name : "");
				}
				else // update subtitle
				{
					for (int i=0; i < g_pRegionSubs.Get()->GetSize(); i++)
						if (g_pRegionSubs.Get()->Get(i)->m_id == id) {
							SetText(g_pRegionSubs.Get()->Get(i)->m_notes.Get());
							return REQUEST_REFRESH;
						}
					g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(id, ""));
					SetText("");
				}
				refreshType = REQUEST_REFRESH;
			}
		}
		else if (g_lastMarkerRegionId>0 || *g_lastText)
		{
			g_lastMarkerPos = -1.0;
			g_lastMarkerRegionId = -1;
			SetText("");
			refreshType = REQUEST_REFRESH;
		}
	}
	return refreshType;
}


///////////////////////////////////////////////////////////////////////////////

#ifdef WANT_ACTION_HELP
// load/save action help (from/to ini file)
// note: WDL's cfg_encode_textblock() & decode_textblock() will not help here..

void LoadHelp(const char* _cmdName, char* _buf, int _bufSize)
{
	if (_cmdName && *_cmdName && _buf && _bufSize>0)
	{
		// "buf" filled with null-terminated strings, double null-terminated
		char buf[MAX_HELP_LENGTH] = "";
		if (/*int sz = */ GetPrivateProfileSection(_cmdName,buf,sizeof(buf),g_actionHelpFn))
		{
//			memset(_buf, 0, _bufSize);

			// GetPrivateProfileSection() sucks: "sz" does not include the very final '\0' (why not)
			// *but* it does not include the final "\0\0" when it truncates either (inconsistent)
			// => do not use "sz"

			int i=-1, j=0;
			while (++i<sizeof(buf) && j<_bufSize)
			{
				if (!buf[i])
				{
					if ((i+1)>=sizeof(buf) || !buf[i+1]) // check double null-terminated
						break;
					_buf[j++] = '\n'; // done this way for ascendant compatibilty
				}
				else if (buf[i] != '|')
					_buf[j++] = buf[i];
			}
			_buf[j<_bufSize?j:_bufSize-1] = '\0';
		}
	}
}

// adds '|' at start of lines, empty lines would be
// scratched by GetPrivateProfileSection() otherwise
//JFB!!! todo [section_custId]
void SaveHelp(const char* _cmdName, const char* _help)
{
	if (_cmdName && *_cmdName && _help)
	{
		char buf[MAX_HELP_LENGTH] = "";
//		memset(buf, 0, MAX_HELP_LENGTH);
		if (*_help)
		{
			*buf = '|';
			int i=-1;
			int j=1; // 1 because '|' was inserted above
			while (_help[++i] && j<sizeof(buf))
			{
				if (_help[i] != '\r')
				{
					buf[j++] = _help[i];
					if (_help[i] == '\n' && j<sizeof(buf))
						buf[j++] = '|';
				}
			}
			j = j>MAX_HELP_LENGTH-2 ? MAX_HELP_LENGTH-2 : j; // if truncated, clamp for double null-termination
			buf[j] = '\0';
			buf[j+1] = '\0';
		}
		WritePrivateProfileStruct(_cmdName, NULL, NULL, 0, g_actionHelpFn); // flush section
		WritePrivateProfileSection(_cmdName, buf, g_actionHelpFn);
	}
}

void SetActionHelpFilename(COMMAND_T*) {
	if (char* fn = BrowseForFiles(__LOCALIZE("S&M - Set action help file","sws_DLG_152"), g_actionHelpFn, NULL, false, SNM_INI_EXT_LIST)) {
		lstrcpyn(g_actionHelpFn, fn, sizeof(g_actionHelpFn));
		free(fn);
	}
}
#endif

///////////////////////////////////////////////////////////////////////////////
// Encode/decode notes (to/from RPP format)
// WDL's cfg_encode_textblock() & cfg_decode_textblock() would not help here..
///////////////////////////////////////////////////////////////////////////////

bool GetStringFromNotesChunk(WDL_FastString* _notesIn, char* _bufOut, int _bufOutSz)
{
	if (!_bufOut || !_notesIn)
		return false;

	memset(_bufOut, 0, _bufOutSz);
	const char* pNotes = _notesIn->Get();
	if (pNotes && *pNotes)
	{
		// find 1st '|'
		int i=0; while (pNotes[i] && pNotes[i] != '|') i++;
		if (pNotes[i]) i++;
		else return true;
	
		int j=0;
		while (pNotes[i] && j < _bufOutSz)
		{
			if (pNotes[i] != '\r' && pNotes[i] != '\n')
			{
        _bufOut[j++] = (pNotes[i]=='|'&&pNotes[i-1]=='\n' ? '\n' : pNotes[i]); // i is >0 here
			}
			i++;
		}
		if (j>=1 && !strcmp(_bufOut+j-1, ">")) // remove trailing ">", if any
			_bufOut[j-1] = '\0';
	}
	return true;
}

bool GetNotesChunkFromString(const char* _bufIn, WDL_FastString* _notesOut, const char* _startLine)
{
	if (_notesOut && _bufIn)
	{
		if (!_startLine) _notesOut->Set("<NOTES\n|");
		else _notesOut->Set(_startLine);

		int i=0;
		while (_bufIn[i])
		{
			if (_bufIn[i] == '\n') 
				_notesOut->Append("\n|");
			else if (_bufIn[i] != '\r') 
				_notesOut->Append(_bufIn+i, 1);
			i++;
		}
		_notesOut->Append("\n>\n");
		return true;
	}
	return false;
}


///////////////////////////////////////////////////////////////////////////////
// NotesUpdateJob
///////////////////////////////////////////////////////////////////////////////

void NotesUpdateJob::Perform() {
	if (NotesWnd* w = g_notesWndMgr.Get())
		w->Update(true);
}


///////////////////////////////////////////////////////////////////////////////
// NotesMarkerRegionListener
///////////////////////////////////////////////////////////////////////////////

// ScheduledJob because of multi-notifs during project switches (vs CSurfSetTrackListChange)
void NotesMarkerRegionListener::NotifyMarkerRegionUpdate(int _updateFlags)
{
	if (g_notesType>=SNM_NOTES_MKR_SUB && g_notesType<=SNM_NOTES_MKRRGN_SUB)
	{
		ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
	}
	else if (g_notesType>=SNM_NOTES_MKR_NAME && g_notesType<=SNM_NOTES_MKRRGN_NAME)
	{
		if (g_internalMkrRgnChange)
			g_internalMkrRgnChange = false;
		else
			ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
	}
}


///////////////////////////////////////////////////////////////////////////////

// import/export subtitle files, only SubRip (.srt) files atm
// see http://en.wikipedia.org/wiki/SubRip#Specifications

bool ImportSubRipFile(const char* _fn)
{
	bool ok = false;
	double firstPos = -1.0;

	// no need to check extension here, it's done for us
	if (FILE* f = fopenUTF8(_fn, "rt"))
	{
		char buf[1024];
		while(fgets(buf, sizeof(buf), f) && *buf)
		{
			if (int num = atoi(buf))
			{
				if (fgets(buf, sizeof(buf), f) && *buf)
				{
					int p1[4], p2[4];
					if (sscanf(buf, "%d:%d:%d,%d --> %d:%d:%d,%d", 
						&p1[0], &p1[1], &p1[2], &p1[3], 
						&p2[0], &p2[1], &p2[2], &p2[3]) != 8)
					{
						break;
					}

					WDL_FastString notes;
					while (fgets(buf, sizeof(buf), f) && *buf) {
						if (*buf == '\r' || *buf == '\n') break;
						notes.Append(buf);
					}
          
					WDL_String name(notes.Get());
					char *p=name.Get();
					while (*p) {
						if (*p == '\r' || *p == '\n') *p=' ';
						p++;
					}
					name.Ellipsize(0, 64); // 64 = native max mkr/rgn name length

					num = AddProjectMarker(NULL, true,
						p1[0]*3600 + p1[1]*60 + p1[2] + double(p1[3])/1000, 
						p2[0]*3600 + p2[1]*60 + p2[2] + double(p2[3])/1000, 
						name.Get(), num);

					if (num >= 0)
					{
						ok = true; // region added (at least)

						if (firstPos < 0.0)
							firstPos = p1[0]*3600 + p1[1]*60 + p1[2] + double(p1[3])/1000;

						int id = MakeMarkerRegionId(num, true);
						if (id > 0) // add the sub, no duplicate mgmt..
							g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(id, notes.Get()));
					}
				}
				else
					break;
			}
		}
		fclose(f);
	}
	
	if (ok)
	{
		UpdateTimeline(); // redraw the ruler (andd arrange view)
		if (firstPos > 0.0)
			SetEditCurPos2(NULL, firstPos, true, false);
	}
	return ok;
}

void ImportSubTitleFile(COMMAND_T* _ct)
{
	if (char* fn = BrowseForFiles(__LOCALIZE("S&M - Import subtitle file","sws_DLG_152"), g_lastImportSubFn, NULL, false, SNM_SUB_EXT_LIST))
	{
		lstrcpyn(g_lastImportSubFn, fn, sizeof(g_lastImportSubFn));
		if (ImportSubRipFile(fn))
			//JFB hard-coded undo label: _ct might be NULL (when called from a button)
			//    + avoid trailing "..." in undo point name (when called from an action)
			Undo_OnStateChangeEx2(NULL, __LOCALIZE("Import subtitle file","sws_DLG_152"), UNDO_STATE_ALL, -1);
		else
			MessageBox(GetMainHwnd(), __LOCALIZE("Invalid subtitle file!","sws_DLG_152"), __LOCALIZE("S&M - Error","sws_DLG_152"), MB_OK);
		free(fn);
	}
}

bool ExportSubRipFile(const char* _fn)
{
	if (FILE* f = fopenUTF8(_fn, "wt"))
	{
		WDL_FastString subs;
		int x=0, subIdx=1, num; bool isRgn; double p1, p2;
		while ((x = EnumProjectMarkers2(NULL, x, &isRgn, &p1, &p2, NULL, &num)))
		{
			// special case for markers: end position = next start position
			if (!isRgn)
			{
				int x2=x;
				if (!EnumProjectMarkers2(NULL, x2, NULL, &p2 /* <- the trick */, NULL, NULL, NULL))
					p2 = p1 + 5.0; // best effort..
			}

			int id = MakeMarkerRegionId(num, isRgn);
			if (id > 0)
			{
				for (int i=0; i < g_pRegionSubs.Get()->GetSize(); i++)
				{
					SNM_RegionSubtitle* rn = g_pRegionSubs.Get()->Get(i);
					if (rn->m_id == id)
					{
						subs.AppendFormatted(64, "%d\n", subIdx++); // subs have their own indexes
						
						int h, m, s, ms;
						TranslatePos(p1, &h, &m, &s, &ms);
						subs.AppendFormatted(64,"%02d:%02d:%02d,%03d --> ",h,m,s,ms);
						TranslatePos(p2, &h, &m, &s, &ms);
						subs.AppendFormatted(64,"%02d:%02d:%02d,%03d\n",h,m,s,ms);

						subs.Append(rn->m_notes.Get());
						if (rn->m_notes.GetLength() && rn->m_notes.Get()[rn->m_notes.GetLength()-1] != '\n')
							subs.Append("\n");
						subs.Append("\n");
					}
				}
			}
		}

		if (subs.GetLength())
		{
			fputs(subs.Get(), f);
			fclose(f);
			return true;
		}
	}
	return false;
}

void ExportSubTitleFile(COMMAND_T* _ct)
{
	char fn[SNM_MAX_PATH] = "";
	if (BrowseForSaveFile(__LOCALIZE("S&M - Export subtitle file","sws_DLG_152"), g_lastExportSubFn, strrchr(g_lastExportSubFn, '.') ? g_lastExportSubFn : NULL, SNM_SUB_EXT_LIST, fn, sizeof(fn))) {
		lstrcpyn(g_lastExportSubFn, fn, sizeof(g_lastExportSubFn));
		ExportSubRipFile(fn);
	}
}


///////////////////////////////////////////////////////////////////////////////
// project_config_extension_t
///////////////////////////////////////////////////////////////////////////////

static bool ProcessExtensionLine(const char *line, ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	LineParser lp(false);
	if (lp.parse(line) || lp.getnumtokens() < 1)
		return false;

	if (!strcmp(lp.gettoken_str(0), "<S&M_PROJNOTES"))
	{
		WDL_FastString notes;
		ExtensionConfigToString(&notes, ctx);

		char buf[MAX_HELP_LENGTH] = "";
		GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH);

		g_prjNotes.Get()->Set(buf);
		return true;
	}
	else if (!strcmp(lp.gettoken_str(0), "<S&M_TRACKNOTES"))
	{
		WDL_FastString notes;
		ExtensionConfigToString(&notes, ctx);

		char buf[MAX_HELP_LENGTH] = "";
		if (GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH))
		{
			GUID g;
			stringToGuid(lp.gettoken_str(1), &g);
			g_SNM_TrackNotes.Get()->Add(new SNM_TrackNotes(&g, buf));
		}
		return true;
	}
	else if (!strcmp(lp.gettoken_str(0), "<S&M_SUBTITLE"))
	{
		if (GetMarkerRegionIndexFromId(NULL, lp.gettoken_int(1)) >= 0) // still exists?
		{
			WDL_FastString notes;
			ExtensionConfigToString(&notes, ctx);

			char buf[MAX_HELP_LENGTH] = "";
			if (GetStringFromNotesChunk(&notes, buf, MAX_HELP_LENGTH))
				g_pRegionSubs.Get()->Add(new SNM_RegionSubtitle(lp.gettoken_int(1), buf));
			return true;
		}
	}
	return false;
}

static void SaveExtensionConfig(ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	char line[SNM_MAX_CHUNK_LINE_LENGTH] = "";
	char strId[128] = "";
	WDL_FastString formatedNotes;

	// save project notes
	if (g_prjNotes.Get()->GetLength())
	{
		strcpy(line, "<S&M_PROJNOTES\n|");
		if (GetNotesChunkFromString(g_prjNotes.Get()->Get(), &formatedNotes, line))
			StringToExtensionConfig(&formatedNotes, ctx);
	}

	// save track notes
	for (int i=0; i<g_SNM_TrackNotes.Get()->GetSize(); i++)
	{
		if (SNM_TrackNotes* tn = g_SNM_TrackNotes.Get()->Get(i))
		{
			MediaTrack* tr = tn->GetTrack();
			if (tn->m_notes.GetLength() &&
				tr && CSurf_TrackToID(tr, false) >= 0) // valid track?
			{
				guidToString((GUID*)tn->GetGUID(), strId);
				if (_snprintfStrict(line, sizeof(line), "<S&M_TRACKNOTES %s\n|", strId) > 0)
					if (GetNotesChunkFromString(tn->m_notes.Get(), &formatedNotes, line))
						StringToExtensionConfig(&formatedNotes, ctx);
			}
			else
				g_SNM_TrackNotes.Get()->Delete(i--, true);
		}
	}

	// save region/marker subs
	for (int i=0; i<g_pRegionSubs.Get()->GetSize(); i++)
	{
		if (SNM_RegionSubtitle* sub = g_pRegionSubs.Get()->Get(i))
		{
			if (sub->m_notes.GetLength() && GetMarkerRegionIndexFromId(NULL, sub->m_id) >= 0) // valid mkr/rgn?
			{
				if (_snprintfStrict(line, sizeof(line), "<S&M_SUBTITLE %d\n|", sub->m_id) > 0)
					if (GetNotesChunkFromString(sub->m_notes.Get(), &formatedNotes, line))
						StringToExtensionConfig(&formatedNotes, ctx);
			}
			else
			{
				g_pRegionSubs.Get()->Delete(i--, true);
			}
		}
	}
}

static void BeginLoadProjectState(bool isUndo, struct project_config_extension_t *reg)
{
	g_prjNotes.Cleanup();
	g_prjNotes.Get()->Set("");

	g_SNM_TrackNotes.Cleanup();
	g_SNM_TrackNotes.Get()->Empty(true);

	g_pRegionSubs.Cleanup();
	g_pRegionSubs.Get()->Empty(true);
}

static project_config_extension_t s_projectconfig = {
	ProcessExtensionLine, SaveExtensionConfig, BeginLoadProjectState, NULL
};


///////////////////////////////////////////////////////////////////////////////

void NotesSetTrackTitle()
{
	if (g_notesType == SNM_NOTES_TRACK)
		if (NotesWnd* w = g_notesWndMgr.Get())
			w->RefreshGUI();
}

// this is our only notification of active project tab change, so update everything
// (ScheduledJob because of multi-notifs)
void NotesSetTrackListChange() {
	ScheduledJob::Schedule(new NotesUpdateJob(SNM_SCHEDJOB_ASYNC_DELAY_OPT));
}


///////////////////////////////////////////////////////////////////////////////

int NotesInit()
{
	lstrcpyn(g_lastImportSubFn, GetResourcePath(), sizeof(g_lastImportSubFn));
	lstrcpyn(g_lastExportSubFn, GetResourcePath(), sizeof(g_lastExportSubFn));

	// load prefs 
	g_notesType = GetPrivateProfileInt(NOTES_INI_SEC, "Type", 0, g_SNM_IniFn.Get());
	g_locked = (GetPrivateProfileInt(NOTES_INI_SEC, "Lock", 0, g_SNM_IniFn.Get()) == 1);
	GetPrivateProfileString(NOTES_INI_SEC, "BigFontName", SNM_DYN_FONT_NAME, g_notesBigFontName, sizeof(g_notesBigFontName), g_SNM_IniFn.Get());
  g_wrapText=(GetPrivateProfileInt(NOTES_INI_SEC, "WrapText", 0, g_SNM_IniFn.Get()) == 1);

	// get the action help filename
	char defaultHelpFn[SNM_MAX_PATH] = "";
	if (_snprintfStrict(defaultHelpFn, sizeof(defaultHelpFn), SNM_ACTION_HELP_INI_FILE, GetResourcePath()) <= 0)
		*defaultHelpFn = '\0';
	GetPrivateProfileString(NOTES_INI_SEC, "Action_help_file", defaultHelpFn, g_actionHelpFn, sizeof(g_actionHelpFn), g_SNM_IniFn.Get());

	// instanciate the window if needed, can be NULL
	g_notesWndMgr.Init();

	if (!plugin_register("projectconfig", &s_projectconfig))
		return 0;

	return 1;
}

void NotesExit()
{
	plugin_register("-projectconfig", &s_projectconfig);

	char tmp[4] = "";
	if (_snprintfStrict(tmp, sizeof(tmp), "%d", g_notesType) > 0)
		WritePrivateProfileString(NOTES_INI_SEC, "Type", tmp, g_SNM_IniFn.Get()); 
	WritePrivateProfileString(NOTES_INI_SEC, "Lock", g_locked?"1":"0", g_SNM_IniFn.Get()); 
	WritePrivateProfileString(NOTES_INI_SEC, "BigFontName", g_notesBigFontName, g_SNM_IniFn.Get());
	WritePrivateProfileString(NOTES_INI_SEC, "WrapText", g_wrapText?"1":"0", g_SNM_IniFn.Get());

	// save the action help filename
	WDL_FastString escapedStr;
	escapedStr.SetFormatted(SNM_MAX_PATH, "\"%s\"", g_actionHelpFn);
	WritePrivateProfileString(NOTES_INI_SEC, "Action_help_file", escapedStr.Get(), g_SNM_IniFn.Get());

	g_notesWndMgr.Delete();
}

void OpenNotes(COMMAND_T* _ct)
{
	if (NotesWnd* w = g_notesWndMgr.Create())
	{
		int newType = (int)_ct->user; // -1 means toggle current type
		if (newType == -1)
			newType = g_notesType;

		w->Show(g_notesType == newType /* i.e toggle */, true);
		w->SetType(newType);

		if (!g_locked)
			SetFocus(GetDlgItem(w->GetHWND(), IDC_EDIT));
	}
}

int IsNotesDisplayed(COMMAND_T* _ct)
{
	if (NotesWnd* w = g_notesWndMgr.Get())
		return w->IsWndVisible();
	return 0;
}

void ToggleNotesLock(COMMAND_T*) {
	if (NotesWnd* w = g_notesWndMgr.Get())
		w->ToggleLock();
}

int IsNotesLocked(COMMAND_T*) {
	return g_locked;
}
