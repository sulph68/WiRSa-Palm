// pbkdf2_fast.h - the iteration loop of PBKDF2-HMAC-SHA256 with the key pads hashed once.
#pragma once
#include <stddef.h>
#include <stdint.h>

// u1 = HMAC(pw, salt || INT(1)) (the first iteration, done by the caller);
// out = u1 ^ u2 ^ ... ^ u<iterations>. iterations >= 1.
void pbkdf2Sha256Iterate(const uint8_t *pw, size_t pwLen, const uint8_t u1[32],
                         uint32_t iterations, uint8_t out[32]);

// True if the last call ran on the SHA hardware (false: software, or no call yet).
bool pbkdf2UsedHardware();
