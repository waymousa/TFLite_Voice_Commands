#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "main_functions.h"

#include "wifi.h"
#include "main.h"

extern "C" void app_main_tflite(void) {
  xEventGroupWaitBits(mqtt_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
  setup();
  while (true) {
    loop();
  }
}

