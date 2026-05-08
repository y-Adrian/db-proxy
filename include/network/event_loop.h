#ifndef DB_PROXY_EVENT_LOOP_H
#define DB_PROXY_EVENT_LOOP_H

#include <cstdint>
#include <vector>
#include <functional>
#include <unordered_map>
#include <chrono>

namespace dbproxy {

class Channel;

/**
 * @brief 事件循环 - Reactor 模式核心
 * 
 * 面试亮点：
 * - 单线程事件循环：避免锁竞争，提高并发效率
 * - epoll_wait：高效 IO 多路复用
 * - 时间轮/定时器：用于超时管理
 * - O(1) 获取就绪事件
 */
class EventLoop {
public:
    using TimerCallback = std::function<void()>;
    
    EventLoop();
    ~EventLoop();
    
    // 禁止拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    
    void loop();
    void quit();
    
    // 更新/删除文件描述符监听
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    
    // 定时器
    using TimerId = uint64_t;
    TimerId runAt(TimerCallback cb, std::chrono::milliseconds timeout);
    TimerId runAfter(TimerCallback cb, std::chrono::milliseconds delay);
    TimerId runEvery(TimerCallback cb, std::chrono::milliseconds interval);
    void cancelTimer(TimerId id);
    
    int epollFd() const { return epoll_fd_; }
    bool isInLoopThread() const;
    
private:
    void handleExpiredTimers();
    
    int epoll_fd_;
    bool quit_;
    int timer_fd_;  // 用于定时器唤醒
    
    pid_t thread_id_;
    std::vector<Channel*> active_channels_;
    
    // 定时器管理（简化实现）
    struct Timer {
        TimerId id;
        std::chrono::steady_clock::time_point expire;
        TimerCallback callback;
        bool repeat;
        std::chrono::milliseconds interval;
    };
    std::unordered_map<TimerId, Timer> timers_;
    TimerId next_timer_id_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_EVENT_LOOP_H
