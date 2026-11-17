#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace lt = libtorrent;

static std::atomic<bool> stopRequested{false};

static std::string formatBytes(double bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  while (bytes >= 1024 && i < 4) {
    bytes /= 1024;
    i++;
  }
  char out[32];
  std::snprintf(out, sizeof(out), "%.1f %s", bytes, units[i]);
  return out;
}

static std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

static void drawProgress(const lt::torrent_status& s) {
  const int width = 30;
  int filled = static_cast<int>(s.progress * width + 0.5f);
  std::string bar;
  for (int i = 0; i < width; i++) bar += i < filled ? "█" : "░";
  std::printf("\r%s %5.1f%%  %s/s  %d peers   ", bar.c_str(), s.progress * 100,
              formatBytes(s.download_payload_rate).c_str(), s.num_peers);
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  std::string link;
  if (argc > 1) {
    link = argv[1];
  } else {
    std::cout << "Paste magnet link: " << std::flush;
    std::getline(std::cin, link);
  }
  link = trim(link);

  if (link.rfind("magnet:", 0) != 0) {
    std::cerr << "That does not look like a magnet link.\n";
    return 1;
  }

  lt::error_code ec;
  lt::add_torrent_params atp = lt::parse_magnet_uri(link, ec);
  if (ec) {
    std::cerr << "Could not read magnet link: " << ec.message() << "\n";
    return 1;
  }

  const char* home = std::getenv("HOME");
  std::string downloadDir = std::string(home ? home : ".") + "/Downloads";
  atp.save_path = downloadDir;

  std::signal(SIGINT, [](int) { stopRequested = true; });

  lt::settings_pack pack;
  pack.set_int(lt::settings_pack::alert_mask, lt::alert_category::error);
  lt::session_params params(pack);
  params.disk_io_constructor = lt::posix_disk_io_constructor;
  lt::session session(std::move(params));

  lt::torrent_handle handle = session.add_torrent(std::move(atp));
  std::cout << "Finding peers...\n";

  bool started = false;
  while (!stopRequested) {
    std::vector<lt::alert*> alerts;
    session.pop_alerts(&alerts);
    for (lt::alert* a : alerts) {
      if (auto* err = lt::alert_cast<lt::torrent_error_alert>(a)) {
        std::cerr << "\nError: " << err->message() << "\n";
        return 1;
      }
    }

    lt::torrent_status s = handle.status();
    if (!started && s.has_metadata) {
      std::cout << "Downloading: " << s.name << " (" << formatBytes(s.total_wanted) << ")\n";
      started = true;
    }
    if (started) drawProgress(s);
    if (started && s.is_finished) {
      std::cout << "\nDone. Saved to " << downloadDir << "/" << s.name << "\n";
      return 0;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  std::cout << "\nStopped.\n";
  return 0;
}
