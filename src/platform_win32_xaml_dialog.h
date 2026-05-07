#pragma once

#include <Windows.h>

void ShowClippMainDialog(HWND owner);
bool ClippMainDialogPreTranslateMessage(MSG* msg);
void CloseClippMainDialog();
