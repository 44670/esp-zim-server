
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/periph_ctrl.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_phy_hal.h"
#include "hal/usb_phy_ll.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "soc/usb_wrap_struct.h"

static const char *TAG = "main";

FIL zimFile;
DWORD zimFileClTbl[128];
int zimFileReady = 0;

uint8_t tmpBuf[32 * 1024];

struct file_server_data {
  /* Base path of file storage */
  char base_path[ESP_VFS_PATH_MAX + 1];
};

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
  const size_t favicon_ico_size = 0;
  httpd_resp_set_type(req, "image/x-icon");
  httpd_resp_send(req, (const char *)"none", favicon_ico_size);
  return ESP_OK;
}

uint64_t parseUInt64(const char *str) {
  uint64_t ret = 0;
  while (*str >= '0' && *str <= '9') {
    ret *= 10;
    ret += *str - '0';
    str++;
  }
  return ret;
}

static esp_err_t zim_get_handler(httpd_req_t *req) {
  if (!zimFileReady) {
    httpd_resp_send_err(req, 500, "ZIM file not ready");
    return ESP_OK;
  }
  const char *uri = req->uri;
  const char *quest = strchr(uri, '?');
  const char *comma = strchr(uri, ',');
  if ((!quest) || (!comma)) {
    httpd_resp_send_err(req, 500, "No param");
    return ESP_OK;
  }
  // ?123,456 where 123 is offset and 456 is length
  int64_t offset = parseUInt64(quest + 1);
  int length = atoi(comma + 1);
  // ESP_LOGI(TAG, "[zim] read offset: %lld, length: %d", offset, length);
  if (offset < 0 || length < 0) {
    httpd_resp_send_err(req, 500, "Invalid param");
    return ESP_OK;
  }
  if (length > 10 * 1024 * 1024) {
    httpd_resp_send_err(req, 500, "Too large");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  f_lseek(&zimFile, offset);
  int remainLength = length;
  while (remainLength > 0) {
    int readSize = MIN(sizeof(tmpBuf), remainLength);
    UINT bytesRead = 0;
    f_read(&zimFile, tmpBuf, readSize, &bytesRead);
    if (bytesRead > 0) {
      httpd_resp_send_chunk(req, (const char *)tmpBuf, bytesRead);
      remainLength -= bytesRead;
    } else {
      break;
    }
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

#define IS_FILE_EXT(filename, ext) \
  (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req,
                                            const char *filename) {
  if (IS_FILE_EXT(filename, ".pdf")) {
    return httpd_resp_set_type(req, "application/pdf");
  } else if (IS_FILE_EXT(filename, ".html")) {
    return httpd_resp_set_type(req, "text/html");
  } else if (IS_FILE_EXT(filename, ".jpg")) {
    return httpd_resp_set_type(req, "image/jpeg");
  } else if (IS_FILE_EXT(filename, ".ico")) {
    return httpd_resp_set_type(req, "image/x-icon");
  } else if (IS_FILE_EXT(filename, ".css")) {
    return httpd_resp_set_type(req, "text/css");
  } else if (IS_FILE_EXT(filename, ".js")) {
    return httpd_resp_set_type(req, "application/javascript");
  } else if (IS_FILE_EXT(filename, ".wasm")) {
    return httpd_resp_set_type(req, "application/wasm");
  }
  /* This is a limited set only */
  /* For any other type always set as plain text */
  return httpd_resp_set_type(req, "application/octet-stream");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char *get_path_from_uri(char *dest, const char *base_path,
                                     const char *uri, size_t destsize) {
  const size_t base_pathlen = strlen(base_path);
  size_t pathlen = strlen(uri);

  const char *quest = strchr(uri, '?');
  if (quest) {
    pathlen = MIN(pathlen, quest - uri);
  }
  const char *hash = strchr(uri, '#');
  if (hash) {
    pathlen = MIN(pathlen, hash - uri);
  }

  if (base_pathlen + pathlen + 1 > destsize) {
    /* Full path string won't fit into destination buffer */
    return NULL;
  }
  if ((pathlen == 1) && (uri[0] == '/')) {
    /* Special case for root path */
    strcpy(dest, base_path);
    strcpy(dest + base_pathlen, "/index.html");
    return dest + base_pathlen;
  }

  /* Construct full path (base + path) */
  strcpy(dest, base_path);
  strlcpy(dest + base_pathlen, uri, pathlen + 1);

  /* Return pointer to path, skipping the base */
  return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t download_get_handler(httpd_req_t *req) {
  char filepath[128];
  char hostBuf[128];
  memset(filepath, 0, sizeof(filepath));
  memset(hostBuf, 0, sizeof(hostBuf));
  FILE *fd = NULL;

  // get host in req header
  if (httpd_req_get_hdr_value_str(req, "Host", hostBuf, sizeof(hostBuf)) ==
      ESP_OK) {
    if (strcmp(hostBuf, "192.168.4.1") != 0) {
      ESP_LOGI(TAG, "Redirect: %s", hostBuf);
      // redirect to http://192.168.4.1
      httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
      httpd_resp_set_status(req, "301 Moved Permanently");
      httpd_resp_send(req, NULL, 0);
      return ESP_OK;
    }
  }
  // if uri begin with /zim?, call zim_get_handler
  if (strncmp(req->uri, "/zim?", 5) == 0) {
    return zim_get_handler(req);
  }

  const char *filename = get_path_from_uri(
      filepath, ((struct file_server_data *)req->user_ctx)->base_path, req->uri,
      sizeof(filepath));
  if (!filename) {
    ESP_LOGE(TAG, "Filename is too long");
    /* Respond with 500 Internal Server Error */
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Filename too long");
    return ESP_FAIL;
  }
  if (strcmp(filename, "/favicon.ico") == 0) {
    return favicon_get_handler(req);
  }

  ESP_LOGI(TAG, "Sending file: %s", filename);
  fd = fopen(filepath, "r");
  if (!fd) {
    /* Respond with 500 Internal Server Error */
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to open file");
    return ESP_FAIL;
  }
  set_content_type_from_file(req, filename);

  /* Retrieve the pointer to scratch buffer for temporary storage */
  char *chunk = (char *)tmpBuf;
  size_t chunksize;
  do {
    /* Read file in chunks into the scratch buffer */
    chunksize = fread(chunk, 1, sizeof(tmpBuf), fd);

    if (chunksize > 0) {
      /* Send the buffer contents as HTTP response chunk */
      if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
        fclose(fd);
        ESP_LOGE(TAG, "File sending failed!");
        /* Abort sending file */
        httpd_resp_sendstr_chunk(req, NULL);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to send file");
        return ESP_FAIL;
      }
    }

    /* Keep looping till the whole file is sent */
  } while (chunksize != 0);

  /* Close file after sending complete */
  fclose(fd);
  ESP_LOGI(TAG, "File sending complete");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

/* Function to start the file server */
esp_err_t start_file_server(const char *base_path) {
  static struct file_server_data *server_data = NULL;

  if (server_data) {
    ESP_LOGE(TAG, "File server already started");
    return ESP_ERR_INVALID_STATE;
  }

  /* Allocate memory for server data */
  server_data = calloc(1, sizeof(struct file_server_data));
  if (!server_data) {
    ESP_LOGE(TAG, "Failed to allocate memory for server data");
    return ESP_ERR_NO_MEM;
  }
  strlcpy(server_data->base_path, base_path, sizeof(server_data->base_path));

  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true; // Kill inactive connections
  /* Use the URI wildcard matching function in order to
   * allow the same handler to respond to multiple different
   * target URIs which match the wildcard scheme */
  config.uri_match_fn = httpd_uri_match_wildcard;

  ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start file server!");
    return ESP_FAIL;
  }

  /* URI handler for getting uploaded files */
  httpd_uri_t file_download = {
      .uri = "/*",  // Match all URIs of type /path/to/file
      .method = HTTP_GET,
      .handler = download_get_handler,
      .user_ctx = server_data  // Pass server data as context
  };
  httpd_register_uri_handler(server, &file_download);
  return ESP_OK;
}

int wifiEarlyInit() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  // Do not use NVS
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  return 0;
}

int wifiSwitchToSTAMode(const char *ssid, const char *psk) {
  wifi_config_t wifi_config = {
      .sta =
          {
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };
  strcpy((char *)wifi_config.sta.ssid, ssid);
  strcpy((char *)wifi_config.sta.password, psk);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_connect();
  return 0;
}

int wifiSwitchToAPMode(const char *ssid, const char *psk, int maxConns,
                       int channel) {
  wifi_config_t wifi_config = {
      .ap =
          {
              .channel = channel,
              .max_connection = maxConns,
              .authmode = WIFI_AUTH_WPA_WPA2_PSK,
              .pmf_cfg =
                  {
                      .required = false,
                  },
          },
  };
  strcpy((char *)wifi_config.ap.ssid, ssid);
  strcpy((char *)wifi_config.ap.password, psk);
  wifi_config.ap.ssid_len = strlen(ssid);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "Switch to AP mode, ssid: %s, psk: %s", ssid, psk);
  return 0;
}

int fdnsBeginServer(void);

void app_main(void) {
  esp_err_t ret;

  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 15,
  };
  sdmmc_card_t *card;
  ESP_LOGI(TAG, "Initializing SD card");

  // Use settings defined above to initialize SD card and mount FAT
  // filesystem. Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience
  // functions. Please check its source code and implement error recovery when
  // developing production applications.

  ESP_LOGI(TAG, "Using SDMMC peripheral");
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
  // has these signals.
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  // To use 1-line SD mode, change this to 1:
  slot_config.width = 1;

  slot_config.cmd = PIN_SD_CMD;
  slot_config.clk = PIN_SD_CLK;
  slot_config.d0 = PIN_SD_DAT0;

  // Enable internal pullups on enabled pins. The internal pullups
  // are insufficient however, please make sure 10k external pullups are
  // connected on the bus. This is for debug / example purpose only.
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret =
      esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG,
               "Failed to mount filesystem. "
               "If you want the card to be formatted, set the "
               "EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
    } else {
      ESP_LOGE(TAG,
               "Failed to initialize the card (%s). "
               "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
    }
    return;
  }
  ESP_LOGI(TAG, "Filesystem mounted");

  ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    printf("Erasing NVS...\n");
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_flash_init();
  }
  wifiEarlyInit();
  wifiSwitchToAPMode(WIFI_AP_SSID, WIFI_AP_PSK, 10, 6);
  start_file_server("/sd/www");
  fdnsBeginServer();

  // Card has been initialized, print its properties
  sdmmc_card_print_info(stdout, card);
  FRESULT fr = f_open(&zimFile, "0:/zim.zim", FA_READ);
  if (fr == FR_OK) {
    ESP_LOGI(TAG, "ZIM File opened");
  } else {
    ESP_LOGE(TAG, "Failed to open file (%d)", fr);
    return;
  }
  zimFile.cltbl = zimFileClTbl;
  zimFile.cltbl[0] = sizeof(zimFileClTbl) / sizeof(DWORD);
  fr = f_lseek(&zimFile, CREATE_LINKMAP);
  if (fr == FR_OK) {
    ESP_LOGI(TAG, "Linkmap created");
  } else {
    ESP_LOGE(TAG, "Failed to create linkmap (%d), the file is too fragmented?",
             fr);
    return;
  }
  zimFileReady = 1;
}
