// The Lone Echo Mods window: drawn by hand with GDI+ on its own thread (dark theme, mod cards with switches, the
// selected mod's actions and list). Everything that touches the game is queued to the game thread (lemods.cpp).
#include "lemods_core.h"
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

namespace LeMods
{
	// ---- Look ----
	static const Color BG(255, 13, 16, 22), SIDEBAR(255, 19, 23, 31), CARD(255, 26, 32, 42), CARD_HOVER(255, 33, 41, 54),
		CARD_SELECTED(255, 30, 44, 60), LINE(255, 40, 49, 62), FG(255, 232, 237, 243), MUTED(255, 138, 150, 168),
		DIM(255, 92, 102, 118), ACCENT(255, 76, 194, 255), ACCENT_DARK(255, 27, 74, 104), ON_TEXT(255, 8, 20, 30),
		OFF_PILL(255, 44, 53, 66), WARN(255, 255, 170, 80);
	static const int MARGIN = 18, HEADER = 70, FOOTER = 34, SIDEBAR_W = 290, CARD_H = 66, BUTTON_H = 40, ROW_H = 30;

	enum HitKind { HIT_NONE, HIT_CARD, HIT_CARD_SWITCH, HIT_MAIN_SWITCH, HIT_BUTTON, HIT_ROW, HIT_LIST_ACTION };
	struct Hit { RectF rect; HitKind kind; int index; };

	static HWND g_window = NULL;
	static std::vector<Hit> g_hits;
	static Hit g_hover = { RectF(), HIT_NONE, -1 };
	static int g_selected = 0, g_listSelected = -1;
	static FLOAT g_listScroll = 0, g_sideScroll = 0;
	static RectF g_listRect, g_sideRect;
	static FLOAT g_listContent = 0, g_sideContent = 0;
	static BOOL g_tracking = FALSE;

	VOID RefreshWindow()
	{
		if (g_window != NULL)
			InvalidateRect(g_window, NULL, FALSE);
	}

	static std::wstring Wide(const CHAR* s)
	{
		if (s == NULL)
			return L"";
		int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
		std::wstring w(n > 0 ? n - 1 : 0, L'\0');
		if (n > 1)
			MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
		return w;
	}

	static VOID RoundRect(GraphicsPath& path, const RectF& r, FLOAT radius)
	{
		FLOAT d = min(radius * 2, min(r.Width, r.Height));
		path.Reset();
		if (d <= 0)
		{
			path.AddRectangle(r);
			return;
		}
		path.AddArc(r.X, r.Y, d, d, 180, 90);
		path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
		path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
		path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
		path.CloseFigure();
	}

	static VOID FillRound(Graphics& g, const RectF& r, FLOAT radius, const Color& color)
	{
		GraphicsPath path;
		RoundRect(path, r, radius);
		SolidBrush brush(color);
		g.FillPath(&brush, &path);
	}

	static VOID StrokeRound(Graphics& g, const RectF& r, FLOAT radius, const Color& color, FLOAT width = 1.0f)
	{
		GraphicsPath path;
		RoundRect(path, r, radius);
		Pen pen(color, width);
		g.DrawPath(&pen, &path);
	}

	enum TextMode { ONE_LINE, WRAP, CENTER };

	static VOID Text(Graphics& g, const std::wstring& s, const Font& font, const Color& color, const RectF& r, TextMode mode = ONE_LINE)
	{
		StringFormat format;
		if (mode != WRAP)
		{
			format.SetFormatFlags(StringFormatFlagsNoWrap);
			format.SetTrimming(StringTrimmingEllipsisCharacter);
			format.SetLineAlignment(StringAlignmentCenter);
		}
		if (mode == CENTER)
			format.SetAlignment(StringAlignmentCenter);
		SolidBrush brush(color);
		g.DrawString(s.c_str(), -1, &font, r, &format, &brush);
	}

	static FLOAT TextHeight(Graphics& g, const std::wstring& s, const Font& font, FLOAT width)
	{
		RectF bounds;
		g.MeasureString(s.c_str(), -1, &font, RectF(0, 0, width, 10000), &bounds);
		return bounds.Height;
	}

	static BOOL IsHover(HitKind kind, int index) { return g_hover.kind == kind && g_hover.index == index; }

	static VOID Switch(Graphics& g, FLOAT x, FLOAT y, BOOL on, BOOL hover)
	{
		RectF pill(x, y, 44, 24);
		FillRound(g, pill, 12, on ? ACCENT : (hover ? Color(255, 58, 69, 85) : OFF_PILL));
		FLOAT knob = on ? x + 23 : x + 3;
		SolidBrush brush(on ? Color(255, 255, 255, 255) : MUTED);
		g.FillEllipse(&brush, knob, y + 3, 18.0f, 18.0f);
	}

	static VOID Paint(HDC hdc, int width, int height)
	{
		Bitmap buffer(width, height, PixelFormat32bppPARGB);
		Graphics g(&buffer);
		g.SetSmoothingMode(SmoothingModeAntiAlias);
		g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
		g.Clear(BG);
		g_hits.clear();

		FontFamily family(L"Segoe UI");
		Font fontKicker(&family, 11, FontStyleBold, UnitPixel), fontTitle(&family, 24, FontStyleBold, UnitPixel),
			fontName(&family, 15, FontStyleBold, UnitPixel), fontSmall(&family, 12, FontStyleRegular, UnitPixel),
			fontBody(&family, 14, FontStyleRegular, UnitPixel), fontButton(&family, 13, FontStyleBold, UnitPixel),
			fontBig(&family, 28, FontStyleBold, UnitPixel), fontRow(&family, 13, FontStyleRegular, UnitPixel);
		std::vector<LoadedMod>& mods = Mods();
		if (g_selected >= (int)mods.size())
			g_selected = (int)mods.size() - 1;

		// Header
		int onCount = 0;
		for (LoadedMod& m : mods)
			onCount += m.enabled ? 1 : 0;
		Text(g, L"LONE ECHO", fontKicker, ACCENT, RectF((FLOAT)MARGIN, 14, 300, 16));
		Text(g, L"Mods", fontBig, FG, RectF((FLOAT)MARGIN, 28, 300, 36));
		WCHAR summary[64];
		swprintf_s(summary, L"%d mods  \x2022  %d on", (int)mods.size(), onCount);
		StringFormat right;
		right.SetAlignment(StringAlignmentFar);
		right.SetLineAlignment(StringAlignmentCenter);
		SolidBrush muted(MUTED);
		g.DrawString(summary, -1, &fontBody, RectF(0, 28, (FLOAT)width - MARGIN, 36), &right, &muted);

		// Sidebar: one card per mod
		g_sideRect = RectF((FLOAT)MARGIN, (FLOAT)HEADER, (FLOAT)SIDEBAR_W, (FLOAT)(height - HEADER - FOOTER - 8));
		g_sideContent = mods.size() * (FLOAT)(CARD_H + 8);
		g_sideScroll = max(0.0f, min(g_sideScroll, g_sideContent - g_sideRect.Height));
		g.SetClip(g_sideRect);
		for (size_t i = 0; i < mods.size(); i++)
		{
			LoadedMod& m = mods[i];
			RectF card(g_sideRect.X, g_sideRect.Y + i * (CARD_H + 8) - g_sideScroll, g_sideRect.Width, (FLOAT)CARD_H);
			if (card.GetBottom() < g_sideRect.Y || card.Y > g_sideRect.GetBottom())
				continue;
			BOOL selected = (int)i == g_selected;
			BOOL hover = IsHover(HIT_CARD, (int)i) || IsHover(HIT_CARD_SWITCH, (int)i);
			FillRound(g, card, 10, selected ? CARD_SELECTED : (hover ? CARD_HOVER : CARD));
			if (selected)
			{
				StrokeRound(g, card, 10, ACCENT_DARK, 1.5f);
				FillRound(g, RectF(card.X, card.Y + 14, 4, card.Height - 28), 2, ACCENT);
			}
			Text(g, Wide(m.mod.name), fontName, FG, RectF(card.X + 16, card.Y + 10, card.Width - 86, 22));
			const CHAR* status = m.mod.status != NULL ? SafeStatus(m.mod.status) : NULL;
			std::wstring line = m.enabled ? Wide(status) : L"Off";
			if (line.empty())
				line = m.enabled ? L"On" : L"Off";
			Text(g, line, fontSmall, m.enabled ? MUTED : DIM, RectF(card.X + 16, card.Y + 34, card.Width - 86, 20));
			Switch(g, card.GetRight() - 60, card.Y + (CARD_H - 24) / 2.0f, m.enabled != 0, IsHover(HIT_CARD_SWITCH, (int)i));
			RectF switchHit(card.GetRight() - 68, card.Y, 68, card.Height);
			g_hits.push_back({ switchHit, HIT_CARD_SWITCH, (int)i });
			g_hits.push_back({ card, HIT_CARD, (int)i });
		}
		g.ResetClip();

		// Main panel: the selected mod
		FLOAT px = (FLOAT)(MARGIN + SIDEBAR_W + 22), pw = (FLOAT)width - px - MARGIN;
		FLOAT y = (FLOAT)HEADER;
		if (g_selected >= 0 && pw > 200)
		{
			LoadedMod& m = mods[g_selected];
			BOOL on = m.enabled != 0;
			FLOAT titleW = pw - 150;
			Text(g, Wide(m.mod.name), fontTitle, FG, RectF(px, y, titleW, 34));
			std::wstring source = Wide(m.source.c_str());
			Text(g, source == L"built in" ? L"BUILT-IN MOD" : (L"FROM " + source), fontKicker, DIM, RectF(px, y + 34, titleW, 16));
			// Big switch button
			RectF toggle(px + pw - 140, y + 4, 140, 40);
			BOOL toggleHover = IsHover(HIT_MAIN_SWITCH, 0);
			if (on)
			{
				FillRound(g, toggle, 10, toggleHover ? Color(255, 120, 212, 255) : ACCENT);
				Text(g, L"ON  \x2014  switch off", fontButton, ON_TEXT, toggle, CENTER);
			}
			else
			{
				FillRound(g, toggle, 10, toggleHover ? CARD_HOVER : CARD);
				StrokeRound(g, toggle, 10, ACCENT, 1.5f);
				Text(g, L"Switch on", fontButton, ACCENT, toggle, CENTER);
			}
			g_hits.push_back({ toggle, HIT_MAIN_SWITCH, 0 });
			y += 60;

			std::wstring description = Wide(m.mod.description);
			FLOAT dh = TextHeight(g, description, fontBody, pw);
			Text(g, description, fontBody, MUTED, RectF(px, y, pw, dh + 4), WRAP);
			y += dh + 14;

			// Status strip
			const CHAR* status = m.mod.status != NULL ? SafeStatus(m.mod.status) : NULL;
			RectF strip(px, y, pw, 36);
			FillRound(g, strip, 8, CARD);
			SolidBrush dot(on ? ACCENT : DIM);
			g.FillEllipse(&dot, strip.X + 14, strip.Y + 13, 10.0f, 10.0f);
			std::wstring statusText = on ? Wide(status) : L"Off \x2014 switch it on to use it";
			if (statusText.empty())
				statusText = L"On";
			Text(g, statusText, fontBody, on ? FG : MUTED, RectF(strip.X + 34, strip.Y, strip.Width - 44, strip.Height));
			y += 52;

			// Buttons
			int buttonCount = 0;
			for (int i = 0; i < LEMOD_MAX_BUTTONS; i++)
				if (m.mod.buttons[i].label != NULL)
					buttonCount++;
			if (buttonCount > 0)
			{
				Text(g, L"ACTIONS", fontKicker, DIM, RectF(px, y, pw, 16));
				y += 22;
				int columns = pw >= 560 ? 4 : (pw >= 420 ? 3 : 2);
				FLOAT bw = (pw - (columns - 1) * 10) / columns;
				int slot = 0;
				for (int i = 0; i < LEMOD_MAX_BUTTONS; i++)
				{
					if (m.mod.buttons[i].label == NULL)
						continue;
					RectF b(px + (slot % columns) * (bw + 10), y + (slot / columns) * (BUTTON_H + 10), bw, (FLOAT)BUTTON_H);
					slot++;
					BOOL active = on && m.mod.buttonActive != NULL && SafeActive(m.mod.buttonActive, i);
					BOOL hover = on && IsHover(HIT_BUTTON, i);
					if (active)
					{
						FillRound(g, b, 9, ACCENT_DARK);
						StrokeRound(g, b, 9, ACCENT, 1.5f);
					}
					else
						FillRound(g, b, 9, hover ? CARD_HOVER : CARD);
					Text(g, Wide(m.mod.buttons[i].label), fontButton, !on ? DIM : (active ? ACCENT : FG),
						RectF(b.X + 8, b.Y, b.Width - 16, b.Height), CENTER);
					if (on)
						g_hits.push_back({ b, HIT_BUTTON, i });
				}
				y += ((buttonCount + columns - 1) / columns) * (BUTTON_H + 10) + 8;
			}

			// List
			g_listRect = RectF();
			if (m.mod.listCount != NULL && m.mod.listItem != NULL)
			{
				Text(g, m.mod.listTitle != NULL ? Wide(m.mod.listTitle) : L"LIST", fontKicker, DIM, RectF(px, y, pw, 16));
				y += 22;
				FLOAT bottom = (FLOAT)(height - FOOTER - 8);
				FLOAT listH = bottom - y - 52;
				if (listH > 60)
				{
					RectF box(px, y, pw, listH);
					FillRound(g, box, 10, CARD);
					g_listRect = RectF(box.X + 4, box.Y + 4, box.Width - 8, box.Height - 8);
					int n = on ? SafeCount(m.mod.listCount) : 0;
					g_listContent = n * (FLOAT)ROW_H;
					g_listScroll = max(0.0f, min(g_listScroll, g_listContent - g_listRect.Height));
					if (g_listSelected >= n)
						g_listSelected = -1;
					g.SetClip(g_listRect);
					if (n == 0)
						Text(g, on ? L"Nothing here yet" : L"Switch the mod on to fill this list", fontBody, DIM, g_listRect, CENTER);
					int first = (int)(g_listScroll / ROW_H);
					for (int i = first; i < n; i++)
					{
						RectF row(g_listRect.X, g_listRect.Y + i * ROW_H - g_listScroll, g_listRect.Width, (FLOAT)ROW_H);
						if (row.Y > g_listRect.GetBottom())
							break;
						BOOL sel = i == g_listSelected;
						if (sel)
							FillRound(g, row, 6, ACCENT_DARK);
						else if (IsHover(HIT_ROW, i))
							FillRound(g, row, 6, CARD_HOVER);
						Text(g, Wide(SafeItem(m.mod.listItem, i)), fontRow, sel ? FG : MUTED, RectF(row.X + 12, row.Y, row.Width - 24, row.Height));
						g_hits.push_back({ row, HIT_ROW, i });
					}
					g.ResetClip();
					// Scroll bar
					if (g_listContent > g_listRect.Height)
					{
						FLOAT barH = max(24.0f, g_listRect.Height * g_listRect.Height / g_listContent);
						FLOAT barY = g_listRect.Y + (g_listRect.Height - barH) * (g_listScroll / (g_listContent - g_listRect.Height));
						FillRound(g, RectF(box.GetRight() - 7, barY, 4, barH), 2, LINE);
					}
					// Action button
					RectF action(px + pw - 180, box.GetBottom() + 10, 180, 36);
					BOOL usable = on && g_listSelected >= 0;
					FillRound(g, action, 9, usable ? (IsHover(HIT_LIST_ACTION, 0) ? Color(255, 120, 212, 255) : ACCENT) : CARD);
					Text(g, Wide(m.mod.listAction != NULL ? m.mod.listAction : "Go"), fontButton, usable ? ON_TEXT : DIM, action, CENTER);
					if (usable)
						g_hits.push_back({ action, HIT_LIST_ACTION, 0 });
					Text(g, g_listSelected >= 0 ? L"Double-click a row to do it straight away" : L"Pick a row", fontSmall, DIM,
						RectF(px, action.Y, pw - 200, action.Height));
				}
			}
		}

		// Footer
		SolidBrush lineBrush(LINE);
		g.FillRectangle(&lineBrush, 0.0f, (FLOAT)(height - FOOTER), (FLOAT)width, 1.0f);
		Text(g, L"Add mods: put DLLs in bin\\win7\\mods   \x2022   Log: bin\\win7\\lemods.log   \x2022   Closing this window keeps the mods running",
			fontSmall, DIM, RectF((FLOAT)MARGIN, (FLOAT)(height - FOOTER), (FLOAT)width - 2 * MARGIN, (FLOAT)FOOTER));

		Graphics screen(hdc);
		screen.DrawImage(&buffer, 0, 0);
	}

	static Hit HitAt(FLOAT x, FLOAT y)
	{
		// Switches and rows are pushed before what they sit on, so the first match wins.
		for (const Hit& h : g_hits)
			if (h.rect.Contains(x, y))
				return h;
		return { RectF(), HIT_NONE, -1 };
	}

	static VOID Click(FLOAT x, FLOAT y, BOOL dbl)
	{
		Hit h = HitAt(x, y);
		std::vector<LoadedMod>& mods = Mods();
		switch (h.kind)
		{
		case HIT_CARD:
			if (h.index != g_selected)
			{
				g_selected = h.index;
				g_listSelected = -1;
				g_listScroll = 0;
			}
			else if (dbl)
				RequestEnable(h.index, !mods[h.index].enabled);
			break;
		case HIT_CARD_SWITCH:
			RequestEnable(h.index, !mods[h.index].enabled);
			break;
		case HIT_MAIN_SWITCH:
			RequestEnable(g_selected, !mods[g_selected].enabled);
			break;
		case HIT_BUTTON:
			RequestButton(g_selected, h.index);
			break;
		case HIT_ROW:
			g_listSelected = h.index;
			if (dbl)
				RequestListAction(g_selected, h.index);
			break;
		case HIT_LIST_ACTION:
			RequestListAction(g_selected, g_listSelected);
			break;
		default:
			break;
		}
		RefreshWindow();
	}

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
	{
		switch (message)
		{
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(window, &ps);
			RECT client;
			GetClientRect(window, &client);
			if (client.right > 0 && client.bottom > 0)
				Paint(hdc, client.right, client.bottom);
			EndPaint(window, &ps);
			return 0;
		}
		case WM_ERASEBKGND:
			return 1;
		case WM_MOUSEMOVE:
		{
			if (!g_tracking)
			{
				TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, window, 0 };
				g_tracking = TrackMouseEvent(&t);
			}
			Hit h = HitAt((FLOAT)GET_X_LPARAM(l), (FLOAT)GET_Y_LPARAM(l));
			if (h.kind != g_hover.kind || h.index != g_hover.index)
			{
				g_hover = h;
				SetCursor(LoadCursor(NULL, h.kind == HIT_NONE ? IDC_ARROW : IDC_HAND));
				RefreshWindow();
			}
			return 0;
		}
		case WM_SETCURSOR:
			if (LOWORD(l) == HTCLIENT)
			{
				SetCursor(LoadCursor(NULL, g_hover.kind == HIT_NONE ? IDC_ARROW : IDC_HAND));
				return TRUE;
			}
			break;
		case WM_MOUSELEAVE:
			g_tracking = FALSE;
			g_hover = { RectF(), HIT_NONE, -1 };
			RefreshWindow();
			return 0;
		case WM_LBUTTONDOWN:
			Click((FLOAT)GET_X_LPARAM(l), (FLOAT)GET_Y_LPARAM(l), FALSE);
			return 0;
		case WM_LBUTTONDBLCLK:
			Click((FLOAT)GET_X_LPARAM(l), (FLOAT)GET_Y_LPARAM(l), TRUE);
			return 0;
		case WM_MOUSEWHEEL:
		{
			POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
			ScreenToClient(window, &p);
			FLOAT delta = -(FLOAT)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * ROW_H * 3;
			if (g_listRect.Contains((FLOAT)p.x, (FLOAT)p.y))
				g_listScroll += delta;
			else if (g_sideRect.Contains((FLOAT)p.x, (FLOAT)p.y))
				g_sideScroll += delta;
			RefreshWindow();
			return 0;
		}
		case WM_TIMER:
			RefreshWindow();  // live status lines
			return 0;
		case WM_GETMINMAXINFO:
			((MINMAXINFO*)l)->ptMinTrackSize = { 860, 560 };
			return 0;
		case WM_CLOSE:
			ShowWindow(window, SW_MINIMIZE);  // the window stays while the game runs
			return 0;
		}
		return DefWindowProcW(window, message, w, l);
	}

	static VOID(*g_beforeShow)() = NULL;

	static DWORD WINAPI WindowThread(LPVOID)
	{
		if (g_beforeShow != NULL)
			g_beforeShow();
		GdiplusStartupInput input;
		ULONG_PTR token = 0;
		GdiplusStartup(&token, &input, NULL);
		HINSTANCE instance = GetModuleHandleW(NULL);
		WNDCLASSW wc = {};
		wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = instance;
		wc.hbrBackground = CreateSolidBrush(RGB(13, 16, 22));
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1));
		wc.lpszClassName = L"LoneEchoMods";
		RegisterClassW(&wc);
		g_window = CreateWindowExW(0, L"LoneEchoMods", L"Lone Echo Mods", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
			60, 60, 980, 680, NULL, NULL, instance, NULL);
		BOOL dark = TRUE;
		DwmSetWindowAttribute(g_window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
		SetTimer(g_window, 1, 250, NULL);
		ShowWindow(g_window, SW_SHOWNOACTIVATE);
		MSG msg;
		while (GetMessageW(&msg, NULL, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		GdiplusShutdown(token);
		return 0;
	}

	VOID StartWindow(VOID(*beforeShow)())
	{
		g_beforeShow = beforeShow;
		CreateThread(NULL, 0, WindowThread, NULL, 0, NULL);
	}
}
