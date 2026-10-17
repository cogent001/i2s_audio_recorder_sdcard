/* I2S Digital Microphone Recording Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

static const char* TAG = "pdm_rec_example";

#define CONFIG_EXAMPLE_PIN_CMD  38
#define CONFIG_EXAMPLE_PIN_CLK  39
#define CONFIG_EXAMPLE_PIN_D0   40

#define NUM_CHANNELS        (1) // For mono recording only!
#define SD_MOUNT_POINT      "/sdcard"
#define SAMPLE_SIZE         (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE           (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS

// When testing SD and SPI modes, keep in mind that once the card has been
// initialized in SPI mode, it can not be reinitialized in SD mode without
// toggling power to the card.
static int16_t i2s_readraw_buff[SAMPLE_SIZE];
size_t bytes_read;
const int WAVE_HEADER_SIZE = 44;

sdmmc_card_t *card;
sdmmc_host_t host = SDMMC_HOST_DEFAULT();

void mount_sdcard(void)
{
    esp_err_t ret;
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 8 * 1024
    };
   
    const char mount_point[] = SD_MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");
    
        
    ESP_LOGI(TAG, "Using SDMMC peripheral");    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    
    sdmmc_card_print_info(stdout, card);
}

void generate_wav_header(char* wav_header, uint32_t wav_size, uint32_t sample_rate){

    // See this for reference: http://soundfile.sapp.org/doc/WaveFormat/
    uint32_t file_size = wav_size + WAVE_HEADER_SIZE - 8;
    uint32_t byte_rate = BYTE_RATE;

    const char set_wav_header[] = {
        'R','I','F','F', // ChunkID
        file_size, file_size >> 8, file_size >> 16, file_size >> 24, // ChunkSize
        'W','A','V','E', // Format
        'f','m','t',' ', // Subchunk1ID
        0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
        0x01, 0x00, // AudioFormat (1 for PCM)
        0x01, 0x00, // NumChannels (1 channel)
        sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24, // SampleRate
        byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24, // ByteRate
        0x02, 0x00, // BlockAlign
        0x10, 0x00, // BitsPerSample (16 bits)
        'd','a','t','a', // Subchunk2ID
        wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24, // Subchunk2Size
    };

    memcpy(wav_header, set_wav_header, sizeof(set_wav_header));
}

void record_wav(uint32_t rec_time)
{
    // Use POSIX and C standard library functions to work with files.
    int flash_wr_size = 0;
    ESP_LOGI(TAG, "Opening file");

    char wav_header_fmt[WAVE_HEADER_SIZE];

    uint32_t flash_rec_time = BYTE_RATE * rec_time;
    generate_wav_header(wav_header_fmt, flash_rec_time, CONFIG_EXAMPLE_SAMPLE_RATE);

    // First check if file exists before creating a new file.
    struct stat st;
    if (stat(SD_MOUNT_POINT"/record.wav", &st) == 0) {
        // Delete it if it exists
        unlink(SD_MOUNT_POINT"/record.wav");
    }

    // Create new WAV file
    FILE* f = fopen(SD_MOUNT_POINT"/record.wav", "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }

    // Write the header to the WAV file
    fwrite(wav_header_fmt, 1, WAVE_HEADER_SIZE, f);

    // Start recording
    while (flash_wr_size < flash_rec_time) {
        // Read the RAW samples from the microphone
        i2s_read(CONFIG_EXAMPLE_I2S_CH, (char *)i2s_readraw_buff, SAMPLE_SIZE, &bytes_read, 100);
        // Write the samples to the WAV file
        fwrite(i2s_readraw_buff, 1, bytes_read, f);
        flash_wr_size += bytes_read;
    }

    ESP_LOGI(TAG, "Recording done!");
    fclose(f);
    ESP_LOGI(TAG, "File written on SDCard");

    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    ESP_LOGI(TAG, "Card unmounted");
    // Deinitialize the bus after all devices are removed
    
}

void init_microphone(void)
{
    // Set the I2S configuration as PDM and 16bits per sample
    /*
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = CONFIG_EXAMPLE_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .dma_buf_count = 8,
        .dma_buf_len = 200,
        .use_apll = 0,
    };

    // Set the pinout configuration (set using menuconfig)
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_PIN_NO_CHANGE, 
        .ws_io_num = CONFIG_EXAMPLE_I2S_CLK_GPIO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = CONFIG_EXAMPLE_I2S_DATA_GPIO,
    };
    */

    static const i2s_config_t i2s_config = 
    {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), // the mode must be set according to DSP configuration
        .sample_rate = 16000,                                // must be the same as DSP configuration
        .bits_per_sample = (i2s_bits_per_sample_t)32,        // must be the same as DSP configuration
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,         // must be the same as DSP configuration
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .dma_buf_count = 3,
        .dma_buf_len = 300,
    };

    static const i2s_pin_config_t pin_config = 
    {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = 41,                  // IIS_SCLK
        .ws_io_num = 42,                   // IIS_LCLK
        .data_out_num = I2S_PIN_NO_CHANGE, // IIS_DSIN
        .data_in_num = 2,                   // IIS_DOUT
    };

    

    // Call driver installation function before any I2S R/W operation.
    ESP_ERROR_CHECK( i2s_driver_install(CONFIG_EXAMPLE_I2S_CH, &i2s_config, 0, NULL) );
    ESP_ERROR_CHECK( i2s_set_pin(CONFIG_EXAMPLE_I2S_CH, &pin_config) );
    ESP_ERROR_CHECK( i2s_zero_dma_buffer(CONFIG_EXAMPLE_I2S_CH) );
    ESP_ERROR_CHECK( i2s_set_clk(CONFIG_EXAMPLE_I2S_CH, CONFIG_EXAMPLE_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO) );
}

void app_main(void)
{
    ESP_LOGI(TAG, "PDM microphone recording Example start");
    // Mount the SDCard for recording the audio file
    mount_sdcard();
    // Init the PDM digital microphone
    init_microphone();
    ESP_LOGI(TAG, "Starting recording for %d seconds!", CONFIG_EXAMPLE_REC_TIME);
    // Start Recording
    record_wav(CONFIG_EXAMPLE_REC_TIME);
    // Stop I2S driver and destroy
    ESP_ERROR_CHECK( i2s_driver_uninstall(CONFIG_EXAMPLE_I2S_CH) );
}
