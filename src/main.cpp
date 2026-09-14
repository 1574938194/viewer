#include <Windows.h>

#include "app/application.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    return nexus::app::Application::Run(commandLine);
}
