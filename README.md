# esp-zim-server

An ESP32/ESP32S3-based ZIM server, serve ZIM files from exfat-formatted SD card.

# Get Started
1. Download and install ESP-IDF, the master branch of ESP-IDF is strongly recommended.
2. You need to patch ESP-IDF to support exfat: 
```
Edit esp-idf/components/fatfs/src/ffconf.h, Replace following lines:
#define FF_USE_FASTSEEK	CONFIG_FATFS_USE_FASTSEEK 
-> #define FF_USE_FASTSEEK	1
#define FF_FS_EXFAT		0 
-> #define FF_FS_EXFAT		1

Edit esp-idf/components/fatfs/vfs/vfs_fat.c, Replace following lines:
ESP_LOGD(TAG, "%s: offset=%ld, filesize:=%d", __func__, new_pos, f_size(file)); 
-> ESP_LOGD(TAG, "%s: offset=%ld, filesize:=%lld", __func__, new_pos, (long long)f_size(file));
```
3. Edit config.h, set the IO pins for your board. Please note that only ESP32S3 allows changing the IO pins of the SDIO.
4. Build and flash the project.
5. Format the SD card with exfat, copy www folder to your SD card, copy and rename your ZIM file to 'zim.zim' in the root of the SD card.
6. Connect to the AP, visit http://192.168.4.1 .
7. Enjoy!
