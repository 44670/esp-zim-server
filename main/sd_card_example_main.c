/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// This example uses SDMMC peripheral to communicate with SD card.
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

static const char *TAG = "example";

FIL zimFile;
DWORD zimFileClTbl[128];
int zimFileReady = 0;


/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE (200 * 1024)  // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE 8192

struct file_server_data {
  /* Base path of file storage */
  char base_path[ESP_VFS_PATH_MAX + 1];

  /* Scratch buffer for temporary storage during file transfer */
  char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
  const size_t favicon_ico_size = 0;
  httpd_resp_set_type(req, "image/x-icon");
  httpd_resp_send(req, (const char *)"none", favicon_ico_size);
  return ESP_OK;
}

uint8_t zimTmpBuf[32 * 1024];
static esp_err_t zim_get_handler(httpd_req_t *req) {
    if (!zimFileReady) {
        httpd_resp_send_err(req, 500, "ZIM file not ready");
        return ESP_OK;
    }
    const char* uri = req->uri;
    const char* quest = strchr(uri, '?');
    const char* comma = strchr(uri, ',');
    if ((!quest) || (!comma)) {
        httpd_resp_send_err(req, 500, "No param");
        return ESP_OK;
    }
    // ?123,456 where 123 is offset and 456 is length
    int64_t offset = _atoi64(quest + 1);
    int length = atoi(comma + 1);
    ESP_LOGI("[zim] read offset: %d, length: %d", offset, length);
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
    while(remainLength > 0) {
        int readSize = MIN(sizeof(zimTmpBuf), remainLength);
        UINT bytesRead = 0;
        f_read(&zimFile, zimTmpBuf, readSize, &bytesRead);
        if (bytesRead > 0) {
            httpd_resp_send_chunk(req, (const char*)zimTmpBuf, bytesRead);
            remainLength -= bytesRead;
        } else {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);

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
  } else if (IS_FILE_EXT(filename, ".jpeg")) {
    return httpd_resp_set_type(req, "image/jpeg");
  } else if (IS_FILE_EXT(filename, ".ico")) {
    return httpd_resp_set_type(req, "image/x-icon");
  }
  /* This is a limited set only */
  /* For any other type always set as plain text */
  return httpd_resp_set_type(req, "text/plain");
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
  char filepath[FILE_PATH_MAX];
  FILE *fd = NULL;

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
  if (strcmp(filename, "/zim") == 0) {
      return zim_get_handler(req);
  }
  ESP_LOGI(TAG, "Sending file: %s", filename);
  fd = fopen(filepath, "r");
  if (!fd) {
    ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
    /* Respond with 500 Internal Server Error */
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to open file");
    return ESP_FAIL;
  }
  set_content_type_from_file(req, filename);

  /* Retrieve the pointer to scratch buffer for temporary storage */
  char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
  size_t chunksize;
  do {
    /* Read file in chunks into the scratch buffer */
    chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

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

  /* Respond with an empty chunk to signal HTTP response completion */
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

void usbEnablePullup(int enabled);

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
#if 0
    /* URI handler for uploading files to server */
    httpd_uri_t file_upload = {
        .uri       = "/upload/*",   // Match all URIs of type /upload/path/to/file
        .method    = HTTP_POST,
        .handler   = upload_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_upload);

    /* URI handler for deleting files from server */
    httpd_uri_t file_delete = {
        .uri       = "/delete/*",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = delete_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_delete);
#endif




  return ESP_OK;
}



void app_main(void) {
  esp_err_t ret;

  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
  };
  sdmmc_card_t *card;
  ESP_LOGI(TAG, "Initializing SD card");

  // Use settings defined above to initialize SD card and mount FAT filesystem.
  // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
  // Please check its source code and implement error recovery when developing
  // production applications.

  ESP_LOGI(TAG, "Using SDMMC peripheral");
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
  // has these signals.
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  // To use 1-line SD mode, change this to 1:
  slot_config.width = 1;

  slot_config.cmd = GPIO_NUM_35;
  slot_config.clk = GPIO_NUM_36;
  slot_config.d0 = GPIO_NUM_37;

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
