#pragma once
/*
 * Command queue for the STM32 UART link.
 *
 *  - One command in flight at a time; the next one goes out only after the
 *    previous reply (or timeout) plus a short gap. Never streamed.
 *  - Replies are matched: "OK FREQ 98.1" must echo the command word that was
 *    sent, so a late reply to an earlier, timed-out command is discarded
 *    instead of being taken as the answer to the next one.
 *  - 500 ms timeout, then one retry (commands are idempotent setters; SAVE is
 *    submitted with retry off to avoid a second flash write).
 *  - JSON lines are accepted at any time (STATUS n sends them unprompted).
 *  - The STM32 boot banner ("fm30ctl x.y ready") is reported as a reset.
 *
 * No Arduino dependencies: the UART and clock come in through LinkIO.
 */
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include "Commands.h"

namespace fm30 {

class LinkIO {
public:
    virtual ~LinkIO() {}
    virtual int readByte() = 0;                              // -1 when nothing is waiting
    virtual void writeBytes(const char *data, size_t len) = 0;
    virtual uint32_t millis() = 0;
};

enum class Outcome : uint8_t { Ok, Err, Timeout };

struct Reply {
    Outcome outcome;
    const char *line;   // reply line ("OK ...", "ERR ...", JSON) or "" on timeout
    const char *body;   // extra lines before the OK (HELP), '\n'-separated
};

class Link {
public:
    using Callback = std::function<void(const Reply &)>;

    static constexpr size_t kQueueLen = 10;
    static constexpr size_t kRxLen = 1024;     // STATUS JSON can reach ~620 bytes
    static constexpr size_t kBodyLen = 1024;   // HELP text
    static constexpr uint32_t kTimeoutMs = 500;
    static constexpr uint32_t kGapMs = 5;

    explicit Link(LinkIO &io) : io_(io) {}

    // Queues a validated command. Returns false if the queue is full (the
    // callback is not called in that case).
    bool submit(const Command &cmd, Callback cb = nullptr, bool retry = true);
    // Same, for a line that's already canonical (internal use and tests).
    bool submitLine(const char *line, Callback cb = nullptr, bool retry = true);

    // Call often: reads the UART, matches replies, handles timeouts, sends.
    void poll();

    size_t pending() const { return count_; }
    bool idle() const { return count_ == 0; }
    // True if a STATUS is queued or in flight (so pollers don't pile up).
    bool statusPending() const;

    // Unsolicited / informational events.
    std::function<void(const char *json)> onJson;
    std::function<void()> onBanner;
    std::function<void(const char *msg)> onLog;

    uint32_t lastRxMs() const { return lastRxMs_; }
    bool everReceived() const { return everRx_; }

    struct Stats { uint32_t sent, timeouts, retries, stale, overflows, errors; };
    const Stats &stats() const { return stats_; }

private:
    struct Entry {
        char line[kMaxLine + 1];
        char expect[12];
        bool expectJson;
        bool retry;
        uint8_t tries;
        Callback cb;
    };

    void handleLine(char *line);
    void complete(Outcome o, const char *line);
    void send(Entry &e);
    void log(const char *fmt, ...);

    LinkIO &io_;
    Entry q_[kQueueLen];
    size_t head_ = 0, count_ = 0;
    bool inFlight_ = false;
    uint32_t sentAt_ = 0, doneAt_ = 0;

    char rx_[kRxLen];
    size_t rxLen_ = 0;
    bool rxOverflow_ = false;
    char body_[kBodyLen];
    size_t bodyLen_ = 0;

    uint32_t lastRxMs_ = 0;
    bool everRx_ = false;
    Stats stats_ = {};
};

}  // namespace fm30
