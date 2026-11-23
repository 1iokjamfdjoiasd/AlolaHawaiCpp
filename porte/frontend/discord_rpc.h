#pragma once
// Discord Rich Presence (22/11/2026) sem biblioteca: o protocolo local do Discord (pipe \\.\pipe\discord-ipc-N,
// quadros = int32 opcode + int32 tamanho + JSON). O mesmo que a discord-rpc / o Social SDK fazem por baixo.
// Precisa de um Application ID (portal de desenvolvedores do Discord): config.ini "discord_id=" (ou kDiscordIdPadrao).
// Tudo numa thread: o jogo nunca espera pelo Discord. Discord fechado = tenta de novo a cada 15 s.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace DiscordRpc
{
// o ID embutido no exe (público, como nos emuladores); vazio até o aplicativo ser criado - o config.ini tem prioridade
inline const char* kDiscordIdPadrao = "";

enum class Status { Off, NoId, NoDiscord, Connecting, Connected, BadId };
inline std::atomic<Status> status{Status::Off};
inline std::atomic<bool> running{false};
inline std::thread worker;
inline std::mutex m;
inline std::string clientId, details, state, largeText;
inline long long startTime = 0;
inline std::atomic<unsigned> version{0};   // a atividade mudou

inline std::string Esc(const std::string& s)
{
    std::string o;
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

inline bool WriteFrame(HANDLE h, int op, const std::string& json)
{
    std::string buf(8 + json.size(), '\0');
    const int len = (int)json.size();
    memcpy(&buf[0], &op, 4); memcpy(&buf[4], &len, 4); memcpy(&buf[8], json.data(), json.size());
    DWORD w = 0;
    return WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) && w == buf.size();
}
// reads one frame (waits up to ms); false = nothing / error
inline bool ReadFrame(HANDLE h, int& op, std::string& json, int ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail >= 8) break;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    int hdr[2]; DWORD r = 0;
    if (!ReadFile(h, hdr, 8, &r, nullptr) || r != 8 || hdr[1] < 0 || hdr[1] > (1 << 20)) return false;
    op = hdr[0];
    json.assign(hdr[1], '\0');
    DWORD got = 0;
    while (got < (DWORD)hdr[1])
    {
        if (!ReadFile(h, &json[got], hdr[1] - got, &r, nullptr) || !r) return false;
        got += r;
    }
    return true;
}

inline void Loop()
{
    HANDLE h = INVALID_HANDLE_VALUE;
    unsigned sent = ~0u;
    auto lastTry = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    auto lastSend = lastTry;
    while (running)
    {
        std::string id;
        { std::lock_guard<std::mutex> l(m); id = clientId; }
        if (id.empty()) { status = Status::NoId; std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        if (h == INVALID_HANDLE_VALUE)
        {
            if (std::chrono::steady_clock::now() - lastTry < std::chrono::seconds(15)) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            lastTry = std::chrono::steady_clock::now();
            status = Status::Connecting;
            for (int i = 0; i < 10 && h == INVALID_HANDLE_VALUE; i++)
            {
                const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
                h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            }
            if (h == INVALID_HANDLE_VALUE) { status = Status::NoDiscord; continue; }
            int op = 0; std::string resp;
            if (!WriteFrame(h, 0, "{\"v\":1,\"client_id\":\"" + Esc(id) + "\"}") || !ReadFrame(h, op, resp, 3000) || op != 1 ||
                resp.find("\"READY\"") == std::string::npos)
            {
                fprintf(stderr, "discord: recusado (%s)\n", resp.substr(0, 160).c_str());
                status = resp.find("4000") != std::string::npos || resp.find("Invalid") != std::string::npos ? Status::BadId : Status::NoDiscord;
                CloseHandle(h); h = INVALID_HANDLE_VALUE;
                continue;
            }
            status = Status::Connected;
            fprintf(stderr, "discord: conectado\n");
            sent = ~0u;
        }
        // the activity (when it changed, and every 60 s)
        const unsigned v = version;
        if (v != sent || std::chrono::steady_clock::now() - lastSend > std::chrono::seconds(60))
        {
            std::string d, s, lt; long long st;
            { std::lock_guard<std::mutex> l(m); d = details; s = state; lt = largeText; st = startTime; }
            static unsigned nonce = 0;
            std::string act = "{\"details\":\"" + Esc(d) + "\"";
            if (!s.empty()) act += ",\"state\":\"" + Esc(s) + "\"";
            act += ",\"timestamps\":{\"start\":" + std::to_string(st) + "},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"" + Esc(lt) + "\"}}";
            const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"activity\":" + act +
                                    "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
            if (!WriteFrame(h, 1, cmd)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; status = Status::NoDiscord; continue; }
            sent = v; lastSend = std::chrono::steady_clock::now();
        }
        int op; std::string resp;
        while (ReadFrame(h, op, resp, 0))   // answers: read and drop (a full pipe would block the Discord)
            if (op == 2) { CloseHandle(h); h = INVALID_HANDLE_VALUE; status = Status::NoDiscord; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFrame(h, 1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + "},\"nonce\":\"fim\"}");   // clears it
        CloseHandle(h);
    }
    status = Status::Off;
}

inline void Start(const std::string& id, const std::string& gameName)
{
    if (running) return;
    { std::lock_guard<std::mutex> l(m); clientId = id; largeText = gameName; startTime = (long long)time(nullptr); }
    running = true;
    worker = std::thread(Loop);
}
inline void Stop()
{
    if (!running) return;
    running = false;
    if (worker.joinable()) worker.join();
}
inline void SetActivity(const std::string& d, const std::string& s)
{
    std::lock_guard<std::mutex> l(m);
    if (d == details && s == state) return;
    details = d; state = s;
    version++;
}
}
