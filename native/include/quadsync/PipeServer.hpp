#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace quadsync {
// UTF-8 NDJSON request/response transport. One local control client at a time.
void RunPipeServer(const std::wstring& name,HANDLE stop,
                   const std::function<std::string(const std::string&)>& dispatch);
}
