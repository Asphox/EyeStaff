#include <Arduino.h>
#include <TFT_eSPI.h>
#include <FreeRTOS.h>
#include <lvgl.h>

TFT_eSPI tft = TFT_eSPI();


#define TFT_DMA_BUFFER_SIZE (TFT_WIDTH*TFT_HEIGHT)
#define TFT_PX_COUNT TFT_DMA_BUFFER_SIZE
DRAM_ATTR uint16_t dma_buffer_1[TFT_DMA_BUFFER_SIZE];
DRAM_ATTR uint16_t dma_buffer_2[TFT_DMA_BUFFER_SIZE];

uint16_t* screen_framebuffer = nullptr;
uint16_t* screen_backbuffer = nullptr;

TaskHandle_t xHandleTaskMain = NULL;
TaskHandle_t xHandleTaskUpdateScreen = NULL;
lv_disp_draw_buf_t  lvgl_draw_context;
lv_disp_drv_t       lvgl_drv_context;

#define DMA_BATCH_MAX_SIZE 0x4000
#define DMA_BATCH_SLEEP_TIME ((DMA_BATCH_MAX_SIZE*16UL*1000UL)/SPI_FREQUENCY)
#define NOTIFY_TASK_UPDATE_SCREEN_READY_TO_DRAW (1UL << 0UL)
#define NOTIFY_TASK_MAIN_SCREEN_UPDATE_STARTED (1UL << 0UL)
#define NOTIFY_TASK_MAIN_SCREEN_UPDATE_COMPLETED (1UL << 1UL)
#define MASK_NOTIFY_MAIN_SCREEN_UPDATE (NOTIFY_TASK_MAIN_SCREEN_UPDATE_STARTED|NOTIFY_TASK_MAIN_SCREEN_UPDATE_COMPLETED)
/**
* @brief Update the TFT screen by switching screenbackbuffer and screenframebuffer and sending it via DMA SPI.
*/
void vTaskUpdateScreen(void* /*pvParameters*/)
{
  // On ESP32 we MUST use a spinlock because of the multicore architecture.
  portMUX_TYPE spinlock_critical_section = portMUX_INITIALIZER_UNLOCKED;
  uint32_t notification_value;
  while(true)
  {
    // Wait for a notification to wake up the task (when the framebuffer is ready to be drawn).
    xTaskNotifyWait(0,
                    NOTIFY_TASK_UPDATE_SCREEN_READY_TO_DRAW,
                    &notification_value,
                    portMAX_DELAY);
    // swap screen_framebuffer and screen_backbuffer
    {
      // Protects the swap with a critical section to prevent preemption of the swap operation
      taskENTER_CRITICAL(&spinlock_critical_section);
      uint16_t* tmp = screen_backbuffer;
      screen_backbuffer = screen_framebuffer;
      screen_framebuffer = tmp;
      taskEXIT_CRITICAL(&spinlock_critical_section);
    }

    // Notify the main task that the update has started
    xTaskNotify(xHandleTaskMain, NOTIFY_TASK_MAIN_SCREEN_UPDATE_STARTED, eSetBits);

    // Batches the DMA transfer with 16k pixels in a packet. 
    // #TODO may be increased to 32k pixels by using a custom implementation... pushPixelsDMA is shit !
    int32_t buffer_size_to_send = TFT_DMA_BUFFER_SIZE;
    uint16_t* start_buffer_to_send = screen_framebuffer;
    tft.startWrite();
    tft.setAddrWindow(0, 0, TFT_WIDTH, TFT_HEIGHT);
    while(buffer_size_to_send>=DMA_BATCH_MAX_SIZE)
    {
      tft.pushPixelsDMA(start_buffer_to_send, DMA_BATCH_MAX_SIZE);
      start_buffer_to_send += DMA_BATCH_MAX_SIZE;
      buffer_size_to_send -= DMA_BATCH_MAX_SIZE;
      // Delay the task for an determined amout of time (lower priority tasks may be executed)
      vTaskDelay(pdMS_TO_TICKS(DMA_BATCH_SLEEP_TIME));
      // Yield for the last "busy" moments
      while(tft.dmaBusy())
        taskYIELD();
    }
    tft.pushPixelsDMA(start_buffer_to_send, buffer_size_to_send);
    while(tft.dmaBusy())
        taskYIELD();
    tft.endWrite();
    // Notify the main task that screen is fully updated
    xTaskNotify(xHandleTaskMain, NOTIFY_TASK_MAIN_SCREEN_UPDATE_COMPLETED, eSetBits);
  }
}

/* 
* @brief function called by LVGL to flush the framebuffer
* We do nothing has we manage the doublebuffering and update ourself
*/
void lvgl_flush_buffer(lv_disp_drv_t*, const lv_area_t*, lv_color_t*)
{}

void lvgl_log(const char* str)
{
  Serial.println(str);
}

void setup() {
  Serial.begin(115200);
  memset(dma_buffer_1, 0x00, TFT_DMA_BUFFER_SIZE*2);
  memset(dma_buffer_2, 0x00, TFT_DMA_BUFFER_SIZE*2);
  // put your setup code here, to run once:
  tft.init();
  tft.setRotation(0);
  tft.initDMA();
  tft.setSwapBytes(true);

  screen_framebuffer = dma_buffer_1;
  screen_backbuffer = dma_buffer_2;

  // Init LVGL
  lv_init();
  lv_log_register_print_cb(&lvgl_log);
  lv_disp_draw_buf_init(&lvgl_draw_context, screen_backbuffer, NULL, TFT_PX_COUNT);
  lv_disp_drv_init(&lvgl_drv_context);
  lvgl_drv_context.hor_res = TFT_WIDTH;
  lvgl_drv_context.ver_res = TFT_HEIGHT;
  lvgl_drv_context.flush_cb = &lvgl_flush_buffer;
  lvgl_drv_context.draw_buf = &lvgl_draw_context;
  lvgl_drv_context.full_refresh = 1;
  lv_disp_drv_register(&lvgl_drv_context);



  xTaskCreate(vTaskUpdateScreen,
              "TaskUpdateScreen",
              2048,
              NULL,
              2,
              &xHandleTaskUpdateScreen);

}

void loop() 
{
  int cnt = 0;
  xHandleTaskMain = xTaskGetCurrentTaskHandle();
  lv_obj_t* label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  while(true)
  {
    // notify that we are ready to draw on screen
    xTaskNotify(xHandleTaskUpdateScreen, NOTIFY_TASK_UPDATE_SCREEN_READY_TO_DRAW, eSetBits);
    // wait for the update to start
    uint32_t notified_value_update_screen = 0;
    // Use the mask because the screen update can be started and completed BEFORE the main task resume
    xTaskNotifyWait(0, MASK_NOTIFY_MAIN_SCREEN_UPDATE, &notified_value_update_screen, portMAX_DELAY);
    // we can start to draw the next frame
    // Always use the current backbuffer
    lv_disp_draw_buf_init(&lvgl_draw_context, screen_backbuffer, NULL, TFT_PX_COUNT);
    // invalidate the entire screen
    lv_obj_invalidate(lv_scr_act());

    // <==============>
    // HERE UI MODIFICATIONS
    lv_label_set_text_fmt(label, "cnt: %d", cnt++);
    // <==============>

    // Draws
    lv_refr_now(NULL);

    // Wait for a notification to wake up the task (when the framebuffer is ready to be drawn).
    // Check if the 'SCREEN UPDATE COMPLETED' flag was already set. If not, wait.
    if((notified_value_update_screen & NOTIFY_TASK_MAIN_SCREEN_UPDATE_COMPLETED) == 0)
    {
      xTaskNotifyWait(0, NOTIFY_TASK_MAIN_SCREEN_UPDATE_COMPLETED, NULL, portMAX_DELAY);
    }
  }
}