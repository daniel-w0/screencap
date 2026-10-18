#ifndef SC_TRAY_H
#define SC_TRAY_H

typedef struct {
  HWND hWindow;
  COLORREF dwAccent;
  HICON hIcon;
} scTrayMenu;

void scTrayInitialize();
void scTrayUpdateIcon();

#endif // SC_TRAY_H