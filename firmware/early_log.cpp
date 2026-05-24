#include "early_log.h"

namespace {

uint8_t  buf_[EARLY_LOG_CAPACITY];
uint16_t head_     = 0;   // oldest byte
uint16_t tail_     = 0;   // one past newest
bool     full_     = false;
bool     frozen_   = false;
uint32_t dropped_  = 0;

} // namespace

extern "C" void early_log_push(uint8_t c)
{
    if (frozen_)
    {
        return;
    }
    buf_[tail_] = c;
    tail_ = static_cast<uint16_t>((tail_ + 1u) % EARLY_LOG_CAPACITY);
    if (full_)
    {
        // Overflow: oldest byte just got overwritten; advance head.
        head_ = tail_;
        ++dropped_;
    }
    else if (tail_ == head_)
    {
        full_ = true;
    }
}

extern "C" void early_log_replay(int (*emit)(int))
{
    if (!emit)
    {
        return;
    }
    if (!full_ && head_ == tail_)
    {
        return;  // empty
    }
    uint16_t i = head_;
    do
    {
        emit(static_cast<int>(buf_[i]));
        i = static_cast<uint16_t>((i + 1u) % EARLY_LOG_CAPACITY);
    } while (i != tail_);
}

extern "C" void early_log_freeze(void)
{
    frozen_ = true;
}

extern "C" uint32_t early_log_dropped_count(void)
{
    return dropped_;
}
