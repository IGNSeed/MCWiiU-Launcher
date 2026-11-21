#include "../UpdateHandoff.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int)
{
	const std::wstring_view command(arguments ? arguments : L"");
	if (!command.empty() && !command.starts_with(L"--update-ready "))
		return 2;
	return LauncherUpdate::RunBootstrapper(command) ? 0 : 1;
}
