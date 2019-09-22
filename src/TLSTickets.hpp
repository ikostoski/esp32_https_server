#ifndef SRC_TLSTICKETS_HPP_
#define SRC_TLSTICKETS_HPP_

#include <Arduino.h>
#include "mbedtls/ssl_ticket.h"
#include "openssl/ssl.h"

namespace httpsserver {

/**
 * Enables handling of RFC 5077 TLS session tickets
 */
class TLSTickets {
public:
  TLSTickets(const char* tag, uint32_t liftimeSecs);

  /**
   * Enables TLS ticket processing for SSL session 
   */
  bool enable(SSL * ssl);

protected:
  bool _initOk;

  /**
   * Holds TLS ticket keys
   */
  mbedtls_ssl_ticket_context _ticketCtx;

  /**
   * MbedTLS Random Number Generator using ESP32's hardware RNG
   * 
   * NOTE: Radio (WiFi/Bluetooth) MUST be running for hardware
   * entropy to be gathered. Otherwise this function is PRNG!
   * 
   * See more details about esp_random(), here:
   * https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/system/system.html
   * 
   */
  static int hardware_random(void * p_rng, unsigned char * output, size_t output_len);

};

} /* namespace httpsserver */

#endif // SRC_TLSTICKETS_HPP_