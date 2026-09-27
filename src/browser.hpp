#pragma once

#include "model.hpp"

#include <sys/types.h>

#include <string>
#include <string_view>

namespace docs2epub {

// Headless Chromium driven through the DevTools protocol over
// --remote-debugging-pipe: NUL-delimited JSON on the child's fds 3 and 4.
class Browser {
  public:
    Browser();
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    void set_viewport(int width, int height);
    void emulate_light_reduced_motion();
    void navigate(const std::string& url, bool wait_for_network_idle);
    json evaluate(std::string_view expression);
    void sleep_ms(int milliseconds);

  private:
    json call(const std::string& method, json params = json::object(), bool in_session = true);
    json read_message(int timeout_ms);
    void wait_for_lifecycle(const std::string& name, const std::string& loader_id, int timeout_ms);
    void continue_paused_document(const json& params);

    pid_t pid_ = -1;
    int to_browser_ = -1;
    int from_browser_ = -1;
    int next_id_ = 1;
    std::string session_;
    std::string frame_;
    std::string buffer_;
    std::string profile_;
    std::vector<json> pending_events_;
};

}  // namespace docs2epub
