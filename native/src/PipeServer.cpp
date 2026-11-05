#include "quadsync/PipeServer.hpp"
#include <sddl.h>
#include <stdexcept>
#include <vector>
#include <memory>

namespace quadsync { namespace {
struct Handle {HANDLE h=nullptr;~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
struct Local {void* p=nullptr;~Local(){if(p)LocalFree(p);}};
bool finish(HANDLE pipe,HANDLE stop,OVERLAPPED& ov,BOOL immediate,DWORD& bytes) {
    if(immediate)return true;
    if(GetLastError()!=ERROR_IO_PENDING)return false;
    HANDLE events[]={stop,ov.hEvent};
    if(WaitForMultipleObjects(2,events,FALSE,INFINITE)!=WAIT_OBJECT_0+1) {
        CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&bytes,TRUE);return false;
    }
    return GetOverlappedResult(pipe,&ov,&bytes,FALSE)!=FALSE;
}
}
void RunPipeServer(const std::wstring& name,HANDLE stop,const std::function<std::string(const std::string&)>& dispatch) {
    Handle token;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h))throw std::runtime_error("Cannot query pipe owner");
    DWORD length=0;GetTokenInformation(token.h,TokenUser,nullptr,0,&length);
    std::vector<unsigned char> user(length);
    if(!GetTokenInformation(token.h,TokenUser,user.data(),length,&length))throw std::runtime_error("Cannot query user SID");
    Local sid,descriptor;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,reinterpret_cast<LPWSTR*>(&sid.p)))throw std::runtime_error("Cannot format user SID");
    std::wstring acl=L"D:P(A;;GA;;;"+std::wstring(static_cast<wchar_t*>(sid.p))+L")";
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor.p,nullptr))throw std::runtime_error("Cannot create pipe ACL");
    SECURITY_ATTRIBUTES sa{sizeof(sa),descriptor.p,FALSE};
    Handle pipe;
    pipe.h=CreateNamedPipeW((L"\\\\.\\pipe\\"+name).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&sa);
    if(pipe.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create named pipe (another engine may already be running)");
    Handle event;event.h=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event.h)throw std::runtime_error("Cannot create pipe event");
    while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0) {
        OVERLAPPED ov{};ov.hEvent=event.h;ResetEvent(event.h);DWORD count=0;
        BOOL connected=ConnectNamedPipe(pipe.h,&ov);
        if(!connected && GetLastError()==ERROR_PIPE_CONNECTED)connected=TRUE;
        if(!finish(pipe.h,stop,ov,connected,count)) {DisconnectNamedPipe(pipe.h);continue;}
        std::string pending;
        bool alive=true;
        while(alive && WaitForSingleObject(stop,0)!=WAIT_OBJECT_0) {
            char buffer[4096];ov={};ov.hEvent=event.h;ResetEvent(event.h);count=0;
            if(!finish(pipe.h,stop,ov,ReadFile(pipe.h,buffer,sizeof(buffer),&count,&ov),count) || count==0)break;
            pending.append(buffer,count);
            if(pending.size()>65536)break;
            size_t newline;
            while((newline=pending.find('\n'))!=std::string::npos) {
                std::string line=pending.substr(0,newline);pending.erase(0,newline+1);
                std::string response=dispatch(line)+"\n";
                size_t offset=0;
                while(offset<response.size()) {
                    ov={};ov.hEvent=event.h;ResetEvent(event.h);count=0;
                    if(!finish(pipe.h,stop,ov,WriteFile(pipe.h,response.data()+offset,static_cast<DWORD>(response.size()-offset),&count,&ov),count) || !count) {alive=false;break;}
                    offset+=count;
                }
                if(!alive)break;
            }
        }
        DisconnectNamedPipe(pipe.h);
    }
}
}
