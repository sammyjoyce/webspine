#include "browser.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace docs2epub {
namespace {

using Clock = std::chrono::steady_clock;

int remaining_ms(Clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

std::string chromium_binary() {
    if (const char* value = std::getenv("DOCS2EPUB_CHROMIUM")) return value;
    return "chromium";
}

}  // namespace

Browser::Browser() {
    int input[2];
    int output[2];
    if (pipe2(input, O_CLOEXEC) != 0 || pipe2(output, O_CLOEXEC) != 0) {
        throw std::runtime_error("Could not create Chromium pipes");
    }
    char profile_template[] = "/tmp/docs2epub-chromium-XXXXXX";
    if (!mkdtemp(profile_template)) throw std::runtime_error("Could not create a Chromium profile directory");
    profile_ = profile_template;
    std::string binary = chromium_binary();
    std::vector<std::string> args = {binary,
                                     "--headless=new",
                                     "--remote-debugging-pipe",
                                     "--no-first-run",
                                     "--no-default-browser-check",
                                     "--disable-gpu",
                                     "--disable-extensions",
                                     "--disable-background-networking",
                                     "--disable-dev-shm-usage",
                                     "--allow-file-access-from-files",
                                     "--hide-scrollbars",
                                     "--mute-audio",
                                     "--user-data-dir=" + profile_,
                                     "about:blank"};
    if (geteuid() == 0) args.insert(args.begin() + 1, "--no-sandbox");
    pid_ = fork();
    if (pid_ < 0) throw std::runtime_error("Could not fork Chromium");
    if (pid_ == 0) {
        dup2(input[0], 3);
        dup2(output[1], 4);
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(input[0]);
    close(output[1]);
    to_browser_ = input[1];
    from_browser_ = output[0];

    json target = call("Target.createTarget", {{"url", "about:blank"}}, false);
    json attached = call("Target.attachToTarget", {{"targetId", target.at("targetId")}, {"flatten", true}}, false);
    session_ = attached.at("sessionId").get<std::string>();
    call("Page.enable");
    call("Runtime.enable");
    call("Page.setLifecycleEventsEnabled", {{"enabled", true}});
    frame_ = call("Page.getFrameTree").at("frameTree").at("frame").at("id").get<std::string>();
}

Browser::~Browser() {
    if (to_browser_ >= 0) {
        try {
            call("Browser.close", json::object(), false);
        } catch (...) {
        }
        close(to_browser_);
    }
    if (from_browser_ >= 0) close(from_browser_);
    if (pid_ > 0) {
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
    }
    std::error_code ignored;
    if (!profile_.empty()) std::filesystem::remove_all(profile_, ignored);
}

json Browser::read_message(int timeout_ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        if (auto end = buffer_.find('\0'); end != std::string::npos) {
            std::string message = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            return json::parse(message);
        }
        pollfd descriptor{from_browser_, POLLIN, 0};
        int ready = poll(&descriptor, 1, remaining_ms(deadline));
        if (ready == 0) throw std::runtime_error("Timed out waiting for Chromium");
        if (ready < 0) throw std::runtime_error("Polling Chromium failed");
        char chunk[65536];
        ssize_t count = read(from_browser_, chunk, sizeof chunk);
        if (count <= 0) {
            throw std::runtime_error("Chromium exited. Set DOCS2EPUB_CHROMIUM or run through nix run.");
        }
        buffer_.append(chunk, static_cast<size_t>(count));
    }
}

json Browser::call(const std::string& method, json params, bool in_session) {
    int id = next_id_++;
    json request = {{"id", id}, {"method", method}, {"params", std::move(params)}};
    if (in_session) request["sessionId"] = session_;
    std::string data = request.dump() + '\0';
    for (size_t written = 0; written < data.size();) {
        ssize_t count = write(to_browser_, data.data() + written, data.size() - written);
        if (count <= 0) throw std::runtime_error("Could not write to Chromium");
        written += static_cast<size_t>(count);
    }
    while (true) {
        json message = read_message(90'000);
        if (message.contains("id") && message["id"] == id) {
            if (message.contains("error")) {
                throw std::runtime_error(method + ": " + message["error"].value("message", "CDP error"));
            }
            return message.value("result", json::object());
        }
        if (message.contains("method")) pending_events_.push_back(std::move(message));
    }
}

void Browser::wait_for_lifecycle(const std::string& name, const std::string& loader_id, int timeout_ms) {
    auto matches = [&](const json& message) {
        if (message.value("method", "") != "Page.lifecycleEvent") return false;
        const auto& params = message.at("params");
        return params.value("name", "") == name && params.value("frameId", "") == frame_ &&
               params.value("loaderId", "") == loader_id;
    };
    for (auto it = pending_events_.begin(); it != pending_events_.end(); ++it) {
        if (matches(*it)) {
            pending_events_.erase(pending_events_.begin(), it + 1);
            return;
        }
    }
    pending_events_.clear();
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        json message = read_message(remaining_ms(deadline) + 1);
        if (matches(message)) return;
    }
}

void Browser::set_viewport(int width, int height) {
    call("Emulation.setDeviceMetricsOverride",
         {{"width", width}, {"height", height}, {"deviceScaleFactor", 1}, {"mobile", false}});
}

void Browser::emulate_light_reduced_motion() {
    call("Emulation.setEmulatedMedia",
         {{"features", json::array({{{"name", "prefers-color-scheme"}, {"value", "light"}},
                                    {{"name", "prefers-reduced-motion"}, {"value", "reduce"}}})}});
}

void Browser::navigate(const std::string& url, bool wait_for_network_idle) {
    pending_events_.clear();
    json result = call("Page.navigate", {{"url", url}});
    if (result.contains("errorText") && !result["errorText"].get<std::string>().empty()) {
        throw std::runtime_error("Could not load " + url + ": " + result["errorText"].get<std::string>());
    }
    wait_for_lifecycle(wait_for_network_idle ? "networkIdle" : "load", result.value("loaderId", ""), 60'000);
}

json Browser::evaluate(std::string_view expression) {
    json result = call("Runtime.evaluate", {{"expression", std::string(expression)},
                                            {"awaitPromise", true},
                                            {"returnByValue", true}});
    if (result.contains("exceptionDetails")) {
        const auto& details = result["exceptionDetails"];
        std::string message = details.contains("exception")
                                  ? details["exception"].value("description", "script error")
                                  : details.value("text", "script error");
        throw std::runtime_error("Page script failed: " + message);
    }
    return result.at("result").value("value", json(nullptr));
}

void Browser::sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace docs2epub
