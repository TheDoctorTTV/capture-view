// This process only provisions Capture View's loopback devices. It never runs
// the GUI or the capture engine with elevated privileges.
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include "../third-party/v4l2loopback/v4l2loopback.h"
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Fd {
  int value;
  explicit Fd(int fd) : value(fd) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd&) = delete;
};
unsigned number(const std::string& text) {
  unsigned result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid numeric argument");
  return result;
}
void run(const char* program, const std::vector<std::string>& arguments) {
  std::vector<char*> argv{const_cast<char*>(program)};
  for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  const pid_t child = ::fork();
  if (child < 0) throw std::runtime_error("could not start system device setup");
  if (child == 0) { ::execv(program, argv.data()); _exit(127); }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) throw std::runtime_error("could not wait for device setup");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("system device setup failed; install a v4l2loopback module matching the running kernel");
}
void write_root_file(const std::string& path, const std::string& data, mode_t mode) {
  Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode));
  if (fd.value < 0 || ::fchown(fd.value, 0, 0) < 0 || ::fchmod(fd.value, mode) < 0)
    throw std::runtime_error("could not save persistent virtual-camera setup");
  size_t offset = 0;
  while (offset < data.size()) {
    const auto written = ::write(fd.value, data.data() + offset, data.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("could not write virtual-camera setup");
    offset += static_cast<size_t>(written);
  }
}
void persist(unsigned uid, unsigned count) {
  // Keep only this application's device creation persistent; never replace
  // global modprobe options or unload other applications' loopback devices.
  const std::string service = "capture-view-virtual-devices-" + std::to_string(uid) + ".service";
  const std::string unit_path = "/etc/systemd/system/" + service;
  if (count == 0) {
    if (std::filesystem::exists(unit_path)) {
      run("/usr/bin/systemctl", {"disable", "--now", service});
      if (::unlink(unit_path.c_str()) < 0) throw std::runtime_error("could not remove the virtual-camera startup service");
      run("/usr/bin/systemctl", {"daemon-reload"});
    }
    return;
  }
  const std::string helper = "/usr/local/libexec/capture-view-device-helper";
  std::filesystem::create_directories("/usr/local/libexec");
  // Stage in a root-owned directory and rename atomically; don't truncate a
  // currently executing helper when the AppImage has been updated.
  const std::string staging = helper + ".new";
  Fd input(::open("/proc/self/exe", O_RDONLY | O_CLOEXEC));
  Fd output(::open(staging.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0755));
  if (input.value < 0 || output.value < 0) throw std::runtime_error("could not install the persistent device helper");
  char bytes[16384];
  for (;;) {
    const auto read = ::read(input.value, bytes, sizeof(bytes));
    if (read < 0 && errno == EINTR) continue;
    if (read < 0) throw std::runtime_error("could not read the device helper");
    if (read == 0) break;
    ssize_t offset = 0;
    while (offset < read) {
      const auto written = ::write(output.value, bytes + offset, static_cast<size_t>(read - offset));
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) throw std::runtime_error("could not install the device helper");
      offset += written;
    }
  }
  if (::fchown(output.value, 0, 0) < 0 || ::fchmod(output.value, 0755) < 0 || ::rename(staging.c_str(), helper.c_str()) < 0)
    throw std::runtime_error("could not finalize the device helper installation");
  write_root_file(unit_path,
    "[Unit]\nDescription=Capture View virtual cameras\nAfter=systemd-udevd.service\n\n"
    "[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=" + helper + " --restore " +
      std::to_string(uid) + " " + std::to_string(count) + "\n\n[Install]\nWantedBy=multi-user.target\n", 0644);
  run("/usr/bin/systemctl", {"daemon-reload"});
  run("/usr/bin/systemctl", {"enable", service});
}
std::map<unsigned, unsigned> discover() {
  std::map<unsigned, unsigned> devices;
  const std::filesystem::path directory("/sys/class/video4linux");
  if (!std::filesystem::is_directory(directory)) return devices;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto filename = entry.path().filename().string();
    if (!filename.starts_with("video")) continue;
    std::string label; std::getline(std::ifstream(entry.path() / "name"), label);
    if (!label.starts_with("Capture View ")) continue;
    const auto suffix = label.substr(13);
    if (suffix.empty() || suffix.size() > 2 || !std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
    const auto index = number(suffix);
    if (index < 1 || index > 8) continue;
    const auto nr = number(filename.substr(5));
    Fd fd(::open(("/dev/" + filename).c_str(), O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    v4l2_capability cap{};
    if (fd.value < 0 || ::ioctl(fd.value, VIDIOC_QUERYCAP, &cap) < 0 ||
        std::string(reinterpret_cast<char*>(cap.driver)) != "v4l2 loopback") continue;
    if (!devices.emplace(index, nr).second) throw std::runtime_error("duplicate Capture View virtual-device names; close other instances first");
  }
  return devices;
}
}
int main(int argc, char** argv) {
  try {
    const bool restore = argc == 4 && std::string(argv[1]) == "--restore";
    if (!restore && (argc != 3 || std::string(argv[1]) != "--count"))
      throw std::runtime_error("usage: capture-view-device-helper --count 0..8");
    const unsigned count = number(argv[restore ? 3 : 2]);
    if (count > 8 || ::geteuid() != 0) throw std::runtime_error("system authorization is required to configure 0..8 virtual cameras");
    const char* requester = ::getenv("PKEXEC_UID");
    if (!requester) requester = ::getenv("SUDO_UID");
    if (!restore && !requester) throw std::runtime_error("run device setup through the application's authorization prompt");
    const auto uid = restore ? number(argv[2]) : number(requester);
    const auto* user = ::getpwuid(uid);
    if (uid == 0 || user == nullptr) throw std::runtime_error("could not identify the requesting desktop user");
    const auto gid = user->pw_gid;
    Fd lock(::open("/run/capture-view-device-setup.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.value < 0 || ::flock(lock.value, LOCK_EX) < 0) throw std::runtime_error("could not lock device setup");
    const char* modprobe = std::filesystem::exists("/usr/bin/modprobe") ? "/usr/bin/modprobe" : "/sbin/modprobe";
    run(modprobe, {"v4l2loopback"});
    Fd control(::open("/dev/v4l2loopback", O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    if (control.value < 0) throw std::runtime_error("v4l2loopback's control device is unavailable");
    auto devices = discover();
    for (const auto& [index, nr] : devices) {
      struct stat st{};
      if (::stat(("/dev/video" + std::to_string(nr)).c_str(), &st) != 0 || (st.st_uid != 0 && st.st_uid != uid))
        throw std::runtime_error("Capture View virtual cameras belong to another desktop user");
    }
    for (const auto& [index, nr] : devices) {
      if (index > count && ::ioctl(control.value, V4L2LOOPBACK_CTL_REMOVE, static_cast<unsigned long>(nr)) < 0)
        throw std::runtime_error("Close apps using Capture View " + std::to_string(index) + " before reducing the virtual-device count");
    }
    for (unsigned i = 1; i <= count; ++i) {
      if (devices.contains(i)) continue;
      v4l2_loopback_config config{};
      config.output_nr = -1;
      config.max_buffers = 4; config.max_openers = 10; config.announce_all_caps = 0;
      const auto label = "Capture View " + std::to_string(i);
      std::copy(label.begin(), label.end(), config.card_label);
      const int nr = ::ioctl(control.value, V4L2LOOPBACK_CTL_ADD, &config);
      if (nr < 0) throw std::runtime_error("could not create virtual camera: " + std::string(std::strerror(errno)));
      devices[i] = static_cast<unsigned>(nr);
    }
    if (std::filesystem::exists("/usr/bin/udevadm")) run("/usr/bin/udevadm", {"settle", "--timeout=5"});
    for (unsigned i = 1; i <= count; ++i) {
      const auto path = "/dev/video" + std::to_string(devices.at(i));
      Fd fd(::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
      struct stat st{}; v4l2_capability cap{};
      if (fd.value < 0 || ::fstat(fd.value, &st) < 0 || !S_ISCHR(st.st_mode) ||
          ::ioctl(fd.value, VIDIOC_QUERYCAP, &cap) < 0 ||
          std::string(reinterpret_cast<char*>(cap.driver)) != "v4l2 loopback" ||
          std::string(reinterpret_cast<char*>(cap.card)) != "Capture View " + std::to_string(i))
        throw std::runtime_error("virtual camera identity changed during setup");
      if (::fchown(fd.value, uid, gid) < 0 || ::fchmod(fd.value, 0660) < 0)
        throw std::runtime_error("could not grant access to the virtual camera");
    }
    if (!restore) persist(uid, count);
    std::cout << "[";
    for (unsigned i = 1; i <= count; ++i) {
      if (i > 1) std::cout << ',';
      std::cout << "\"/dev/video" << devices.at(i) << "\"";
    }
    std::cout << "]\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
