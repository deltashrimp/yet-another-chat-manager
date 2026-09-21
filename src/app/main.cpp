#include "app/app_context.hpp"
#include "config/config.hpp"
#include "storage/db.hpp"
#include "telegram/client.hpp"
#include "version.hpp"

#include <curl/curl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>

namespace {
std::atomic<bool> g_stop{false};

void handle_signal(int /*signum*/) { g_stop.store(true); }

int healthcheck(const yacm::config::Config& cfg, const std::shared_ptr<spdlog::logger>& log) {
    try {
        yacm::storage::Database db(cfg.db_path);
        db.open();
        db.migrate();
        yacm::tg::Client client(cfg.bot_token);
        const auto me = client.get_me();
        std::printf("ok: bot=%s id=%lld\n", me.username.c_str(),
                    static_cast<long long>(me.id));
        db.close();
        return 0;
    } catch (const std::exception& e) {
        log->error("healthcheck failed: {}", e.what());
        return 1;
    }
}

void print_usage() {
    std::printf("yacm v%s (build %s)\n", YACM_VERSION, YACM_BUILD_DATE);
    std::printf("usage: bot [--help] [--version] [--healthcheck]\n");
}

}  // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const std::string arg = (argc > 1) ? argv[1] : "";
    if (arg == "--help") {
        print_usage();
        return 0;
    }
    if (arg == "--version") {
        std::printf("yacm v%s (build %s)\n", YACM_VERSION, YACM_BUILD_DATE);
        return 0;
    }

    yacm::config::Config cfg;
    try {
        cfg = yacm::config::from_env();
    } catch (const yacm::config::ConfigError& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 2;
    }

    auto log = spdlog::stdout_color_mt("yacm");
    log->set_level(spdlog::level::from_str(cfg.log_level));

    if (arg == "--healthcheck") {
        const int rc = healthcheck(cfg, log);
        curl_global_cleanup();
        return rc;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        yacm::app::AppContext ctx(std::move(cfg), log, g_stop);
        ctx.init();
        log->info("yacm v{} starting", YACM_VERSION);
        ctx.run();
        ctx.shutdown();
    } catch (const std::exception& e) {
        log->error("fatal: {}", e.what());
        return 1;
    }

    curl_global_cleanup();
    return 0;
}