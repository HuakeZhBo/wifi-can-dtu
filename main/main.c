#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>


#define PORT                        6666
#define LED                         GPIO_NUM_13

int can_dtu_socket;
int sock;
TaskHandle_t blink_task;

static void blink_fast_task()
{
    while (1)
    {
        gpio_set_level(LED, 1);
        vTaskDelay(400 / portTICK_PERIOD_MS);
        gpio_set_level(LED, 0);
        vTaskDelay(400 / portTICK_PERIOD_MS);
    }
}
static void tcp_server_task()
{
    int keepAlive = 1;
    int keepIdle = 5;
    int keepInterval = 5;
    int keepCount = 3;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        // accept函数是阻塞调用 直到接收到tcp连接才会继续执行
        sock = accept(can_dtu_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0)   break;
        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
        if (client_addr.sin_family == PF_INET) {
            char addr_str[16];
            inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
            printf("Connection from: %s\n", addr_str);
        }
        xTaskCreate(blink_fast_task, "blink_fast", 2048, NULL, 2, &blink_task);
        int len;
        char rx_buffer[16];

        do {
            len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                printf("Error%d at receiving\n", errno);
            } else if (len == 0) {
                printf("Connection closed\n");
            } else {
                rx_buffer[len] = 0;
                printf("Rx:%s\n", rx_buffer);
                twai_message_t TxMsg = {
                    .extd = 0,
                    .identifier = rx_buffer[0],
                    .data_length_code = 8,
                    .data = {1, 2, 3, 4, 5, 6, 7, 8}
                };
                if(ESP_OK != twai_transmit(&TxMsg, pdMS_TO_TICKS(100)))
                    printf("Send Failed\n");
                else    printf("Send OK\n");
            }
        } while (len > 0);
        vTaskDelete(blink_task);
        gpio_set_level(LED, 0);
        shutdown(sock, 0);
        close(sock);
    }
    close(can_dtu_socket);
    vTaskDelete(NULL);
}
// static void ap_handler(void* arg, esp_event_base_t event_base,
//                                     int32_t event_id, void* event_data)
// {
//     printf("ap_handler\n");
// }
static void can_task()
{
    char tx_buffer[16];
    // CAN 初始化
    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_5, GPIO_NUM_4, TWAI_MODE_NORMAL);
    twai_timing_config_t t_cfg = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (ESP_OK != twai_driver_install(&g_cfg, &t_cfg, &f_cfg))
    {
        printf("CAN Initial Failed\n");
        return;
    }
    else if(ESP_OK != twai_start())
    {
        printf("CAN Start Failed\n");
        return;
    }
    printf("CAN driver installed\n");
    twai_message_t RxMsg;
    while(1)
    {
        if(ESP_OK == twai_receive(&RxMsg, pdMS_TO_TICKS(1000)))
        {
            tx_buffer[0] = RxMsg.data[0];
            tx_buffer[1] = RxMsg.data[1];
            tx_buffer[2] = RxMsg.data[2];
            tx_buffer[3] = RxMsg.data[3];
            tx_buffer[4] = RxMsg.data[4];
            tx_buffer[5] = RxMsg.data[5];
            tx_buffer[6] = RxMsg.data[6];
            tx_buffer[7] = RxMsg.data[7];
            int len = send(sock, tx_buffer, 12, 0);
            if(len > 0)
                printf("id:%ld dlc:%d data:\n",RxMsg.identifier, RxMsg.data_length_code);
        }
    }
    vTaskDelete(NULL);
}
void app_main(void)
{
    // LED GPIO 初始化
    gpio_reset_pin(LED);
    gpio_set_direction(LED, GPIO_MODE_OUTPUT);

    // WIFI AP 模式初始化
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_config);
    // esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_handler, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_ap_config_t ap_cfg = {
                        .ssid = "wifi-can-dtu",
                        .password = "cidi20171016",
                        .ssid_len = 12,
                        .max_connection = 1,
                        .authmode = WIFI_AUTH_WPA_WPA2_PSK};
    esp_wifi_set_config(WIFI_IF_AP, (wifi_config_t*)(&ap_cfg));
    esp_wifi_start();

    // socket 套接字创建并绑定端口
    int opt = 1;
    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    can_dtu_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (can_dtu_socket < 0)    return;
    setsockopt(can_dtu_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(can_dtu_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0)   return;
    if (listen(can_dtu_socket, 2) != 0)    return;

    // 创建tcp任务
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    xTaskCreate(can_task, "can", 2048, NULL, 2, &blink_task);
    return;
}
