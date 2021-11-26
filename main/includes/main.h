EventGroupHandle_t mqtt_event_group;
/* The event group allows multiple bits for each event */
#define CONNECTED_BIT BIT0
#define DISCONNECTED_BIT BIT1
void aws_iot_task(void);