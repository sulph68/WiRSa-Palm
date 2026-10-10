// tls_strerror.c - replaces mbedTLS's mbedtls_strerror() to save ~15KB of flash.
//
// The only callers are the Arduino core's WiFiClientSecure (ssl_client.cpp's
// _handle_error and WiFiClientSecure::lastError), which use the text for a
// log_e line that already prints the numeric code. The real implementation
// lives in libmbedcrypto.a's error.c.obj together with ~15KB of error-message
// strings; because this object defines the symbol first, the linker never
// pulls that archive member in. Nothing in WiRSa itself reads the text.
//
// Output: "mbedtls -0x7780" - look the code up in mbedtls/ssl.h et al.

#include <stdio.h>
#include <stddef.h>

void mbedtls_strerror(int ret, char *buf, size_t buflen) {
  if (buf == NULL || buflen == 0) return;
  snprintf(buf, buflen, "mbedtls -0x%04X", (unsigned) (ret < 0 ? -ret : ret));
}
