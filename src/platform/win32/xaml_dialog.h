#pragma once

#ifndef _WINDEF_
struct HWND__;
typedef HWND__* HWND;
#endif

#ifndef _WINUSER_
struct tagMSG;
typedef tagMSG MSG;
#endif

enum class ClippMainDialogPage {
    Clipp,
    Network,
};

void ShowClippMainDialog(HWND owner);
void ShowClippMainDialog(HWND owner, ClippMainDialogPage page);
bool ClippMainDialogPreTranslateMessage(MSG* msg);
void CloseClippMainDialog();
