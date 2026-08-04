#include "touch.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace touch {
namespace {

struct Finger {
  float x = 0.f, y = 0.f;
  bool down = false;
  int id = -1;
};

struct Device {
  int fd = -1;
  float s2tx = 1.f, s2ty = 1.f;
  input_absinfo absX{}, absY{};
  Finger finger[10]{};
};

std::mutex g_mu;
std::vector<Device> g_devs;
std::atomic<bool> g_run{false};
float g_screen_w = 1080.f, g_screen_h = 1920.f;
float g_touch_max_x = 1.f, g_touch_max_y = 1.f;
int g_orient = 0;
float g_x = 0.f, g_y = 0.f;
bool g_down = false;
bool g_prev_down = false;

bool is_touch_device(int fd) {
  uint8_t* bits = nullptr;
  ssize_t bits_size = 0;
  int res = 0;
  bool has_slot = false, has_x = false, has_y = false;
  while (true) {
    res = ioctl(fd, EVIOCGBIT(EV_ABS, bits_size), bits);
    if (res < bits_size) break;
    bits_size = res + 16;
    bits = static_cast<uint8_t*>(
        realloc(bits, static_cast<size_t>(bits_size) * 2));
  }
  for (int j = 0; j < res; ++j) {
    for (int k = 0; k < 8; ++k) {
      if (!(bits[j] & (1 << k))) continue;
      const int code = j * 8 + k;
      input_absinfo abs{};
      if (ioctl(fd, EVIOCGABS(code), &abs) != 0) continue;
      if (code == ABS_MT_SLOT) has_slot = true;
      if (code == ABS_MT_POSITION_X) has_x = true;
      if (code == ABS_MT_POSITION_Y) has_y = true;
    }
  }
  free(bits);
  return has_slot && has_x && has_y;
}

void to_screen(float tx, float ty, float& ox, float& oy) {
  const float sx = g_touch_max_x > 1.f ? g_touch_max_x : g_screen_w;
  const float sy = g_touch_max_y > 1.f ? g_touch_max_y : g_screen_h;
  switch (g_orient) {
    case 1:
      ox = ty / sy * g_screen_w;
      oy = g_screen_h - tx / sx * g_screen_h;
      break;
    case 2:
      ox = g_screen_w - tx / sx * g_screen_w;
      oy = g_screen_h - ty / sy * g_screen_h;
      break;
    case 3:
      ox = g_screen_w - ty / sy * g_screen_w;
      oy = tx / sx * g_screen_h;
      break;
    default:
      ox = tx / sx * g_screen_w;
      oy = ty / sy * g_screen_h;
      break;
  }
}

void publish_finger(const Finger& f) {
  if (f.down) {
    float sx, sy;
    to_screen(f.x, f.y, sx, sy);
    g_x = sx;
    g_y = sy;
    g_down = true;
  } else {
    g_down = false;
  }
}

void* reader(void* arg) {
  const int idx = static_cast<int>(reinterpret_cast<long>(arg));
  Device& dev = g_devs[static_cast<size_t>(idx)];
  int latest = 0;
  input_event ev[64]{};
  // 用 poll 等待，避免 O_NONBLOCK 空转占满 CPU
  while (g_run.load(std::memory_order_relaxed)) {
    pollfd pfd{};
    pfd.fd = dev.fd;
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, 200);  // 有事件即醒；超时仅用于检查退出
    if (pr <= 0) continue;
    if (!(pfd.revents & POLLIN)) continue;
    const auto n = read(dev.fd, ev, sizeof(ev));
    if (n <= 0 || (n % static_cast<ssize_t>(sizeof(input_event))) != 0) continue;
    const size_t count = static_cast<size_t>(n) / sizeof(input_event);
    std::lock_guard<std::mutex> lock(g_mu);
    for (size_t i = 0; i < count; ++i) {
      auto& e = ev[i];
      if (e.type == EV_ABS) {
        if (e.code == ABS_MT_SLOT) {
          latest = e.value;
          if (latest < 0) latest = 0;
          if (latest > 9) latest = 9;
          continue;
        }
        if (e.code == ABS_MT_TRACKING_ID) {
          if (e.value == -1)
            dev.finger[latest].down = false;
          else {
            dev.finger[latest].id = e.value;
            dev.finger[latest].down = true;
          }
          continue;
        }
        if (e.code == ABS_MT_POSITION_X) {
          dev.finger[latest].x = static_cast<float>(e.value) * dev.s2tx;
          continue;
        }
        if (e.code == ABS_MT_POSITION_Y) {
          dev.finger[latest].y = static_cast<float>(e.value) * dev.s2ty;
          continue;
        }
      }
      if (e.type == EV_SYN && e.code == SYN_REPORT) {
        bool any = false;
        for (int k = 0; k < 10; ++k) {
          if (dev.finger[k].down) {
            publish_finger(dev.finger[k]);
            any = true;
            break;
          }
        }
        if (!any) g_down = false;
      }
    }
  }
  return nullptr;
}

}  // namespace

void shutdown() {
  g_run = false;
  // 先关 fd 唤醒 poll，再稍等线程退出
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& d : g_devs) {
      if (d.fd >= 0) {
        ::close(d.fd);
        d.fd = -1;
      }
    }
  }
  usleep(50 * 1000);
  std::lock_guard<std::mutex> lock(g_mu);
  g_devs.clear();
  g_down = false;
}

bool init(float screen_w, float screen_h) {
  shutdown();
  g_screen_w = screen_w > 1.f ? screen_w : 1080.f;
  g_screen_h = screen_h > 1.f ? screen_h : 1920.f;
  g_devs.clear();

  DIR* dir = opendir("/dev/input/");
  if (!dir) return false;
  int event_count = 0;
  while (auto* p = readdir(dir)) {
    if (std::strstr(p->d_name, "event")) ++event_count;
  }
  closedir(dir);

  char path[64];
  for (int i = 0; i <= event_count; ++i) {
    std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) continue;
    if (!is_touch_device(fd)) {
      ::close(fd);
      continue;
    }
    Device d{};
    d.fd = fd;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &d.absX) != 0 ||
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &d.absY) != 0) {
      ::close(fd);
      continue;
    }
    g_devs.push_back(d);
  }
  if (g_devs.empty()) return false;

  g_touch_max_x = static_cast<float>(g_devs[0].absX.maximum);
  g_touch_max_y = static_cast<float>(g_devs[0].absY.maximum);
  if (g_touch_max_x < 1.f) g_touch_max_x = 1.f;
  if (g_touch_max_y < 1.f) g_touch_max_y = 1.f;

  for (auto& d : g_devs) {
    d.s2tx = g_touch_max_x /
             static_cast<float>(d.absX.maximum > 0 ? d.absX.maximum : 1);
    d.s2ty = g_touch_max_y /
             static_cast<float>(d.absY.maximum > 0 ? d.absY.maximum : 1);
  }

  g_run = true;
  for (size_t i = 0; i < g_devs.size(); ++i) {
    pthread_t th{};
    pthread_create(&th, nullptr, reader,
                   reinterpret_cast<void*>(static_cast<long>(i)));
    pthread_detach(th);
  }
  return true;
}

void set_display(float screen_w, float screen_h, int orientation) {
  if (screen_w > 1.f) g_screen_w = screen_w;
  if (screen_h > 1.f) g_screen_h = screen_h;
  g_orient = orientation & 3;
}

State snapshot() {
  State s;
  bool down = false;
  float x = 0.f, y = 0.f;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    down = g_down;
    x = g_x;
    y = g_y;
  }
  s.x = x;
  s.y = y;
  s.down = down;
  s.just_pressed = down && !g_prev_down;
  s.just_released = !down && g_prev_down;
  g_prev_down = down;
  return s;
}

}  // namespace touch
