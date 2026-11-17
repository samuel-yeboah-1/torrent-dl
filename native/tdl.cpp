#include <libtorrent/add_torrent_params.hpp>
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
#include <vector>

#include <termios.h>
#include <unistd.h>

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

static std::string fitName(const std::string& name, size_t width) {
  if (name.size() <= width) return name + std::string(width - name.size(), ' ');
  size_t cut = width - 3;
  while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) cut--;
  return name.substr(0, cut) + "..." + std::string(width - 3 - cut, ' ');
}

static std::string progressLine(const lt::torrent_status& s, int elapsedSeconds) {
  char line[256];
  if (s.errc) {
    std::snprintf(line, sizeof(line), "%s Error: %s", fitName(s.name.empty() ? "Unknown" : s.name, 28).c_str(),
                  s.errc.message().c_str());
    return line;
  }
  if (!s.has_metadata) {
    std::snprintf(line, sizeof(line), "%s Finding peers... %d found, %ds", fitName("Waiting for details", 28).c_str(),
                  s.num_peers, elapsedSeconds);
    return line;
  }
  const int width = 20;
  int filled = static_cast<int>(s.progress * width + 0.5f);
  std::string bar;
  for (int i = 0; i < width; i++) bar += i < filled ? "█" : "░";
  if (s.is_finished) {
    std::snprintf(line, sizeof(line), "%s %s 100.0%%  Done", fitName(s.name, 28).c_str(), bar.c_str());
    return line;
  }
  std::snprintf(line, sizeof(line), "%s %s %5.1f%%  %s/s  %d peers", fitName(s.name, 28).c_str(), bar.c_str(),
                s.progress * 100, formatBytes(s.download_payload_rate).c_str(), s.num_peers);
  return line;
}

static bool readLine(std::string& out) {
  out.clear();
  if (!isatty(STDIN_FILENO)) return static_cast<bool>(std::getline(std::cin, out));

  termios original;
  tcgetattr(STDIN_FILENO, &original);
  termios raw = original;
  raw.c_lflag &= ~(ICANON | ECHO | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  bool ok = true;
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1) {
    if (c == '\r' || c == '\n') break;
    if (c == 3 || (c == 4 && out.empty())) {
      ok = false;
      break;
    }
    if (c == 127 || c == 8) {
      if (!out.empty()) {
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty()) out.pop_back();
        std::fputs("\b \b", stdout);
      }
    } else if (static_cast<unsigned char>(c) >= 32 || c == '\t') {
      out += c;
      std::fputc(c, stdout);
    }
    std::fflush(stdout);
  }

  tcsetattr(STDIN_FILENO, TCSANOW, &original);
  std::fputc('\n', stdout);
  return ok;
}

static std::vector<std::string> askForLinks() {
  std::vector<std::string> links;
  std::cout << "Paste magnet links, one per line. Press Enter on an empty line to start.\n";
  std::string line;
  while (std::cout << "> " << std::flush && readLine(line)) {
    line = trim(line);
    if (line.empty()) break;
    links.push_back(line);
  }
  return links;
}

int main(int argc, char** argv) {
  std::vector<std::string> links;
  for (int i = 1; i < argc; i++) links.push_back(trim(argv[i]));
  if (links.empty()) links = askForLinks();
  if (links.empty()) {
    std::cerr << "No magnet links given.\n";
    return 1;
  }

  const char* home = std::getenv("HOME");
  std::string downloadDir = std::string(home ? home : ".") + "/Downloads";

  std::vector<lt::add_torrent_params> toAdd;
  for (const std::string& link : links) {
    if (link.rfind("magnet:", 0) != 0) {
      std::cerr << "Skipping, not a magnet link: " << link.substr(0, 40) << "\n";
      continue;
    }
    lt::error_code ec;
    lt::add_torrent_params atp = lt::parse_magnet_uri(link, ec);
    if (ec) {
      std::cerr << "Skipping, could not read link: " << ec.message() << "\n";
      continue;
    }
    atp.save_path = downloadDir;
    toAdd.push_back(std::move(atp));
  }
  if (toAdd.empty()) return 1;

  std::signal(SIGINT, [](int) { stopRequested = true; });

  lt::settings_pack pack;
  pack.set_int(lt::settings_pack::alert_mask, lt::alert_category_t{});
  lt::session_params params(pack);
  params.disk_io_constructor = lt::posix_disk_io_constructor;
  lt::session session(std::move(params));

  std::vector<lt::torrent_handle> handles;
  for (auto& atp : toAdd) handles.push_back(session.add_torrent(std::move(atp)));

  auto startTime = std::chrono::steady_clock::now();
  bool drawnBefore = false;
  bool allDone = false;

  while (!stopRequested && !allDone) {
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count());

    if (drawnBefore) std::printf("\x1b[%zuA", handles.size());
    allDone = true;
    for (const auto& handle : handles) {
      lt::torrent_status s = handle.status();
      std::printf("\r\x1b[2K%s\n", progressLine(s, elapsed).c_str());
      if (!s.is_finished && !s.errc) allDone = false;
    }
    std::fflush(stdout);
    drawnBefore = true;

    if (!allDone) std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (stopRequested) {
    std::cout << "Stopped.\n";
    return 0;
  }
  std::cout << "All done. Files are in " << downloadDir << "\n";
  return 0;
}
