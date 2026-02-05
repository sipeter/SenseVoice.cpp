#pragma once

#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <chrono>

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 0)
        : m_capacity(capacity), m_head(0), m_size(0), m_overwrite_oldest(true) {
        m_buffer.resize(m_capacity);
    }

    void reset(size_t capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capacity = capacity;
        m_buffer.clear();
        m_buffer.resize(m_capacity);
        m_head = 0;
        m_size = 0;
    }

    void set_overwrite_oldest(bool enable) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_overwrite_oldest = enable;
    }

    void push(const T* data, size_t count) {
        if (!data || count == 0) return;
        if (m_capacity == 0) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (size_t i = 0; i < count; ++i) {
                if (m_size < m_capacity) {
                    size_t pos = (m_head + m_size) % m_capacity;
                    m_buffer[pos] = data[i];
                    ++m_size;
                } else if (m_overwrite_oldest) {
                    m_buffer[m_head] = data[i];
                    m_head = (m_head + 1) % m_capacity;
                }
            }
        }

        m_cv.notify_all();
    }

    void push(const std::vector<T>& data) {
        push(data.data(), data.size());
    }

    bool read(size_t count, std::vector<T>& out, int wait_ms = 0) {
        if (count == 0) {
            out.clear();
            return false;
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        if (wait_ms > 0 && m_size == 0) {
            m_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&]() { return m_size > 0; });
        }

        if (m_size == 0) {
            out.clear();
            return false;
        }

        size_t available = (m_size < count) ? m_size : count;
        out.resize(available);

        for (size_t i = 0; i < available; ++i) {
            out[i] = m_buffer[(m_head + i) % m_capacity];
        }

        m_head = (m_head + available) % m_capacity;
        m_size -= available;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head = 0;
        m_size = 0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size;
    }

private:
    std::vector<T> m_buffer;
    size_t m_capacity;
    size_t m_head;
    size_t m_size;
    bool m_overwrite_oldest;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
};