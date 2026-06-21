#include "pch.h"
#include "scLogging.h"
#include "scAssert.h"
#include "scApp.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
  { // Setup Console
    bool bHasConsole = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      bHasConsole = true;
    } else {
#if defined(SC_DEBUG)
      if (AllocConsole()) {
        bHasConsole = true;
      }
#endif // SC_DEBUG
    }

    if (bHasConsole) {
      FILE* pFile = NULL;
      freopen_s(&pFile, "CONOUT$", "w", stdout);
      freopen_s(&pFile, "CONOUT$", "w", stderr);
      freopen_s(&pFile, "CONIN$",  "r", stdin);
    }
  }

  { // Ensure we are the only instance running
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "screencap_single_instance_mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      // maybe we can open the main window if we do this
      scLogWarn("There is already an instance of Screencap running! Aborting...");
      return 0;
    }
  }

  { // Application...
    scAppInit();

    scAppDestroy();
  }

  return 0;
}