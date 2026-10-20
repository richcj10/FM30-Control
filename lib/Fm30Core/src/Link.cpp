#include "Link.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace fm30 {

namespace {

bool startsWithWord(const char *line, const char *word)
{
    size_t n = strlen(word);
    return !strncmp(line, word, n) && (line[n] == 0 || line[n] == ' ');
}

bool endsWith(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

// "fm30ctl 0.3 ready" (the STM32 prints it once after reset).
bool isBanner(const char *line)
{
    return !strncmp(line, "fm30ctl ", 8) && endsWith(line, " ready");
}

}  // namespace

void Link::log(const char *fmt, ...)
{
    if (!onLog) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    onLog(buf);
}

bool Link::submit(const Command &cmd, Callback cb, bool retry)
{
    return submitLine(cmd.line, std::move(cb), retry);
}

bool Link::submitLine(const char *line, Callback cb, bool retry)
{
    size_t len = strlen(line);
    if (!len || len > kMaxLine) { log("rejected: bad length %u", (unsigned)len); return false; }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c >= 0x7F) { log("rejected: control/non-ASCII byte in command"); return false; }
    }
    if (count_ >= kQueueLen) { log("queue full, dropped: %s", line); return false; }

    Entry &e = q_[(head_ + count_) % kQueueLen];
    memcpy(e.line, line, len + 1);
    const char *w = expectedReplyWord(line, e.expect, sizeof e.expect);
    e.expectJson = (w == nullptr);
    if (w && w != e.expect) snprintf(e.expect, sizeof e.expect, "%s", w);
    e.retry = retry;
    e.tries = 0;
    e.cb = std::move(cb);
    count_++;
    return true;
}

bool Link::statusPending() const
{
    for (size_t i = 0; i < count_; i++)
        if (q_[(head_ + i) % kQueueLen].expectJson) return true;
    return false;
}

void Link::send(Entry &e)
{
    io_.writeBytes(e.line, strlen(e.line));
    io_.writeBytes("\n", 1);
    e.tries++;
    inFlight_ = true;
    sentAt_ = io_.millis();
    bodyLen_ = 0;
    body_[0] = 0;
    stats_.sent++;
}

void Link::complete(Outcome o, const char *line)
{
    Entry &e = q_[head_];
    Callback cb = std::move(e.cb);
    e.cb = nullptr;
    head_ = (head_ + 1) % kQueueLen;
    count_--;
    inFlight_ = false;
    doneAt_ = io_.millis();
    if (cb) {
        Reply r = {o, line, body_};
        cb(r);
    }
}

void Link::poll()
{
    int c;
    while ((c = io_.readByte()) >= 0) {
        if (c == '\r' || c == '\n') {
            if (rxOverflow_) {
                stats_.overflows++;
                log("rx line longer than %u bytes, dropped", (unsigned)(kRxLen - 1));
            } else if (rxLen_) {
                rx_[rxLen_] = 0;
                handleLine(rx_);
            }
            rxLen_ = 0;
            rxOverflow_ = false;
        } else if (c >= 0x20 && c < 0x7F) {    // noise while the STM32 resets is dropped
            if (rxLen_ < kRxLen - 1) rx_[rxLen_++] = (char)c;
            else rxOverflow_ = true;
        }
    }

    uint32_t now = io_.millis();
    if (inFlight_ && now - sentAt_ >= kTimeoutMs) {
        Entry &e = q_[head_];
        if (e.retry && e.tries < 2) {
            stats_.retries++;
            log("timeout, retrying: %s", e.line);
            send(e);
        } else {
            stats_.timeouts++;
            log("timeout: %s", e.line);
            complete(Outcome::Timeout, "");
        }
    }
    if (!inFlight_ && count_ && io_.millis() - doneAt_ >= kGapMs) send(q_[head_]);
}

void Link::handleLine(char *line)
{
    lastRxMs_ = io_.millis();
    everRx_ = true;

    if (line[0] == '{') {
        if (onJson) onJson(line);
        if (inFlight_ && q_[head_].expectJson) complete(Outcome::Ok, line);
        return;
    }

    bool ok = startsWithWord(line, "OK");
    if (ok || startsWithWord(line, "ERR")) {
        if (!inFlight_) {
            stats_.stale++;
            log("reply with nothing in flight, ignored: %s", line);
            return;
        }
        Entry &e = q_[head_];
        if (!ok) {
            stats_.errors++;
            log("%s -> %s", e.line, line);
            complete(Outcome::Err, line);
            return;
        }
        // The STM32 echoes the command word: "OK FREQ 98.1", "OK SAVED", bare "OK".
        const char *w = line + 2;
        while (*w == ' ') w++;
        size_t wl = strcspn(w, " ");
        if (!e.expectJson && wl == strlen(e.expect) && !strncmp(w, e.expect, wl)) {
            complete(Outcome::Ok, line);
        } else {
            stats_.stale++;
            log("stale reply for '%s', ignored: %s", e.line, line);
        }
        return;
    }

    if (isBanner(line)) {
        log("STM32 reset (%s)", line);
        // Whatever was in flight was probably lost; let it time out shortly and retry.
        if (inFlight_) sentAt_ = io_.millis() - kTimeoutMs + 100;
        if (onBanner) onBanner();
        return;
    }
    if (!strncmp(line, "settings:", 9) || !strncmp(line, "clock:", 6)) {
        log("STM32 %s", line);
        return;
    }
    if (inFlight_ && (startsWithWord(q_[head_].line, "HELP") || startsWithWord(q_[head_].line, "DACDBG"))) {
        size_t n = strlen(line);
        if (bodyLen_ + n + 2 < kBodyLen) {
            memcpy(body_ + bodyLen_, line, n);
            bodyLen_ += n;
            body_[bodyLen_++] = '\n';
            body_[bodyLen_] = 0;
        }
        return;
    }
    log("unexpected line: %.80s", line);
}

}  // namespace fm30
