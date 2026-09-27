#include "build.hpp"
#include "core.hpp"
#include "scrape.hpp"
#include "validate.hpp"
#include "workspace.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace docs2epub;

namespace {

struct ScrapeFlags {
    std::string url;
    std::string workspace;
    std::optional<std::string> title, language, content_selector, nav_selector;
    int max_pages = 500;
    bool force = false;
};

void emit(const json& value, bool as_json) {
    if (as_json) {
        std::cout << value.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
        return;
    }
    if (!value.is_object()) {
        std::cout << (value.is_string() ? value.get<std::string>() : value.dump()) << "\n";
        return;
    }
    for (const auto& [key, item] : value.items()) {
        std::cout << key << ": " << (item.is_string() ? item.get<std::string>() : item.dump()) << "\n";
    }
}

fs::path workspace_for(const std::string& value, const std::string& url = {}) {
    if (!value.empty()) return value;
    if (!url.empty()) return default_workspace(url);
    throw std::runtime_error("--workspace is required for this command.");
}

std::optional<std::string> find_executable(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::istringstream entries(path ? path : "");
    for (std::string directory; std::getline(entries, directory, ':');) {
        auto candidate = fs::path(directory.empty() ? "." : directory) / name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return std::nullopt;
}

StageResult scrape_stage(const ScrapeFlags& flags, const fs::path& workspace) {
    auto site = scrape({flags.url, workspace, flags.title, flags.language, flags.content_selector, flags.nav_selector,
                        flags.max_pages, flags.force});
    return {"scrape", "passed", {{"pages", static_cast<int>(site.pages.size())}}, {}};
}

int run(const ScrapeFlags& flags, const std::string& output, bool no_reflow, bool as_json) {
    auto workspace = workspace_for(flags.workspace, flags.url);
    std::vector<StageResult> stages;
    std::optional<std::string> epub;
    std::string active = "scrape";
    try {
        stages.push_back(scrape_stage(flags, workspace));
        active = "build";
        auto built = build(workspace, output.empty() ? std::nullopt : std::optional<fs::path>(output));
        epub = built.epub.string();
        stages.push_back({"build", "passed", {{"chapters", built.chapters}, {"assets", built.assets}}, {}});
        active = "validate";
        stages.push_back(validate(built.epub, workspace, !no_reflow));
    } catch (const std::exception& error) {
        stages.push_back({active, "failed", {}, {error_finding("COMMAND_FAILED", error.what(), active)}});
    }
    bool passed = std::all_of(stages.begin(), stages.end(), [](const auto& s) { return s.status == "passed"; });
    Workspace store(workspace);
    Report report{"run", passed ? "passed" : "failed", store.root.string(), epub, stages};
    store.create();
    store.write_report(report);
    emit(report, as_json);
    return passed ? 0 : 1;
}

int doctor(bool as_json) {
    auto chromium = find_executable(std::getenv("DOCS2EPUB_CHROMIUM") ? std::getenv("DOCS2EPUB_CHROMIUM") : "chromium");
    auto epubcheck = find_executable("epubcheck");
    bool ok = chromium && epubcheck;
    json tools = {{"chromium", chromium ? json(*chromium) : json(nullptr)},
                  {"epubcheck", epubcheck ? json(*epubcheck) : json(nullptr)},
                  {"version", version},
                  {"status", ok ? "passed" : "failed"}};
    emit(tools, as_json);
    return ok ? 0 : 3;
}

int inspect(const std::string& subject, const std::string& workspace_path, bool as_json) {
    Workspace workspace(workspace_for(workspace_path));
    if (subject == "report") {
        emit(json::parse(read_file(workspace.root / "report.json")), as_json);
    } else if (subject == "pages") {
        emit(workspace.read_pages(), as_json);
    } else {
        emit(workspace.read_site(), as_json);
    }
    return 0;
}

void add_scrape_flags(CLI::App* command, ScrapeFlags& flags, bool& as_json) {
    command->add_option("url", flags.url, "Documentation entry URL")->required();
    command->add_option("--workspace", flags.workspace);
    command->add_option("--title", flags.title);
    command->add_option("--language", flags.language);
    command->add_option("--content-selector", flags.content_selector);
    command->add_option("--nav-selector", flags.nav_selector);
    command->add_option("--max-pages", flags.max_pages)->capture_default_str();
    command->add_flag("--force", flags.force);
    command->add_flag("--json", as_json);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> values(argv + 1, argv + argc);
    if (!values.empty() && starts_with_any(values.front(), {"http://", "https://"})) values.insert(values.begin(), "run");
    std::reverse(values.begin(), values.end());

    CLI::App app{"Turn a rendered documentation site into a validated EPUB.", "docs2epub"};
    app.set_version_flag("--version", std::string(version));
    app.require_subcommand(1);

    ScrapeFlags flags;
    bool as_json = false, no_reflow = false;
    std::string output, workspace, epub_path, subject = "site";

    auto* run_command = app.add_subcommand("run", "Scrape, build, and validate a documentation site.");
    add_scrape_flags(run_command, flags, as_json);
    run_command->add_option("-o,--output", output);
    run_command->add_flag("--no-reflow", no_reflow);

    auto* scrape_command = app.add_subcommand("scrape", "Capture rendered pages into a rerunnable workspace.");
    add_scrape_flags(scrape_command, flags, as_json);

    auto* build_command = app.add_subcommand("build", "Build an EPUB from a captured workspace without network access.");
    build_command->add_option("--workspace", workspace)->required();
    build_command->add_option("-o,--output", output);
    build_command->add_flag("--json", as_json);

    auto* validate_command = app.add_subcommand("validate", "Validate a packaged EPUB.");
    validate_command->add_option("epub", epub_path)->required();
    validate_command->add_option("--workspace", workspace);
    validate_command->add_flag("--no-reflow", no_reflow);
    validate_command->add_flag("--json", as_json);

    auto* inspect_command = app.add_subcommand("inspect", "Inspect a workspace without changing it.");
    inspect_command->add_option("subject", subject)->check(CLI::IsMember({"site", "pages", "report"}));
    inspect_command->add_option("--workspace", workspace)->required();
    inspect_command->add_flag("--json", as_json);

    auto* doctor_command = app.add_subcommand("doctor", "Check the local runtime and validators.");
    doctor_command->add_flag("--json", as_json);

    try {
        app.parse(values);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    try {
        if (run_command->parsed()) return run(flags, output, no_reflow, as_json);
        if (scrape_command->parsed()) {
            try {
                emit(scrape_stage(flags, workspace_for(flags.workspace, flags.url)), as_json);
                return 0;
            } catch (const std::exception& error) {
                emit({{"status", "failed"}, {"error", error.what()}}, as_json);
                return 1;
            }
        }
        if (build_command->parsed()) {
            auto built = build(workspace, output.empty() ? std::nullopt : std::optional<fs::path>(output));
            emit({{"status", "passed"}, {"epub", built.epub.string()}, {"chapters", built.chapters},
                  {"assets", built.assets}},
                 as_json);
            return 0;
        }
        if (validate_command->parsed()) {
            auto result = validate(epub_path, workspace.empty() ? std::nullopt : std::optional<fs::path>(workspace),
                                   !no_reflow);
            emit(result, as_json);
            return result.status == "passed" ? 0 : 1;
        }
        if (inspect_command->parsed()) return inspect(subject, workspace, as_json);
        return doctor(as_json);
    } catch (const std::exception& error) {
        std::cerr << "docs2epub: " << error.what() << "\n";
        return 1;
    }
}
