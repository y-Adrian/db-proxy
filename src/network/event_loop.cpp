#include "network/event_loop.h"
#include "core/logger.h"
#include <unistd.h>
#include <cstring>
#include <thread>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#endif

namespace dbproxy {

EventLoop::EventLoop()
    : epoll_fd_(-1), quit_(false), thread_id_(0), next_timer_id_(1) {
    thread_id_ = getpid();  // 简化，实际应该是线程 ID

#if defined(__linux__)
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_ERROR("Failed to create epoll");
    }
    
    // 创建定时器 fd
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = timer_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);
    }
#else
    timer_fd_ = -1;
#endif
}

EventLoop::~EventLoop() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
    if (timer_fd_ >= 0) {
        close(timer_fd_);
    }
}

void EventLoop::loop() {
#if defined(__linux__)
    const int MAX_EVENTS = 1024;
    struct epoll_event events[MAX_EVENTS];
    
    while (!quit_) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);  // 100ms 超时
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;
            
            if (fd == timer_fd_) {
                // 定时器触发
                uint64_t exp;
                read(timer_fd_, &exp, sizeof(exp));
                handleExpiredTimers();
            }
        }
    }
#else
    while (!quit_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        handleExpiredTimers();
    }
#endif
}

void EventLoop::quit() {
    quit_ = true;
}

bool EventLoop::isInLoopThread() const {
    return thread_id_ == getpid();  // 简化
}

void EventLoop::updateChannel(Channel* channel) {
    (void)channel;
}

void EventLoop::removeChannel(Channel* channel) {
    (void)channel;
}

EventLoop::TimerId EventLoop::runAt(TimerCallback cb, std::chrono::milliseconds timeout) {
    auto now = std::chrono::steady_clock::now();
    Timer timer;
    timer.id = next_timer_id_++;
    timer.expire = now + timeout;
    timer.callback = std::move(cb);
    timer.repeat = false;
    TimerId id = timer.id;
    timers_[id] = std::move(timer);
    return id;
}

EventLoop::TimerId EventLoop::runAfter(TimerCallback cb, std::chrono::milliseconds delay) {
    return runAt(std::move(cb), delay);
}

EventLoop::TimerId EventLoop::runEvery(TimerCallback cb, std::chrono::milliseconds interval) {
    auto now = std::chrono::steady_clock::now();
    Timer timer;
    timer.id = next_timer_id_++;
    timer.expire = now + interval;
    timer.callback = std::move(cb);
    timer.repeat = true;
    timer.interval = interval;
    TimerId id = timer.id;
    timers_[id] = std::move(timer);
    return id;
}

void EventLoop::cancelTimer(TimerId id) {
    timers_.erase(id);
}

void EventLoop::handleExpiredTimers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<TimerId> expired;
    
    for (auto& [id, timer] : timers_) {
        if (timer.expire <= now) {
            expired.push_back(id);
            
            // 执行回调
            if (timer.callback) {
                timer.callback();
            }
            
            // 如果是重复定时器，重新设置
            if (timer.repeat) {
                timer.expire = now + timer.interval;
            }
        }
    }
    
    // 删除已过期的非重复定时器
    for (auto id : expired) {
        auto it = timers_.find(id);
        if (it != timers_.end() && !it->second.repeat) {
            timers_.erase(it);
        }
    }
}

}  // namespace dbproxy
