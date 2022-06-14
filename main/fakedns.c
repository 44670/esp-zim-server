// A DNS server for captive portal, respond all queries with the same IP
// address.

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

const uint8_t fDnsTargetIP[] = {192, 168, 4, 1};

#define TAG "dns"

int dnsFD = -1;

typedef struct __attribute__((packed)) dns_header {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
} dns_header_t;

typedef struct __attribute__((packed)) dns_answer_a {
  uint16_t nameRef;
  uint16_t type;
  uint16_t class;
  uint32_t ttl;
  uint16_t len;
  uint8_t addr[4];
} dns_answer_a_t;

void dnsTask() {
  char req[1024];
  int len;
  struct sockaddr_in addr;
  socklen_t addrLen = sizeof(addr);

  while (1) {
    memset(req, 0, sizeof(req));
    len = recvfrom(dnsFD, req, sizeof(req) - 32, 0, (struct sockaddr*)&addr,
                   &addrLen);
    if (len < 0) {
      ESP_LOGE(TAG, "recvfrom failed: %d", errno);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }
    if (len < sizeof(dns_header_t)) {
      ESP_LOGE(TAG, "dns packet too short: %d", len);
      continue;
    }
    dns_header_t* reqHeader = (dns_header_t*)req;
    if (reqHeader->qdcount < 1) {
      ESP_LOGE(TAG, "dns query count < 1: %d", reqHeader->qdcount);
      continue;
    }
    // Convert the request to a response
    reqHeader->flags = htons(0x8180);
    reqHeader->ancount = htons(1);
    dns_answer_a_t* ans = req + len;
    ans->nameRef = htons(0xC00C); // Assume name is at req + 12
    ans->type = htons(0x0001);
    ans->class = htons(0x0001);
    ans->ttl = htonl(64);
    ans->len = htons(4);
    memcpy(ans->addr, fDnsTargetIP, 4);
    len += sizeof(dns_answer_a_t);
    // Send the response
    ESP_LOGI(TAG, "dns response: %d", len);
    if (sendto(dnsFD, req, len, 0, (struct sockaddr*)&addr, addrLen) < 0) {
      ESP_LOGE(TAG, "sendto failed: %d", errno);
    }
  }
  vTaskDelete(NULL);
}

int fdnsBeginServer(void) {
  // Listen at UDP port 53 for DNS requests
  struct sockaddr_in dns_addr;
  memset(&dns_addr, 0, sizeof(dns_addr));
  dns_addr.sin_family = AF_INET;
  dns_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  dns_addr.sin_port = htons(53);
  dnsFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (dnsFD < 0) {
    ESP_LOGE(TAG, "Unable to create a DNS socket: errno %d", errno);
    return dnsFD;
  }
  ESP_LOGI(TAG, "DNS socket created");
  if (bind(dnsFD, (struct sockaddr*)&dns_addr, sizeof(dns_addr)) != 0) {
    ESP_LOGE(TAG, "Unable to bind DNS socket: errno %d", errno);
    return -1;
  }
  ESP_LOGI(TAG, "DNS socket bound");
  xTaskCreatePinnedToCore(&dnsTask, "dnsTask", 4096, NULL, 5, NULL, 0);

  return 0;
}