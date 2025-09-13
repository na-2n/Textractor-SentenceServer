#include "Extension.h"

#include <mutex>
#include <atomic>
#include <string>
#include <fstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stringapiset.h>

#pragma comment(lib, "Ws2_32.lib") 

#define DEFAULT_ADDR htonl(INADDR_LOOPBACK)
#define DEFAULT_PORT htons(2653)

std::atomic<SOCKET> sockfd = -1;
std::atomic_bool run = true;
std::mutex m;
std::wstring cur_sentence = L"";
HANDLE accept_thread_handle;
struct sockaddr_in bind_addr;

void read_conf()
{
    bind_addr.sin_family = AF_INET;

    std::ifstream f("senserv.txt", std::ios::binary);
    if (!f) {
        bind_addr.sin_addr.s_addr = DEFAULT_ADDR;
        bind_addr.sin_port = DEFAULT_PORT;

        return;
    }

    int i = 0;
    for (std::string line; std::getline(f, line, ':');) {
        switch (i) {
            case 0:
                inet_pton(AF_INET, line.c_str(), &(bind_addr.sin_addr));
                break;

            case 1:
                bind_addr.sin_port = htons(atoi(line.c_str()));
                break;
        }

        i++;
    }

    if (i == 0) bind_addr.sin_addr.s_addr = DEFAULT_ADDR;
    if (i <= 1) bind_addr.sin_port = DEFAULT_PORT;
}

int init_sock()
{
    WSADATA wsa_data;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return -1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        return 1;
    }

    const char enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
        return 1;
    }

    if (::bind(sockfd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) == -1) {
        return 1;
    }

    if (listen(sockfd, 8) == -1) {
        return 1;
    }

    return 0;
}

int close_sock()
{
    WSACleanup();

    closesocket(sockfd);

    return 0;
}

// taken from Textractor include/common.h
template <typename F>
HANDLE SpawnThread(const F &f)
{
    F *copy = new F(f);
    return CreateThread(nullptr, 0, [](void *copy)
    {
        (*(F *)copy)();
        delete (F *)copy;
        return 0UL;
    }, copy, 0, nullptr);
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            read_conf();

#ifdef _DEBUG
            MessageBoxW(NULL, (L"Running on port: " + std::to_wstring(ntohs(bind_addr.sin_port))).c_str(), L"SentenceServer", MB_OK | MB_ICONINFORMATION);
#endif

            if (init_sock() != 0) {
                MessageBoxW(NULL, L"Failed to init socket", L"SentenceServer", MB_OK | MB_ICONERROR);
                return FALSE;
            }

            accept_thread_handle = SpawnThread([]() {
                while (run) {
                    SOCKET clientfd = accept(sockfd, NULL, NULL);
                    if (clientfd == -1) {
                        //throw std::runtime_error("error accepting client");
                        continue;
                    }

                    std::string resp;
                    {
                        std::lock_guard<std::mutex> lock(m);

                        int size_needed = WideCharToMultiByte(CP_UTF8, 0, cur_sentence.c_str(), static_cast<int>(cur_sentence.size()), NULL, 0, NULL, NULL);
                        std::string sentence_utf8(size_needed, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, cur_sentence.c_str(), static_cast<int>(cur_sentence.size()), &sentence_utf8[0], size_needed, NULL, NULL);

                        resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain;charset=utf-8\r\n"
                               "Content-Length: " + std::to_string(sentence_utf8.size()) + "\r\n"
                               "\r\n"
                               + sentence_utf8;
                    }

                    if (send(clientfd, resp.c_str(), static_cast<int>(resp.size()), 0) < 0) {
                        MessageBoxW(NULL, L"Failed to send http response", L"SentenceServer", MB_OK | MB_ICONERROR);
                        closesocket(clientfd);

                        continue;
                    }

                    shutdown(clientfd, SD_SEND);

                    char tmp[BUFSIZ];
                    while (recv(clientfd, tmp, BUFSIZ, 0) > 0); // wait until the client closes the connection 

                    closesocket(clientfd);
                }
            });

            break;

        case DLL_PROCESS_DETACH:
            run = false;
            CloseHandle(accept_thread_handle);
            close_sock();

            break;
    }

    return TRUE;
}

/*
    Param sentence: sentence received by Textractor (UTF-16). Can be modified, Textractor will receive this modification only if true is returned.
    Param sentenceInfo: contains miscellaneous info about the sentence (see README).
    Return value: whether the sentence was modified.
    Textractor will display the sentence after all extensions have had a chance to process and/or modify it.
    The sentence will be destroyed if it is empty or if you call Skip().
    This function may be run concurrently with itself: please make sure it's thread safe.
    It will not be run concurrently with DllMain.
*/
bool ProcessSentence(std::wstring &sentence, SentenceInfo sentenceInfo)
{
    if (sentenceInfo["current select"] && sentenceInfo["process id"] != 0) {
        std::lock_guard<std::mutex> lock(m);
        cur_sentence = sentence;
    }

    return false;
}

