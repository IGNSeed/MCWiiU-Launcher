// Shared with the legacy command-line parser; not invoked by the launcher.
bool isConsoleConnected = false;
void requireConsole()
{
    #if BOOST_OS_WINDOWS
    if (isConsoleConnected)
        return;

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwFileType = GetFileType(hOut);

    if (dwFileType == FILE_TYPE_UNKNOWN || dwFileType == FILE_TYPE_CHAR)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE)
        {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            freopen("CONIN$", "r", stdin);
            isConsoleConnected = true;
        }
    }
    else
    {
        isConsoleConnected = true;
    }
    #endif
}
