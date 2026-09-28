#include "html.hpp"

#include "core.hpp"

#include <libxml/HTMLparser.h>
#include <libxml/xmlsave.h>

#include <stdexcept>

namespace webspine::html {
namespace {

const xmlChar* xml_chars(const std::string& value) {
    return reinterpret_cast<const xmlChar*>(value.c_str());
}

std::string to_string(const xmlChar* value) {
    return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}

void collect(Node node, const std::function<bool(Node)>& match, std::vector<Node>& out) {
    for (Node child = node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        if (!match || match(child)) out.push_back(child);
        collect(child, match, out);
    }
}

void collect_text(Node node, std::vector<std::string>& out) {
    for (Node child = node->children; child; child = child->next) {
        if (child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE) {
            out.push_back(to_string(child->content));
        } else if (child->type == XML_ELEMENT_NODE) {
            collect_text(child, out);
        }
    }
}

void cdata_to_text(Node node) {
    for (Node child = node->children; child; child = child->next) {
        if (child->type == XML_CDATA_SECTION_NODE) child->type = XML_TEXT_NODE;
        else if (child->type == XML_ELEMENT_NODE) cdata_to_text(child);
    }
}

std::string save(xmlDocPtr doc, Node node) {
    xmlBufferPtr buffer = xmlBufferCreate();
    xmlSaveCtxtPtr context = xmlSaveToBuffer(buffer, "UTF-8", XML_SAVE_AS_XML | XML_SAVE_NO_DECL);
    xmlSaveTree(context, node);
    xmlSaveClose(context);
    std::string out(reinterpret_cast<const char*>(xmlBufferContent(buffer)), xmlBufferLength(buffer));
    xmlBufferFree(buffer);
    (void)doc;
    return out;
}

}  // namespace

Fragment::Fragment(std::string_view markup) {
    std::string document = "<!DOCTYPE html><html><body>" + std::string(markup) + "</body></html>";
    doc_ = htmlReadMemory(document.data(), static_cast<int>(document.size()), nullptr, "UTF-8",
                          HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (!doc_) throw std::runtime_error("Could not parse HTML fragment");
    root_ = first_element(xmlDocGetRootElement(doc_), [](Node node) { return name(node) == "body"; });
    if (!root_) throw std::runtime_error("Parsed HTML fragment has no body");
    cdata_to_text(root_);
}

Fragment::~Fragment() {
    for (Node node : detached_) xmlFreeNode(node);
    xmlFreeDoc(doc_);
}

Node Fragment::create(std::string_view tag, std::string_view value) {
    Node node = xmlNewDocNode(doc_, nullptr, xml_chars(std::string(tag)), nullptr);
    if (!value.empty()) set_text(node, value);
    return node;
}

void Fragment::detach(Node node) {
    xmlUnlinkNode(node);
    detached_.push_back(node);
}

void Fragment::replace(Node node, Node replacement) {
    xmlAddPrevSibling(node, replacement);
    detach(node);
}

void Fragment::unwrap(Node node) {
    while (node->children) {
        Node child = node->children;
        xmlUnlinkNode(child);
        xmlAddPrevSibling(node, child);
    }
    detach(node);
}

Node Fragment::wrap(Node node, std::string_view tag) {
    Node wrapper = create(tag);
    xmlAddPrevSibling(node, wrapper);
    xmlUnlinkNode(node);
    xmlAddChild(wrapper, node);
    return wrapper;
}

std::string Fragment::xml() const {
    std::string out;
    for (Node child = root_->children; child; child = child->next) out += save(doc_, child);
    return out;
}

std::string name(Node node) { return node && node->type == XML_ELEMENT_NODE ? to_string(node->name) : ""; }

void rename(Node node, std::string_view value) { xmlNodeSetName(node, xml_chars(std::string(value))); }

int heading_level(Node node) {
    auto tag = name(node);
    return tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' ? tag[1] - '0' : 0;
}

bool is_heading(Node node) { return heading_level(node) != 0; }

std::vector<Node> elements(Node root, const std::function<bool(Node)>& match) {
    std::vector<Node> out;
    collect(root, match, out);
    return out;
}

std::vector<Node> child_elements(Node node, std::string_view tag) {
    std::vector<Node> out;
    for (Node child = node->children; child; child = child->next) {
        if (name(child) == tag) out.push_back(child);
    }
    return out;
}

Node first_element(Node root, const std::function<bool(Node)>& match) {
    for (Node child = root ? root->children : nullptr; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        if (match(child)) return child;
        if (Node found = first_element(child, match)) return found;
    }
    return nullptr;
}

Node ancestor(Node node, std::string_view tag) {
    for (Node parent = node->parent; parent; parent = parent->parent) {
        if (name(parent) == tag) return parent;
    }
    return nullptr;
}

std::optional<std::string> attr(Node node, std::string_view key) {
    xmlAttrPtr property = xmlHasProp(node, xml_chars(std::string(key)));
    if (!property) return std::nullopt;
    xmlChar* value = xmlNodeGetContent(reinterpret_cast<Node>(property));
    std::string out = to_string(value);
    xmlFree(value);
    return out;
}

void set_attr(Node node, std::string_view key, std::string_view value) {
    xmlSetProp(node, xml_chars(std::string(key)), xml_chars(std::string(value)));
}

std::vector<std::string> attr_names(Node node) {
    std::vector<std::string> out;
    for (xmlAttrPtr property = node->properties; property; property = property->next) {
        std::string key = to_string(property->name);
        if (property->ns && property->ns->prefix) key = to_string(property->ns->prefix) + ":" + key;
        out.push_back(key);
    }
    return out;
}

void remove_attr(Node node, std::string_view key) {
    std::string wanted(key);
    for (xmlAttrPtr property = node->properties; property;) {
        xmlAttrPtr next = property->next;
        std::string current = to_string(property->name);
        if (property->ns && property->ns->prefix) current = to_string(property->ns->prefix) + ":" + current;
        if (current == wanted) xmlRemoveProp(property);
        property = next;
    }
}

std::string text(Node node) {
    std::vector<std::string> parts;
    collect_text(node, parts);
    std::string out;
    for (const auto& part : parts) out += part;
    return out;
}

std::string joined_text(Node node, std::string_view separator) {
    std::vector<std::string> parts;
    collect_text(node, parts);
    std::string out;
    for (const auto& part : parts) {
        auto stripped = strip_whitespace(part);
        if (stripped.empty()) continue;
        if (!out.empty()) out += separator;
        out += stripped;
    }
    return out;
}

void set_text(Node node, std::string_view value) {
    while (node->children) {
        Node child = node->children;
        xmlUnlinkNode(child);
        xmlFreeNode(child);
    }
    std::string owned(value);
    xmlAddChild(node, xmlNewDocText(node->doc, xml_chars(owned)));
}

void remove_comments(Node root) {
    for (Node child = root->children; child;) {
        Node next = child->next;
        if (child->type == XML_COMMENT_NODE) {
            xmlUnlinkNode(child);
            xmlFreeNode(child);
        } else if (child->type == XML_ELEMENT_NODE) {
            remove_comments(child);
        }
        child = next;
    }
}

std::string outer_xml(Node node) { return save(node->doc, node); }

}  // namespace webspine::html
