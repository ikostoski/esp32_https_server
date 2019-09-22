#include "TLSTickets.hpp"
#include "HTTPSServerConstants.hpp"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

namespace httpsserver {

// Low level SSL implementation on ESP32
// Copied from  esp-idf/components/openssl/platform/ssl_pm.c 
struct ssl_pm {    
  mbedtls_net_context fd;
  mbedtls_net_context cl_fd;
  mbedtls_ssl_config conf; 
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ssl_context ssl;
  mbedtls_entropy_context entropy;
};

int TLSTickets::hardware_random(void * p_rng, unsigned char * output, size_t output_len) {
   esp_fill_random(output, output_len);
   return 0;
}

TLSTickets::TLSTickets(const char* tag, uint32_t lifetimeSeconds) {
  _initOk = false;

  #ifdef MBEDTLS_SSL_SESSION_TICKETS
  // Setup TLS tickets context
  mbedtls_ssl_ticket_init(&_ticketCtx);
  int ret = mbedtls_ssl_ticket_setup(
    &_ticketCtx,
    TLSTickets::hardware_random,
    NULL,
    MBEDTLS_CIPHER_AES_256_GCM, 
    lifetimeSeconds
  );
  if (ret != 0) return;

  _initOk = true;
  HTTPS_LOGI("Using TLS session tickets");
  #endif //MBEDTLS_SSL_SESSION_TICKETS 
}
  
bool TLSTickets::enable(SSL * ssl) {
  bool res = false;
  #ifdef MBEDTLS_SSL_SESSION_TICKETS
  if (_initOk) {      
    // Get handle of low-level mbedtls structures for the session
    struct ssl_pm * ssl_pm = (struct ssl_pm *) ssl->ssl_pm;            
    // Configure TLS ticket callbacks using default MbedTLS implementation
    mbedtls_ssl_conf_session_tickets_cb(
      &ssl_pm->conf,
      mbedtls_ssl_ticket_write,
      mbedtls_ssl_ticket_parse,
      &_ticketCtx
    );        
    res = true;
  }
  #endif //MBEDTLS_SSL_SESSION_TICKETS 
  return res;
}
    
} /* namespace httpsserver */