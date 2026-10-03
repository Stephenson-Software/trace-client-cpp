// Drives the client against a real HTTP server on a loopback port -- a small
// one written here with plain sockets, so the tests have no more dependencies
// than the client does. Build and run: see the README's "Building".
#include "../trace_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define CLOSE_SOCKET closesocket
static const socket_t NO_SOCKET = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define CLOSE_SOCKET ::close
static const socket_t NO_SOCKET = -1;
#endif

// ---------------------------------------------------------------- harness

static int failures = 0;
static std::string currentTest;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++failures;                                                                    \
            std::cerr << "  FAIL " << currentTest << " (line " << __LINE__ << "): " #condition \
                      << std::endl;                                                        \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(expected, actual)                                                         \
    do {                                                                                   \
        if (!((expected) == (actual))) {                                                   \
            ++failures;                                                                    \
            std::cerr << "  FAIL " << currentTest << " (line " << __LINE__ << "): expected ["  \
                      << (expected) << "] got [" << (actual) << "]" << std::endl;          \
        }                                                                                  \
    } while (0)

static double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void setEnv(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

// ---------------------------------------------------------------- stub server

struct Request {
    std::string method, path, authorization, contentType, userAgent, body;
    std::vector<std::string> headerLines;
};

static std::string lower(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

class StubServer {
public:
    StubServer() : status(201), held(false), stopping(false), listener(NO_SOCKET), port(0) {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address;
        std::memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address);
        ::listen(listener, 64);
        socklen_t length = sizeof address;
        ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
        port = ntohs(address.sin_port);
        acceptor = std::thread(&StubServer::acceptLoop, this);
    }

    ~StubServer() {
        release();
        stopping = true;
        acceptor.join();
        std::vector<std::thread> running;
        {
            std::lock_guard<std::mutex> lock(mutex);
            running.swap(workers);
        }
        for (std::size_t i = 0; i < running.size(); ++i) running[i].join();
        CLOSE_SOCKET(listener);
    }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port); }

    void hold() { std::lock_guard<std::mutex> lock(mutex); held = true; }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        held = false;
        changed.notify_all();
    }

    bool waitFor(std::size_t count, double seconds) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::milliseconds(static_cast<long long>(seconds * 1000)),
                                [&] { return requests.size() >= count; });
    }

    std::vector<Request> received() {
        std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }

    std::atomic<int> status;

private:
    void acceptLoop() {
        while (!stopping) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener, &readable);
            timeval wait;
            wait.tv_sec = 0;
            wait.tv_usec = 50 * 1000;
            if (::select(static_cast<int>(listener + 1), &readable, NULL, NULL, &wait) <= 0) continue;
            socket_t connection = ::accept(listener, NULL, NULL);
            if (connection == NO_SOCKET) continue;
            std::lock_guard<std::mutex> lock(mutex);
            workers.push_back(std::thread(&StubServer::serve, this, connection));
        }
    }

    void serve(socket_t connection) {
        std::string data;
        char buffer[4096];
        std::size_t headerEnd = std::string::npos;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            int n = static_cast<int>(::recv(connection, buffer, sizeof buffer, 0));
            if (n <= 0) { CLOSE_SOCKET(connection); return; }
            data.append(buffer, static_cast<std::size_t>(n));
        }
        Request request;
        std::string head = data.substr(0, headerEnd);
        std::size_t lineEnd = head.find("\r\n");
        std::string requestLine = head.substr(0, lineEnd);
        std::size_t space = requestLine.find(' ');
        request.method = requestLine.substr(0, space);
        request.path = requestLine.substr(space + 1, requestLine.rfind(' ') - space - 1);
        std::size_t contentLength = 0;
        bool expectContinue = false;
        std::size_t position = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (position < head.size()) {
            std::size_t next = head.find("\r\n", position);
            if (next == std::string::npos) next = head.size();
            std::string line = head.substr(position, next - position);
            request.headerLines.push_back(line);
            std::size_t colon = line.find(':');
            std::string name = lower(line.substr(0, colon));
            std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            if (name == "content-length") contentLength = static_cast<std::size_t>(std::atol(value.c_str()));
            if (name == "authorization") request.authorization = value;
            if (name == "content-type") request.contentType = value;
            if (name == "user-agent") request.userAgent = value;
            if (name == "expect") expectContinue = true;
            position = next + 2;
        }
        if (expectContinue) {
            const char *go = "HTTP/1.1 100 Continue\r\n\r\n";
            ::send(connection, go, static_cast<int>(std::strlen(go)), 0);
        }
        std::string body = data.substr(headerEnd + 4);
        while (body.size() < contentLength) {
            int n = static_cast<int>(::recv(connection, buffer, sizeof buffer, 0));
            if (n <= 0) break;
            body.append(buffer, static_cast<std::size_t>(n));
        }
        request.body = body;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_for(lock, std::chrono::seconds(10), [&] { return !held; });
            requests.push_back(request);
            changed.notify_all();
        }
        std::string response = "HTTP/1.1 " + std::to_string(status.load()) + " Stub\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(connection, response.data(), static_cast<int>(response.size()), 0);
        CLOSE_SOCKET(connection);
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Request> requests;
    bool held;
    std::atomic<bool> stopping;
    socket_t listener;
    int port;
    std::thread acceptor;
    std::vector<std::thread> workers;
};

struct Log {
    std::mutex mutex;
    std::vector<std::string> lines;
    trace_client::Logger logger() {
        return [this](const std::string &line) {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(line);
        };
    }
    std::size_t count(const std::string &needle) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t n = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) if (lines[i].find(needle) != std::string::npos) ++n;
        return n;
    }
};

static void cleanEnvironment() {
    // The machine running the tests may itself have opted out; every test
    // starts from a clean environment and sets what it needs.
    setEnv("TRACE_USAGE_REPORTING", NULL);
    setEnv("DO_NOT_TRACK", NULL);
}

// ---------------------------------------------------------------- tests

static void reportPostsTheEventToTheMetricsEndpointWithTheKey() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "secret-key");
    CHECK(client.isEnabled());
    client.report("startup");
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    CHECK_EQ(1u, got.size());
    if (got.empty()) return;
    CHECK_EQ(std::string("POST"), got[0].method);
    CHECK_EQ(std::string("/api/metrics"), got[0].path);
    CHECK_EQ(std::string("Bearer secret-key"), got[0].authorization);
    CHECK_EQ(std::string("application/json; charset=utf-8"), got[0].contentType);
    CHECK_EQ(std::string("{\"application\":\"MyGame\",\"name\":\"startup\",\"tags\":{\"version\":\"1.2.3\"}}"), got[0].body);
}

static void reportCarriesValueAndTagsWhenGiven() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl() + "/", "MyGame", "1.2.3", "k");
    client.report("command", 1.5, {{"name", "home"}, {"version", "1.0"}});
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    if (got.empty()) { CHECK(!got.empty()); return; }
    CHECK_EQ(std::string("/api/metrics"), got[0].path); // trailing slash on the base URL dropped
    CHECK_EQ(std::string("{\"application\":\"MyGame\",\"name\":\"command\",\"value\":1.5,"
                         "\"tags\":{\"name\":\"home\",\"version\":\"1.0\"}}"), got[0].body);
}

static void userAgentNamesTheClientVersion() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    client.report("startup");
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    if (got.empty()) { CHECK(!got.empty()); return; }
    CHECK_EQ(std::string("trace-client-cpp/" TRACE_CLIENT_VERSION " (MyGame)"), got[0].userAgent);
}

static void reportReturnsBeforeTheServerAnswers() {
    StubServer server;
    server.hold();
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) client.report("startup");
    CHECK(secondsSince(start) < 0.1);
    server.release();
    client.close();
}

static void reportDoesNotThrowWhenNothingIsListening() {
    int port;
    {
        StubServer closed; // bind a port, then let it go
        port = std::atoi(closed.baseUrl().substr(closed.baseUrl().rfind(':') + 1).c_str());
    }
    Log log;
    trace_client::TraceClient client("http://127.0.0.1:" + std::to_string(port), "MyGame", "1.2.3", "k", true, log.logger());
    client.report("startup");
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    client.close();
    CHECK(secondsSince(start) < trace_client::TIMEOUT_SECONDS + 1);
    CHECK_EQ(1u, log.count("could not deliver"));
}

static void rejectedKeyIsLoggedNotThrown() {
    StubServer server;
    server.status = 401;
    Log log;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "wrong", true, log.logger());
    client.report("startup");
    client.close();
    CHECK_EQ(1u, server.received().size());
    CHECK_EQ(1u, log.count("answered 401"));
}

static void missingCurlIsAnUnreachableServer() {
#if defined(TRACE_CLIENT_POSIX)
    StubServer server;
    const char *original = std::getenv("PATH");
    std::string path = original ? original : "";
    setEnv("PATH", "/nonexistent-trace-client-test");
    Log log;
    {
        trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k", true, log.logger());
        CHECK(client.isEnabled());
        client.report("startup");
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        client.close();
        CHECK(secondsSince(start) < 1.0);
    }
    setEnv("PATH", path.c_str());
    CHECK_EQ(0u, server.received().size());
    CHECK_EQ(1u, log.count("could not"));
#endif
}

static void disabledClientSendsNothing() {
    StubServer server;
    {
        trace_client::TraceClient off(server.baseUrl(), "MyGame", "1.2.3", "k", false);
        CHECK(!off.isEnabled());
        CHECK_EQ(std::string("config"), off.disabledReason());
        off.report("startup");
        trace_client::TraceClient keyless(server.baseUrl(), "MyGame", "1.2.3", "  ");
        CHECK(!keyless.isEnabled());
        CHECK_EQ(std::string("no key"), keyless.disabledReason());
        keyless.report("startup");
        trace_client::TraceClient nothing;
        CHECK(!nothing.isEnabled());
        CHECK_EQ(std::string("config"), nothing.disabledReason());
        nothing.report("startup");
        nothing.close();
        trace_client::TraceClient unnamed(server.baseUrl(), " ", "1.2.3", "k");
        CHECK_EQ(std::string("unavailable"), unnamed.disabledReason());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(0u, server.received().size());
}

static void disabledReasonIsEmptyWhenTheClientReports() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    CHECK(client.disabledReason().empty());
}

static void environmentOptOutsDisableForEveryAcceptedValue() {
    StubServer server;
    const char *off[] = {"off", "OFF", "false", "0", "no", " No "};
    for (std::size_t i = 0; i < sizeof off / sizeof off[0]; ++i) {
        cleanEnvironment();
        setEnv("TRACE_USAGE_REPORTING", off[i]);
        trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
        CHECK(!client.isEnabled());
        CHECK_EQ(std::string("environment"), client.disabledReason());
        client.report("startup");
    }
    const char *dnt[] = {"1", "true", "TRUE", "yes"};
    for (std::size_t i = 0; i < sizeof dnt / sizeof dnt[0]; ++i) {
        cleanEnvironment();
        setEnv("DO_NOT_TRACK", dnt[i]);
        trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
        CHECK(!client.isEnabled());
        CHECK_EQ(std::string("environment"), client.disabledReason());
    }
    cleanEnvironment();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(0u, server.received().size());
}

static void otherEnvironmentValuesLeaveTheProgramSettingInCharge() {
    StubServer server;
    const char *values[] = {"on", "true", "1", "", "maybe"};
    for (std::size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        cleanEnvironment();
        setEnv("TRACE_USAGE_REPORTING", values[i]);
        setEnv("DO_NOT_TRACK", i == 0 ? "0" : "false");
        trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
        CHECK(client.isEnabled());
    }
    cleanEnvironment();
}

static void environmentWinsOverTheConfigFlagAndTheKey() {
    setEnv("DO_NOT_TRACK", "1");
    trace_client::TraceClient off("http://127.0.0.1:9", "MyGame", "1.2.3", "k", false);
    CHECK_EQ(std::string("environment"), off.disabledReason());
    trace_client::TraceClient keyless("http://127.0.0.1:9", "MyGame", "1.2.3", "");
    CHECK_EQ(std::string("environment"), keyless.disabledReason());
    CHECK(trace_client::environmentOptsOut());
    cleanEnvironment();
    CHECK(!trace_client::environmentOptsOut());
}

static void reportIgnoresABlankName() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    client.report("");
    client.report("   ");
    client.close();
    CHECK_EQ(0u, server.received().size());
}

static void jsonIsEscapedAndCleaned() {
    using trace_client::detail::json;
    CHECK_EQ(std::string("{\"application\":\"A\\\"pp\",\"name\":\"n\\\\\",\"tags\":{\"ok\":\"line\\nbreak\\t\\u0001\"}}"),
             json("A\"pp", "n\\", false, 0, {{"ok", "line\nbreak\t\x01"}}));
    // NaN and infinities are not JSON numbers; they are left out.
    CHECK_EQ(std::string("{\"application\":\"A\",\"name\":\"n\"}"), json("A", "n", true, std::nan(""), {}));
    CHECK_EQ(std::string("{\"application\":\"A\",\"name\":\"n\"}"), json("A", "n", true, HUGE_VAL, {}));
    CHECK_EQ(std::string("{\"application\":\"A\",\"name\":\"n\",\"value\":0.1}"), json("A", "n", true, 0.1, {}));
    CHECK_EQ(std::string("{\"application\":\"A\",\"name\":\"n\",\"value\":-3}"), json("A", "n", true, -3.0, {}));
    // UTF-8 passes through; invalid bytes become U+FFFD instead of costing the report.
    CHECK_EQ(std::string("{\"application\":\"A\",\"name\":\"caf\xC3\xA9 \xEF\xBF\xBD\"}"),
             json("A", "caf\xC3\xA9 \xFF", false, 0, {}));
    // A blank tag key is not a tag the server accepts.
    CHECK_EQ(std::string("{\"application\":\"A\",\"name\":\"n\"}"), json("A", "n", false, 0, {{" ", "x"}}));
}

static void tagsAreHeldToTheServerLimits() {
    using trace_client::detail::json;
    trace_client::Tags many;
    for (int i = 0; i < 40; ++i) many["k" + std::to_string(100 + i)] = "v";
    std::string body = json("A", "n", false, 0, many);
    std::size_t count = 0;
    for (std::size_t at = body.find("\"k1"); at != std::string::npos; at = body.find("\"k1", at + 1)) ++count;
    CHECK_EQ(trace_client::MAX_TAGS, count);

    std::string longValue(300, 'x');
    body = json("A", std::string(300, 'n'), false, 0, {{"version", longValue}});
    CHECK(body.find("\"" + std::string(255, 'x') + "\"") != std::string::npos);
    CHECK(body.find(std::string(256, 'x')) == std::string::npos);
    CHECK(body.find("\"" + std::string(255, 'n') + "\"") != std::string::npos);
    // Truncation never splits a multi-byte character.
    std::string accents;
    for (int i = 0; i < 200; ++i) accents += "\xC3\xA9";
    std::string cut = trace_client::detail::cleanUtf8(accents, 255);
    CHECK_EQ(254u, cut.size());
}

static void cleanUtf8ReplacesEveryMalformedSequence() {
    using trace_client::detail::cleanUtf8;
    const std::string fffd = "\xEF\xBF\xBD";
    // Well-formed text of every length passes through, NUL included.
    CHECK_EQ(std::string("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"),
             cleanUtf8("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", std::string::npos));
    CHECK_EQ(std::string("a\0b", 3), cleanUtf8(std::string("a\0b", 3), std::string::npos));
    // Each byte of a sequence that cannot be decoded costs one U+FFFD.
    CHECK_EQ(fffd + fffd, cleanUtf8("\xC0\xAF", std::string::npos));              // overlong, 2 bytes
    CHECK_EQ(fffd + fffd + fffd, cleanUtf8("\xE0\x80\xAF", std::string::npos));   // overlong, 3 bytes
    CHECK_EQ(fffd + fffd + fffd, cleanUtf8("\xED\xA0\x80", std::string::npos));   // UTF-16 surrogate
    CHECK_EQ(fffd + fffd + fffd + fffd, cleanUtf8("\xF4\x90\x80\x80", std::string::npos)); // past U+10FFFF
    CHECK_EQ(fffd + "x", cleanUtf8("\x80x", std::string::npos));                  // stray continuation
    CHECK_EQ("a" + fffd + fffd, cleanUtf8("a\xE2\x82", std::string::npos));      // cut off at the end
    CHECK_EQ(fffd + "A", cleanUtf8("\xC3" "A", std::string::npos));               // lead byte, no continuation
}

static void cleanUtf8NeverExceedsTheByteLimit() {
    using trace_client::detail::cleanUtf8;
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK_EQ(std::string(), cleanUtf8("abc", 0));
    CHECK_EQ(std::string("ab"), cleanUtf8("abc", 2));
    // A replacement is three bytes, and is left out rather than cut.
    CHECK_EQ(std::string(), cleanUtf8("\xFF", 2));
    CHECK_EQ(fffd, cleanUtf8("\xFF\xFF", 5));
    // A four-byte character that does not fit is left out whole.
    CHECK_EQ(std::string("abc"), cleanUtf8("abc\xF0\x9F\x98\x80", 6));
    CHECK_EQ(std::string("abc\xF0\x9F\x98\x80"), cleanUtf8("abc\xF0\x9F\x98\x80", 7));
}

static void curlQuoteKeepsEveryValueOnOneConfigLine() {
    using trace_client::detail::curlQuote;
    CHECK_EQ(std::string("\"\""), curlQuote(""));
    CHECK_EQ(std::string("\"a\\\"b\\\\c\""), curlQuote("a\"b\\c"));
    CHECK_EQ(std::string("\"\\n\\r\\t\\v\""), curlQuote("\n\r\t\v"));
    // Control characters curl has no escape for are dropped; the rest is kept.
    CHECK_EQ(std::string("\"xy\""), curlQuote(std::string("x\x01\x1f\b\f\0y", 7)));
    CHECK_EQ(std::string("\"caf\xC3\xA9 \x7F\""), curlQuote("caf\xC3\xA9 \x7F"));
    // Whatever the bytes, the result is one quoted line: no raw control
    // character, and no quote that is not escaped except the two around it.
    std::string every;
    for (int c = 0; c < 256; ++c) every += static_cast<char>(c);
    std::string quoted = curlQuote(every);
    for (std::size_t i = 0; i < quoted.size(); ++i) {
        CHECK(static_cast<unsigned char>(quoted[i]) >= 0x20);
    }
    for (std::size_t i = 1; i + 1 < quoted.size(); ++i) {
        if (quoted[i] == '\\') { ++i; continue; }
        CHECK(quoted[i] != '"');
    }
    CHECK_EQ('"', quoted[0]);
    CHECK_EQ('"', quoted[quoted.size() - 1]);
}

static void headerSafeDropsControlCharactersOnly() {
    using trace_client::detail::headerSafe;
    CHECK_EQ(std::string("abc d\xC3\xA9"), headerSafe("a\r\nb\tc\x7F d\xC3\xA9"));
    CHECK_EQ(std::string("Bearer"), headerSafe(std::string("Bear\0er", 7)));
    CHECK_EQ(std::string(), headerSafe("\x01\x1f\x7F"));
}

static void parseStatusReadsCurlsWriteOutOrReportsNoAnswer() {
#if !defined(TRACE_CLIENT_USE_LIBCURL) && !defined(__EMSCRIPTEN__)
    using trace_client::detail::parseStatus;
    CHECK_EQ(201, parseStatus("201"));
    CHECK_EQ(401, parseStatus(" 401\r\n"));
    CHECK_EQ(999, parseStatus("999"));
    // curl writes 000 when no response came back at all.
    CHECK_EQ(-1, parseStatus("000"));
    CHECK_EQ(-1, parseStatus(""));
    CHECK_EQ(-1, parseStatus(" \n"));
    CHECK_EQ(-1, parseStatus("1000"));
    CHECK_EQ(-1, parseStatus("20l"));
    CHECK_EQ(-1, parseStatus("HTTP/1.1 201"));
#endif
}

static void nothingTheProgramPassesCanChangeWhereOrWhatCurlSends() {
    StubServer server;
    // A newline in any value would, unescaped, start a new curl option.
    std::string sneaky = "My\"Game\nurl = \"http://127.0.0.1:1/\"\n";
    trace_client::TraceClient client(server.baseUrl(), sneaky, "1.2.3", "k\r\nX-Injected: 1");
    client.report("startup\n", {{"t\\", "\"\n-o /tmp/x"}});
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    if (got.empty()) { CHECK(!got.empty()); return; }
    CHECK_EQ(std::string("Bearer kX-Injected: 1"), got[0].authorization);
    for (std::size_t i = 0; i < got[0].headerLines.size(); ++i) {
        CHECK(lower(got[0].headerLines[i]).find("x-injected") != 0);
    }
    CHECK_EQ(trace_client::detail::json("My\"Game\nurl = \"http://127.0.0.1:1/\"", "startup\n", false, 0,
                                        {{"t\\", "\"\n-o /tmp/x"}, {"version", "1.2.3"}}),
             got[0].body);
}

static void aNewlineInTheBaseUrlCannotAddACurlOption() {
    StubServer server;
    // Unescaped, the second line would be an option of its own.
    trace_client::TraceClient client(server.baseUrl() + "\nheader = X-Injected:1\n#", "MyGame", "1.2.3", "k");
    client.report("startup");
    client.close();
    std::vector<Request> got = server.received();
    for (std::size_t r = 0; r < got.size(); ++r) {
        for (std::size_t i = 0; i < got[r].headerLines.size(); ++i) {
            CHECK(lower(got[r].headerLines[i]).find("x-injected") != 0);
        }
    }
}

static void queueIsBoundedAndDropsRatherThanGrows() {
    StubServer server;
    server.hold(); // the sender waits on the first report
    Log log;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k", true, log.logger());
    const std::size_t flood = trace_client::QUEUE_CAPACITY * 3;
    for (std::size_t i = 0; i < flood; ++i) client.report("flood");
    CHECK(log.count("queue full") >= flood - trace_client::QUEUE_CAPACITY - 1);
    server.release();
    client.close(1.0);
}

static void closeSendsWhatWasJustQueuedBeforeStopping() {
    // A CLI reports once and exits at once; without draining, the event
    // races the sender thread. 20 back-to-back report()+close() pairs make
    // a lost one visible.
    StubServer server;
    for (int i = 0; i < 20; ++i) {
        trace_client::TraceClient client(server.baseUrl(), "MyCli", "1.2.3", "k");
        client.report("startup", {{"run", std::to_string(i)}});
        client.close();
    }
    CHECK_EQ(20u, server.received().size());
}

static void destructorClosesToo() {
    StubServer server;
    {
        trace_client::TraceClient client(server.baseUrl(), "MyCli", "1.2.3", "k");
        client.report("startup");
    }
    CHECK_EQ(1u, server.received().size());
}

static void closeReturnsWithinTheTimeoutWhenTheServerHangs() {
    StubServer server;
    server.hold(); // never answers while held
    trace_client::TraceClient client(server.baseUrl(), "MyCli", "1.2.3", "k");
    client.report("startup");
    client.report("second");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    client.close(1.0);
    CHECK(secondsSince(start) < 2.0);
    CHECK(!client.isEnabled());
    server.release();
}

static void reportTagsEveryEventWithTheProgramVersionTrimmed() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", " 2.0.0-SNAPSHOT ", "k");
    CHECK(client.isEnabled());
    client.report("command", {{"name", "home"}});
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    if (got.empty()) { CHECK(!got.empty()); return; }
    CHECK_EQ(std::string("{\"application\":\"MyGame\",\"name\":\"command\","
                         "\"tags\":{\"name\":\"home\",\"version\":\"2.0.0-SNAPSHOT\"}}"), got[0].body);
    // the program's version is a tag; the User-Agent still names the client's
    CHECK_EQ(std::string("trace-client-cpp/" TRACE_CLIENT_VERSION " (MyGame)"), got[0].userAgent);
}

static void anEventsOwnVersionTagWinsOverTheProgramVersion() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    trace_client::Tags tags;
    tags["version"] = "9.9.9";
    client.report("startup", tags);
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    if (got.empty()) { CHECK(!got.empty()); return; }
    CHECK_EQ(std::string("{\"application\":\"MyGame\",\"name\":\"startup\",\"tags\":{\"version\":\"9.9.9\"}}"),
             got[0].body);
    CHECK_EQ(1u, tags.size());
    CHECK_EQ(std::string("9.9.9"), tags["version"]);
}

static void theCallersTagsAreNeverModified() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    trace_client::Tags tags;
    tags["name"] = "home";
    client.report("command", tags);
    client.report("command", 1.0, tags);
    CHECK(server.waitFor(2, 10));
    client.close();
    CHECK_EQ(1u, tags.size());
    CHECK(tags.find("version") == tags.end());

    trace_client::Tags merged = trace_client::detail::withVersion(tags, "1.2.3");
    CHECK_EQ(1u, tags.size());
    CHECK_EQ(std::string("1.2.3"), merged["version"]);
    CHECK_EQ(std::string("home"), merged["name"]);
    CHECK_EQ(std::string("1.2.3"), trace_client::detail::withVersion(trace_client::Tags(), "1.2.3")["version"]);
}

static void aBlankOrOverlongVersionIsRejected() {
    // Nothing in the client throws: a bad version is a bad argument like a
    // blank application name -- the client reports nothing, reason "unavailable".
    StubServer server;
    {
        trace_client::TraceClient empty(server.baseUrl(), "MyGame", "", "k");
        CHECK(!empty.isEnabled());
        CHECK_EQ(std::string("unavailable"), empty.disabledReason());
        empty.report("startup");
        trace_client::TraceClient blank(server.baseUrl(), "MyGame", " \t ", "k");
        CHECK(!blank.isEnabled());
        CHECK_EQ(std::string("unavailable"), blank.disabledReason());
        blank.report("startup");
        trace_client::TraceClient overlong(server.baseUrl(), "MyGame", std::string(trace_client::MAX_LENGTH + 1, '9'), "k");
        CHECK(!overlong.isEnabled());
        CHECK_EQ(std::string("unavailable"), overlong.disabledReason());
        overlong.report("startup");
        // exactly the limit, once trimmed, is accepted
        trace_client::TraceClient longest(server.baseUrl(), "MyGame",
                                          "  " + std::string(trace_client::MAX_LENGTH, '9') + "  ", "k");
        CHECK(longest.isEnabled());
        longest.close();
    }
    CHECK_EQ(0u, server.received().size());
}

static void closeIsPromptAndIdempotent() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k");
    client.report("startup");
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    client.close();
    client.close();
    CHECK(secondsSince(start) < trace_client::TIMEOUT_SECONDS + 1);
    CHECK(!client.isEnabled());
    CHECK(client.disabledReason().empty());
    client.report("after-close"); // a no-op, not a crash
}

// ---------------------------------------------------------------- installation ID

static std::string tempBase() {
    std::string base = trace_client::detail::getEnvironment("TMPDIR");
    if (base.empty()) base = trace_client::detail::getEnvironment("TEMP");
    if (base.empty()) base = "/tmp";
    return base + "/trace-client-test-" + trace_client::detail::randomUuid();
}

static bool exists(const std::string &path) {
    return trace_client::detail::pathKind(path) != trace_client::detail::PATH_MISSING;
}

static std::string readFile(const std::string &path) {
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

static void writeFile(const std::string &path, const std::string &content) {
    trace_client::detail::makeParentDirectories(path);
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    out << content;
}

static void removeDirectory(const std::string &path) {
#if defined(_WIN32)
    _rmdir(path.c_str());
#else
    ::rmdir(path.c_str());
#endif
}

static bool isUuid(const std::string &id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return id[14] == '4' && std::string("89ab").find(id[19]) != std::string::npos;
}

static std::string installTagIn(const std::string &body) {
    std::string marker = "\"install\":\"";
    std::size_t at = body.find(marker);
    if (at == std::string::npos) return std::string();
    at += marker.size();
    return body.substr(at, body.find('"', at) - at);
}

static void randomUuidIsAVersion4Uuid() {
    std::string a = trace_client::detail::randomUuid();
    std::string b = trace_client::detail::randomUuid();
    CHECK(isUuid(a));
    CHECK(isUuid(b));
    CHECK(a != b);
}

static void installIdFromFilePersistsOnceAndReusesIt() {
    std::string base = tempBase();
    std::string path = base + "/nested/dir/install-id";
    std::string first = trace_client::TraceClient::installIdFromFile(path);
    CHECK(isUuid(first));
    CHECK_EQ(first + "\n", readFile(path));
    std::string second = trace_client::TraceClient::installIdFromFile(path);
    CHECK_EQ(first, second);
    CHECK_EQ(first + "\n", readFile(path)); // read, not rewritten
    std::remove(path.c_str());
    removeDirectory(base + "/nested/dir");
    removeDirectory(base + "/nested");
    removeDirectory(base);
}

static void installIdFromFileReadsTheFirstValidLine() {
    std::string base = tempBase();
    std::string path = base + "/install-id";
    std::string content = "\n   \nnot valid!\r\n  my-id_1.0  \r\nsecond-id\n";
    writeFile(path, content);
    CHECK_EQ(std::string("my-id_1.0"), trace_client::TraceClient::installIdFromFile(path));
    CHECK_EQ(content, readFile(path));
    // a file with no usable line gets a fresh ID written to it
    writeFile(path, "has spaces\n" + std::string(trace_client::MAX_LENGTH + 1, 'x') + "\n");
    std::string fresh = trace_client::TraceClient::installIdFromFile(path);
    CHECK(isUuid(fresh));
    CHECK_EQ(fresh + "\n", readFile(path));
    std::remove(path.c_str());
    removeDirectory(base);
}

static void installIdFromFileFallsBackToMemoryWhenThePathIsUnusable() {
    // A path under a regular file can never be created, even as root.
    std::string base = tempBase();
    std::string blocker = base + "/a-file";
    writeFile(blocker, "keep me\n");
    std::string a = trace_client::TraceClient::installIdFromFile(blocker + "/install-id");
    std::string b = trace_client::TraceClient::installIdFromFile(blocker + "/install-id");
    CHECK(isUuid(a));
    CHECK(isUuid(b));
    CHECK(a != b); // nothing persisted
    CHECK_EQ(std::string("keep me\n"), readFile(blocker));
    // A path that is a directory is not readable as a file and is left alone.
    std::string directory = base + "/a-directory";
    trace_client::detail::makeParentDirectories(directory + "/");
    CHECK(isUuid(trace_client::TraceClient::installIdFromFile(directory)));
    CHECK(trace_client::detail::pathKind(directory) == trace_client::detail::PATH_OTHER);
    // and an empty path is no path
    CHECK(isUuid(trace_client::TraceClient::installIdFromFile("")));
    std::remove(blocker.c_str());
    removeDirectory(directory);
    removeDirectory(base);
}

static void everyEventCarriesTheInstallIdFromTheFile() {
    StubServer server;
    std::string base = tempBase();
    std::string path = base + "/install-id";
    std::string id;
    {
        trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k",
                                         trace_client::InstallId::fromFile(path));
        CHECK(client.isEnabled());
        id = client.installId();
        CHECK(isUuid(id));
        CHECK_EQ(id + "\n", readFile(path));
        client.report("startup");
        client.report("command", {{"name", "home"}});
        CHECK(server.waitFor(2, 10));
        client.close();
        CHECK_EQ(id, client.installId()); // unchanged by close()
    }
    std::vector<Request> got = server.received();
    CHECK_EQ(2u, got.size());
    for (std::size_t i = 0; i < got.size(); ++i) CHECK_EQ(id, installTagIn(got[i].body));
    if (!got.empty()) {
        CHECK_EQ(std::string("{\"application\":\"MyGame\",\"name\":\"startup\",\"tags\":{\"install\":\"" + id
                             + "\",\"version\":\"1.2.3\"}}"), got[0].body);
    }
    // a second client on the same file reuses the ID
    trace_client::TraceClient again(server.baseUrl(), "MyGame", "1.2.3", "k",
                                    trace_client::InstallId::fromFile(path));
    CHECK_EQ(id, again.installId());
    again.close();
    std::remove(path.c_str());
    removeDirectory(base);
}

static void aDisabledClientNeverMakesUpOrWritesAnInstallId() {
    StubServer server;
    std::string base = tempBase();
    std::string path = base + "/install-id";
    {
        trace_client::TraceClient off(server.baseUrl(), "MyGame", "1.2.3", "k",
                                      trace_client::InstallId::fromFile(path), false);
        CHECK(!off.isEnabled());
        CHECK(off.installId().empty());
        trace_client::TraceClient keyless(server.baseUrl(), "MyGame", "1.2.3", " ",
                                          trace_client::InstallId::fromFile(path));
        CHECK(keyless.installId().empty());
        trace_client::TraceClient unnamed(server.baseUrl(), " ", "1.2.3", "k",
                                          trace_client::InstallId::fromFile(path));
        CHECK(unnamed.installId().empty());
        setEnv("TRACE_USAGE_REPORTING", "off");
        trace_client::TraceClient environment(server.baseUrl(), "MyGame", "1.2.3", "k",
                                              trace_client::InstallId::fromFile(path));
        CHECK_EQ(std::string("environment"), environment.disabledReason());
        CHECK(environment.installId().empty());
        setEnv("TRACE_USAGE_REPORTING", NULL);
        setEnv("DO_NOT_TRACK", "1");
        trace_client::TraceClient dnt(server.baseUrl(), "MyGame", "1.2.3", "k",
                                      trace_client::InstallId::of("explicit"));
        CHECK(dnt.installId().empty());
        setEnv("DO_NOT_TRACK", NULL);
        trace_client::TraceClient nothing;
        CHECK(nothing.installId().empty());
    }
    CHECK(!exists(path));
    CHECK(!exists(base));
    CHECK_EQ(0u, server.received().size());
}

static void anExplicitInstallIdIsTrimmedAndWinsOverTheFile() {
    StubServer server;
    std::string base = tempBase();
    std::string path = base + "/install-id";
    trace_client::InstallId install;
    install.id = "  my-own-id  ";
    install.file = path;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k", install);
    CHECK_EQ(std::string("my-own-id"), client.installId());
    client.report("startup");
    CHECK(server.waitFor(1, 10));
    client.close();
    CHECK(!exists(path)); // the file is not consulted, so not written
    std::vector<Request> got = server.received();
    if (!got.empty()) CHECK_EQ(std::string("my-own-id"), installTagIn(got[0].body));
    trace_client::TraceClient viaOf(server.baseUrl(), "MyGame", "1.2.3", "k", trace_client::InstallId::of("abc"));
    CHECK_EQ(std::string("abc"), viaOf.installId());
}

static void aBlankInstallIdMeansNoneAndAnOverlongOneIsRejected() {
    StubServer server;
    {
        trace_client::TraceClient blank(server.baseUrl(), "MyGame", "1.2.3", "k", trace_client::InstallId::of(" \t "));
        CHECK(blank.isEnabled());
        CHECK(blank.installId().empty());
        blank.report("startup");
        CHECK(server.waitFor(1, 10));
        blank.close();
        trace_client::TraceClient overlong(server.baseUrl(), "MyGame", "1.2.3", "k",
                                           trace_client::InstallId::of(std::string(trace_client::MAX_LENGTH + 1, 'x')));
        CHECK(!overlong.isEnabled());
        CHECK_EQ(std::string("unavailable"), overlong.disabledReason());
        CHECK(overlong.installId().empty());
        overlong.report("startup");
        trace_client::TraceClient longest(server.baseUrl(), "MyGame", "1.2.3", "k",
                                          trace_client::InstallId::of(" " + std::string(trace_client::MAX_LENGTH, 'x') + " "));
        CHECK(longest.isEnabled());
        CHECK_EQ(trace_client::MAX_LENGTH, longest.installId().size());
        longest.close();
        trace_client::TraceClient none(server.baseUrl(), "MyGame", "1.2.3", "k");
        CHECK(none.installId().empty());
    }
    std::vector<Request> got = server.received();
    CHECK_EQ(1u, got.size());
    if (!got.empty()) {
        CHECK_EQ(std::string("{\"application\":\"MyGame\",\"name\":\"startup\",\"tags\":{\"version\":\"1.2.3\"}}"),
                 got[0].body);
    }
}

static void anEventsOwnInstallTagWins() {
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k", trace_client::InstallId::of("configured"));
    trace_client::Tags tags;
    tags["install"] = "per-event";
    client.report("startup", tags);
    CHECK(server.waitFor(1, 10));
    client.close();
    std::vector<Request> got = server.received();
    if (!got.empty()) CHECK_EQ(std::string("per-event"), installTagIn(got[0].body));
    CHECK_EQ(1u, tags.size()); // the caller's tags are never modified
}

static void theInstallTagNeverPassesTheTagCap() {
    trace_client::Tags full;
    for (std::size_t i = 0; i < trace_client::MAX_TAGS; ++i) full["t" + std::to_string(100 + i)] = "v";
    CHECK(trace_client::detail::withInstall(full, "id").count("install") == 0);
    CHECK_EQ(trace_client::MAX_TAGS, trace_client::detail::withInstall(full, "id").size());
    trace_client::Tags room(full);
    room.erase(room.begin());
    trace_client::Tags merged = trace_client::detail::withInstall(room, "id");
    CHECK_EQ(trace_client::MAX_TAGS, merged.size());
    CHECK_EQ(std::string("id"), merged["install"]);
    CHECK(trace_client::detail::withInstall(room, "").count("install") == 0);

    // Through the client: 31 of the event's own tags plus "version" fill the cap.
    StubServer server;
    trace_client::TraceClient client(server.baseUrl(), "MyGame", "1.2.3", "k", trace_client::InstallId::of("configured"));
    trace_client::Tags many;
    for (std::size_t i = 0; i + 1 < trace_client::MAX_TAGS; ++i) many["t" + std::to_string(100 + i)] = "v";
    client.report("startup", many);
    many.erase(many.begin());
    client.report("second", many);
    CHECK(server.waitFor(2, 10));
    client.close();
    std::vector<Request> got = server.received();
    CHECK_EQ(2u, got.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        bool first = got[i].body.find("\"name\":\"startup\"") != std::string::npos;
        CHECK_EQ(first ? std::string() : std::string("configured"), installTagIn(got[i].body));
        CHECK(got[i].body.find("\"version\":\"1.2.3\"") != std::string::npos);
    }
}

int main() {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct Test { const char *name; void (*run)(); };
    const Test tests[] = {
        {"reportPostsTheEventToTheMetricsEndpointWithTheKey", reportPostsTheEventToTheMetricsEndpointWithTheKey},
        {"reportCarriesValueAndTagsWhenGiven", reportCarriesValueAndTagsWhenGiven},
        {"userAgentNamesTheClientVersion", userAgentNamesTheClientVersion},
        {"reportReturnsBeforeTheServerAnswers", reportReturnsBeforeTheServerAnswers},
        {"reportDoesNotThrowWhenNothingIsListening", reportDoesNotThrowWhenNothingIsListening},
        {"rejectedKeyIsLoggedNotThrown", rejectedKeyIsLoggedNotThrown},
        {"missingCurlIsAnUnreachableServer", missingCurlIsAnUnreachableServer},
        {"disabledClientSendsNothing", disabledClientSendsNothing},
        {"disabledReasonIsEmptyWhenTheClientReports", disabledReasonIsEmptyWhenTheClientReports},
        {"environmentOptOutsDisableForEveryAcceptedValue", environmentOptOutsDisableForEveryAcceptedValue},
        {"otherEnvironmentValuesLeaveTheProgramSettingInCharge", otherEnvironmentValuesLeaveTheProgramSettingInCharge},
        {"environmentWinsOverTheConfigFlagAndTheKey", environmentWinsOverTheConfigFlagAndTheKey},
        {"reportIgnoresABlankName", reportIgnoresABlankName},
        {"jsonIsEscapedAndCleaned", jsonIsEscapedAndCleaned},
        {"tagsAreHeldToTheServerLimits", tagsAreHeldToTheServerLimits},
        {"cleanUtf8ReplacesEveryMalformedSequence", cleanUtf8ReplacesEveryMalformedSequence},
        {"cleanUtf8NeverExceedsTheByteLimit", cleanUtf8NeverExceedsTheByteLimit},
        {"curlQuoteKeepsEveryValueOnOneConfigLine", curlQuoteKeepsEveryValueOnOneConfigLine},
        {"headerSafeDropsControlCharactersOnly", headerSafeDropsControlCharactersOnly},
        {"parseStatusReadsCurlsWriteOutOrReportsNoAnswer", parseStatusReadsCurlsWriteOutOrReportsNoAnswer},
        {"nothingTheProgramPassesCanChangeWhereOrWhatCurlSends", nothingTheProgramPassesCanChangeWhereOrWhatCurlSends},
        {"aNewlineInTheBaseUrlCannotAddACurlOption", aNewlineInTheBaseUrlCannotAddACurlOption},
        {"queueIsBoundedAndDropsRatherThanGrows", queueIsBoundedAndDropsRatherThanGrows},
        {"closeSendsWhatWasJustQueuedBeforeStopping", closeSendsWhatWasJustQueuedBeforeStopping},
        {"destructorClosesToo", destructorClosesToo},
        {"closeReturnsWithinTheTimeoutWhenTheServerHangs", closeReturnsWithinTheTimeoutWhenTheServerHangs},
        {"closeIsPromptAndIdempotent", closeIsPromptAndIdempotent},
        {"reportTagsEveryEventWithTheProgramVersionTrimmed", reportTagsEveryEventWithTheProgramVersionTrimmed},
        {"anEventsOwnVersionTagWinsOverTheProgramVersion", anEventsOwnVersionTagWinsOverTheProgramVersion},
        {"theCallersTagsAreNeverModified", theCallersTagsAreNeverModified},
        {"aBlankOrOverlongVersionIsRejected", aBlankOrOverlongVersionIsRejected},
        {"randomUuidIsAVersion4Uuid", randomUuidIsAVersion4Uuid},
        {"installIdFromFilePersistsOnceAndReusesIt", installIdFromFilePersistsOnceAndReusesIt},
        {"installIdFromFileReadsTheFirstValidLine", installIdFromFileReadsTheFirstValidLine},
        {"installIdFromFileFallsBackToMemoryWhenThePathIsUnusable", installIdFromFileFallsBackToMemoryWhenThePathIsUnusable},
        {"everyEventCarriesTheInstallIdFromTheFile", everyEventCarriesTheInstallIdFromTheFile},
        {"aDisabledClientNeverMakesUpOrWritesAnInstallId", aDisabledClientNeverMakesUpOrWritesAnInstallId},
        {"anExplicitInstallIdIsTrimmedAndWinsOverTheFile", anExplicitInstallIdIsTrimmedAndWinsOverTheFile},
        {"aBlankInstallIdMeansNoneAndAnOverlongOneIsRejected", aBlankInstallIdMeansNoneAndAnOverlongOneIsRejected},
        {"anEventsOwnInstallTagWins", anEventsOwnInstallTagWins},
        {"theInstallTagNeverPassesTheTagCap", theInstallTagNeverPassesTheTagCap},
    };
    const std::size_t count = sizeof tests / sizeof tests[0];
    for (std::size_t i = 0; i < count; ++i) {
        cleanEnvironment();
        currentTest = tests[i].name;
        int before = failures;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        tests[i].run();
        std::printf("%s %s (%.2fs)\n", failures == before ? "ok  " : "FAIL", tests[i].name, secondsSince(start));
    }
    std::printf("\n%zu tests, %d failed checks\n", count, failures);
    return failures == 0 ? 0 : 1;
}
