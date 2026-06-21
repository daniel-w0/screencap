#include "pch.h"
#include "scAssert.h"
#include "scLogging.h"

#if defined(SC_DEBUG)
void _scAssertFail(const char* sCondition, const char* sFile, s32 iLine, const char* sMsg, ...) {
    scLogError("ASSERTION FAILED: (%s) in %s at line %d", sCondition, sFile, iLine);

    char messageBuffer[1024];
    va_list args;
    va_start(args, sMsg);
    vsnprintf(messageBuffer, sizeof(messageBuffer), sMsg, args);
    va_end(args);

    scLogError("REASON: %s", messageBuffer);

    char szAlertBuffer[2048];
    snprintf(szAlertBuffer, sizeof(szAlertBuffer), "Condition: %s\nFile: %s\nLine: %d\n\nReason: %s\n\nClick 'Abort' to break into debugger.",  sCondition, sFile, iLine, messageBuffer);

    int iResult = MessageBoxA(NULL, szAlertBuffer, "Assertion Failed!", MB_ICONERROR | MB_ABORTRETRYIGNORE | MB_DEFBUTTON2);

    if (iResult == IDABORT) {
      __debugbreak();
        ExitProcess(1);
    } else if (iResult == IDRETRY) {
        if (IsDebuggerPresent()) {
            __debugbreak(); 
        }
    }
}
#endif