#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"

#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#define WIFI_CONNECTED_BIT BIT0

#define BROKER_URI CONFIG_BROKER_URI

static const char *WIFI_TAG = "Wifi Station";
static const char *MQTT_TAG = "MQTT Client";

static int s_retry_num = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static TaskHandle_t wifi_reconnect_handle = NULL;
static TaskHandle_t send_wol_handle = NULL;

void wifi_reconnect_task(void *arg)
{
    TickType_t retry_delay = pdMS_TO_TICKS(10000);
    TickType_t max_delay = pdMS_TO_TICKS(1200*1000); // 20 min

    while (1) {
        esp_wifi_connect();
        ESP_LOGI(WIFI_TAG, "Attempt to reconnect to AP");
        
        // Blocks for retry_delay ms
        if (ulTaskNotifyTakeIndexed(0, pdTRUE, retry_delay)) {
            break;
        }
        retry_delay *= 1.5;
        if (retry_delay > max_delay) {
            retry_delay = max_delay;
        }
    }
    wifi_reconnect_handle = NULL;
    vTaskDelete(NULL);
}

void send_wol_task(void *arg)
{
    // Set up socket
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_UDP;

    struct sockaddr_in broadcast_addr;
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(9);

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(WIFI_TAG, "Unable to create socket: errno %d", errno);
        send_wol_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int broadcast_enable = 1;
    if ( setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        ESP_LOGE(WIFI_TAG, "setsockopt fail");
        close(sock);
        send_wol_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Magic Packet
    uint8_t packet[102];
    uint8_t mac_addr[6];

    for (int i=0; i<6; i++) {
        char hex[] = {CONFIG_MAC_ADDR[i*3], CONFIG_MAC_ADDR[i*3+1], '\0'};
        mac_addr[i] = (uint8_t) strtoul(hex, NULL, 16);
    }
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet+6*(i+1), mac_addr, 6);
    }

    // Broadcast Packet
    const TickType_t period = pdMS_TO_TICKS(50);
    int success = 1;
    for (int i = 0; i < 5; i++) {
        if ( sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
            success = 0;
        }
        vTaskDelay(period);
    }
    if (!success) {
        esp_mqtt_client_publish((esp_mqtt_client_handle_t) arg, "pc/wol_status", "One or more WoL packets failed to send.", 0, 2, 0);
    } else {
        esp_mqtt_client_publish((esp_mqtt_client_handle_t) arg, "pc/wol_status", "WoL Packet burst sent successfully", 0, 2, 0);
    }

    close(sock);
    send_wol_handle = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_reconnect_handle == NULL) {
            xTaskCreate(wifi_reconnect_task, "Reconnect WiFi", 4096, (void *) 1, 5, &wifi_reconnect_handle);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (wifi_reconnect_handle != NULL) {
            xTaskNotifyGiveIndexed(wifi_reconnect_handle, 0);
        }
    }
}

static void mqtt_event_handler(void* arg, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "pc", 0);
            ESP_LOGI(MQTT_TAG, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
            //Attempt reconnect
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d, reason code=0x%02x ", event->msg_id, (uint8_t)*event->data);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(MQTT_TAG, "TOPIC=%.*s, DATA=%.*s", event->topic_len, event->topic, event->data_len, event->data);
            
            if (!strncmp(event->data, "wake", 4)) {
                ESP_LOGI(MQTT_TAG, "Sending WoL Packet");
                if (send_wol_handle == NULL) {
                    xTaskCreate(send_wol_task, "Send WoL Packet", 4096, (void *) client, 5, &send_wol_handle);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
            ESP_LOGI(MQTT_TAG, "MQTT return code is %d", event->error_handle->connect_return_code);
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGI(MQTT_TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
            //Attempt reconnect
            break;
        default:
            ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);
            break;
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASS,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            #ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
            .disable_wpa3_compatible_mode = 0,
            #endif
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

    // Waiting unti the connection is established (WIFI_CONNECTED_BIT). The bits are set by wifi_event_handler()
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s", CONFIG_WIFI_SSID);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = BROKER_URI,
            .verification.crt_bundle_attach = esp_crt_bundle_attach
        },
        .credentials = {
            .username = CONFIG_MQTT_USER,
            .authentication.password = CONFIG_MQTT_PASS
        }
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    mqtt_app_start();
}