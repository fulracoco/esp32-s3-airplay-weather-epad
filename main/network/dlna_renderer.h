#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Start the DLNA audio renderer and its SSDP discovery responder. */
esp_err_t dlna_renderer_init(void);

/** Register the UPnP description and SOAP endpoints on the web server. */
esp_err_t dlna_renderer_register(httpd_handle_t server);
