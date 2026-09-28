#pragma once

#include <libxml/tree.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webspine::html {

using Node = xmlNodePtr;

// An HTML fragment parsed into a libxml2 document. Nodes detached while
// editing are kept alive until the fragment is destroyed, so snapshots taken
// with elements() never dangle.
class Fragment {
  public:
    explicit Fragment(std::string_view markup);
    ~Fragment();
    Fragment(const Fragment&) = delete;
    Fragment& operator=(const Fragment&) = delete;

    Node root() const { return root_; }
    Node create(std::string_view name, std::string_view text = {});
    void detach(Node node);
    void replace(Node node, Node replacement);
    void unwrap(Node node);
    Node wrap(Node node, std::string_view name);
    std::string xml() const;

  private:
    xmlDocPtr doc_ = nullptr;
    Node root_ = nullptr;
    std::vector<Node> detached_;
};

std::string name(Node node);
void rename(Node node, std::string_view name);
bool is_heading(Node node);
int heading_level(Node node);
std::vector<Node> elements(Node root, const std::function<bool(Node)>& match = {});
std::vector<Node> child_elements(Node node, std::string_view name);
Node first_element(Node root, const std::function<bool(Node)>& match);
Node ancestor(Node node, std::string_view name);

std::optional<std::string> attr(Node node, std::string_view key);
void set_attr(Node node, std::string_view key, std::string_view value);
std::vector<std::string> attr_names(Node node);
void remove_attr(Node node, std::string_view key);

std::string text(Node node);
std::string joined_text(Node node, std::string_view separator);
void set_text(Node node, std::string_view value);
void remove_comments(Node root);
std::string outer_xml(Node node);

}  // namespace webspine::html
