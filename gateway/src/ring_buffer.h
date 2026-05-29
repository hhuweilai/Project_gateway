/*
 * ring_buffer.h — C++11 线程安全环形缓冲区
 *
 * 用于采集线程 → 网络线程之间的帧传递。
 * 固定容量，push 满时阻塞，pop 空时阻塞。
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <condition_variable>

#define FRAME_BUF_SIZE  (640 * 480 * 2)  /* YUYV 一帧最大字节数 */
#define RING_BUF_COUNT  4                /* 缓冲帧数 */

class RingBuffer {
public:
    RingBuffer() : head_(0), tail_(0), count_(0), closed_(false) {}

    /* 推入一帧（阻塞直到有空位或关闭） */
    bool push(const uint8_t *data, size_t len) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return count_ < RING_BUF_COUNT || closed_; });
        if (closed_) return false;

        size_t copy_len = (len > FRAME_BUF_SIZE) ? FRAME_BUF_SIZE : len;
        std::memcpy(buf_[tail_], data, copy_len);
        len_[tail_] = copy_len;
        tail_ = (tail_ + 1) % RING_BUF_COUNT;
        count_++;
        not_empty_.notify_one();
        return true;
    }

    /* 取出一帧（阻塞直到有数据或关闭） */
    bool pop(uint8_t *data, size_t *len) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return count_ > 0 || closed_; });
        if (closed_ && count_ == 0) return false;

        std::memcpy(data, buf_[head_], len_[head_]);
        *len = len_[head_];
        head_ = (head_ + 1) % RING_BUF_COUNT;
        count_--;
        not_full_.notify_one();
        return true;
    }

    /* 关闭缓冲，唤醒所有等待线程 */
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool is_closed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    uint8_t  buf_[RING_BUF_COUNT][FRAME_BUF_SIZE];
    size_t   len_[RING_BUF_COUNT];
    int      head_, tail_, count_;
    bool     closed_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};

#endif /* RING_BUFFER_H */
