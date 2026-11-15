#pragma once
// A bounded FFmpeg subprocess: argv is passed directly, never through a shell.
// PAMF must be demuxed as MPEG; .pam otherwise selects the image demuxer.
#ifdef _WIN32
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef POLLIN
#define POLLIN 1
#endif
namespace port_sail {
// poll and non-blocking read over an anonymous pipe, as the code below uses them
// on POSIX. Anonymous pipes have no overlapped mode, so PeekNamedPipe stands in.
struct pollfd { HANDLE fd; short events, revents; };
inline bool pipe_available(HANDLE h, DWORD& available) {
    available = 0;
    return PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr);
}
inline int poll(pollfd* p, int, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    DWORD available;
    while (pipe_available(p->fd, available) && !available) {
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        Sleep(1);
    }
    return 1;
}
inline ssize_t read(HANDLE h, void* data, size_t count) {
    DWORD available, got = 0;
    if (pipe_available(h, available)) {
        if (!available) { errno = EAGAIN; return -1; }
        if (ReadFile(h, data, count < available ? (DWORD)count : available, &got, nullptr)) return (ssize_t)got;
    }
    if (GetLastError() == ERROR_BROKEN_PIPE) return 0;   // the writer closed: end of file
    errno = EIO;
    return -1;
}
struct Child {
    HANDLE process = nullptr;
    HANDLE fd = nullptr;
    // The rules CommandLineToArgvW and the C runtime parse a command line by.
    static void quote(std::string& line, const std::string& arg) {
        if (!line.empty()) line += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) { line += arg; return; }
        line += '"';
        size_t backslashes = 0;
        for (char c : arg) {
            if (c == '\\') { backslashes++; continue; }
            line.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
            line += c;
            backslashes = 0;
        }
        line.append(backslashes * 2, '\\');
        line += '"';
    }
    bool spawn(std::vector<std::string> args) {
        // PATH only, as posix_spawnp: CreateProcess alone also tries the current directory.
        char program[MAX_PATH];
        const DWORD found = SearchPathA(std::getenv("PATH"), args[0].c_str(), ".exe", MAX_PATH, program, nullptr);
        if (!found || found >= MAX_PATH) return false;
        std::string line;
        for (const auto& a : args) quote(line, a);
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        HANDLE reader, writer;
        if (!CreatePipe(&reader, &writer, &inherit, 1u << 20)) return false;
        SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
        HANDLE null_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOA startup{};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = null_in;
        startup.hStdOutput = writer;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION created{};
        const BOOL ok = CreateProcessA(program, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &created);
        CloseHandle(writer);
        if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
        if (!ok) { CloseHandle(reader); return false; }
        CloseHandle(created.hThread);
        process = created.hProcess;
        fd = reader;
        return true;
    }
    int finish(bool terminate) {
        if (fd) { CloseHandle(fd); fd = nullptr; }
        if (!process) return -1;
        if (terminate) TerminateProcess(process, 1);
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        const BOOL ok = GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        return ok ? (int)code : -1;
    }
    ~Child() { finish(true); }
};
#else
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
extern char** environ;
namespace port_sail {
struct Child {
    pid_t pid = -1;
    int fd = -1;
    bool spawn(std::vector<std::string> args) {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC)) return false;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        int error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);
        if (error) { close(pipefd[0]); pid = -1; return false; }
        fd = pipefd[0];
        fcntl(fd, F_SETFL, O_NONBLOCK);
        return true;
    }
    int finish(bool terminate) {
        if (fd >= 0) { close(fd); fd = -1; }
        if (pid < 0) return -1;
        if (terminate) kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) { pid = -1; return -1; }
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    ~Child() { finish(true); }
};
#endif /* _WIN32 */
struct Video {
    int width = 0, height = 0, rate_num = 0, rate_den = 1;
    bool probe(const std::string& path) {
        Child child;
        if (!child.spawn({"ffprobe", "-v", "error", "-f", "mpeg", "-select_streams", "v:0",
                "-show_entries", "stream=width,height,r_frame_rate", "-of", "csv=p=0", path})) return false;
        std::string output;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool eof = false;
        while (std::chrono::steady_clock::now() < deadline && output.size() < 1024) {
            pollfd p{child.fd, POLLIN, 0}; poll(&p, 1, 100);
            char data[256]; ssize_t n = read(child.fd, data, sizeof data);
            if (!n) { eof = true; break; }
            if (n > 0) output.append(data, n);
            else if (errno != EAGAIN && errno != EINTR) break;
        }
        int status = child.finish(!eof);
        return status == 0 && sscanf(output.c_str(), "%d,%d,%d/%d", &width, &height, &rate_num, &rate_den) == 4
            && width > 0 && width <= 4096 && height > 0 && height <= 2160
            && rate_num > 0 && rate_den > 0 && double(rate_num)/rate_den <= 120;
    }
};
struct Frame { std::vector<uint8_t> bytes; uint64_t index = 0; };
class Decoder {
    Child child;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Frame> frames;
    bool ended = false;
    int result = 0;
public:
    ~Decoder() { stop(); }
    void stop() {
        cancelled = true; changed.notify_all();
        if (worker.joinable()) worker.join();
    }
    bool start(const std::string& path, const std::string& pixel, size_t frame_size) {
        if (!frame_size || frame_size > 4096u*2160u*4u) return false;
        if (!child.spawn({"ffmpeg", "-nostdin", "-v", "error", "-threads", "2", "-f", "mpeg", "-i", path,
            "-map", "0:v:0", "-an", "-sn", "-dn", "-threads", "1", "-pix_fmt", pixel,
            "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1"})) return false;
        worker = std::thread([this, frame_size] {
            uint64_t index = 0;
            bool eof = false, partial = false;
            while (!cancelled) {
                Frame frame; frame.index = index++; frame.bytes.resize(frame_size);
                size_t used = 0;
                while (used < frame_size && !cancelled) {
                    pollfd p{child.fd, POLLIN, 0}; poll(&p, 1, 100);
                    ssize_t n = read(child.fd, frame.bytes.data()+used, frame_size-used);
                    if (!n) { eof = true; break; }
                    if (n > 0) used += n;
                    else if (errno != EAGAIN && errno != EINTR) { partial = true; break; }
                }
                if (eof || partial) { partial = partial || used != 0; break; }
                std::unique_lock lock(mutex);
                changed.wait(lock, [this]{return cancelled || frames.size() < 4;});
                if (cancelled) break;
                frames.push_back(std::move(frame));
            }
            int code = child.finish(cancelled || !eof);
            std::lock_guard lock(mutex);
            result = partial && code == 0 ? -1 : code; ended = true;
        });
        return true;
    }
    bool take(Frame& frame) {
        std::lock_guard lock(mutex);
        if (frames.empty()) return false;
        frame = std::move(frames.front()); frames.pop_front(); changed.notify_all(); return true;
    }
    bool done(int& code) {
        std::lock_guard lock(mutex); code = result; return ended && frames.empty();
    }
};
}
