#pragma once
/*
 * Validation of commands headed for the STM32.
 *
 * Everything from the network is treated as untrusted. Input is parsed into
 * values, range-checked against the same limits the STM32 uses (cli.c), and
 * then a fresh, canonical command line is built from those values. Raw input
 * is never forwarded, so it can't smuggle a second command, an octal number
 * ("POWER 080" would be strtol base 0 on the STM32) or an over-long line.
 */
#include <stddef.h>
#include <stdint.h>

namespace fm30 {

constexpr size_t kMaxLine = 127;       // STM32 line buffer is 128 incl. NUL
constexpr int kPowerMax = 3968;        // POWER_MAX (0xF80) in the STM32 config.h
constexpr size_t kPsLen = 8;
constexpr size_t kRtLen = 64;
constexpr size_t kLcdTopLen = 8;
constexpr size_t kLcdBottomLen = 48;
constexpr int kLcdSlots = 4;
constexpr int kLcdPages = 8;

enum class Cmd : uint8_t {
    Freq, Power, Tx, Vol, Bal, U6Trim, U6, VtScale,
    Rds, RdsFree, RdsLevel, RdsPhase, Pi, Pty, Flag, Ps, Rt,
    LcdOnOff, LcdPage, LcdMsg, LcdRotate,
    Status, Save, Defaults, Help,
};

struct Command {
    char line[kMaxLine + 1];   // canonical line, no terminator
    Cmd cmd;
    // Set for Cmd::LcdMsg so the caller can keep its own copy of the slot.
    int lcdSlot;
    char lcdTop[kLcdTopLen + 1];
    char lcdBottom[kLcdBottomLen + 1];
    // Set for Cmd::LcdRotate: bit n-1 = page n.
    uint8_t rotateMask;
};

// Parses free-form input such as "freq 98.1" or "LCD MSG 2 Hi|there".
// Returns false and a short reason in err when the input is invalid.
bool parseCommand(const char *input, Command &out, char *err, size_t errLen);

// Builders for the convenience endpoints; text is sanitized and clipped.
void buildPs(const char *text, Command &out);
void buildRt(const char *text, Command &out);
bool buildLcdMsg(int slot, const char *top, const char *bottom, Command &out, char *err, size_t errLen);
bool buildPowerPercent(int percent, Command &out, char *err, size_t errLen);
bool buildFreq(const char *mhz, Command &out, char *err, size_t errLen);

// Commands behind the control password: anything that changes what goes on
// air at RF level (frequency, power, deviation/tuning internals, RDS
// injection) and DEFAULTS, which resets frequency and power.
bool isProtected(Cmd cmd);

// The word the STM32 echoes after "OK" for this command line: "FREQ" for
// "FREQ 98.1", "SAVED" for "SAVE", "" when the reply is a bare "OK".
// Returns nullptr when the reply is a JSON line (STATUS).
const char *expectedReplyWord(const char *line, char *buf, size_t bufLen);

}  // namespace fm30
