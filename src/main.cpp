#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <math.h>

#include "Onewire.h"
#include "ds18b20.h"
#include "ow_rom.h"

#include "FreeRTOS.h"
#include "task.h"

#ifdef CYW43_WL_GPIO_LED_PIN
    #include "pico/cyw43_arch.h"
#endif

Onewire onewire;

// We will be using a PIO object to perform the hardware Onewire signaling
PIO pio = pio0;
uint32_t pio_offset;

// For simplicity, we will power the onewire sensor directly from a GPIO pin.
// This is not recommended in real life!
// The GPIOs are chosen so that they use 3 pins side-by-side on the Pico boards.
uint32_t onewire_power_gpio = 14;       // The temp sensor power is pico board pin 19
                                        // The temp sensor ground is pico board pin 18
uint32_t onewire_bus_gpio   = 13;       // The temp sensor Onewire bus is pico board pin 17

#define ILLEGAL_TEMP -273.0f

typedef struct {
    uint64_t onewire_address;
    uint16_t rawtemp;
    float curr_temp_C;
    float prev_temp_C;
} sensor_info_t;

// We will look for a maximum of this many sensors when we scan the onewire bus
#define MAX_SENSOR_COUNT 20

sensor_info_t sensors[MAX_SENSOR_COUNT];

// Our system only expects this many sensors though:
#define EXPECTED_SENSOR_COUNT 1

int actual_sensor_count;

// --------------------------------------------------------------------------------------------
void _panic(const char* msg)
{
    printf("%s: %s\n", __FUNCTION__, msg);
    __breakpoint();
}

// --------------------------------------------------------------------------------------------
// The on-board LED is treated differently between the plain pico boards and their wireless variants.
// These functions hides the differences.
int32_t pico_led_init(void)
{
    #if defined(PICO_DEFAULT_LED_PIN)
        // non-wireless boards access the LED GPIO directly
        gpio_init(PICO_DEFAULT_LED_PIN);
        gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
        return PICO_OK;
    #elif defined(CYW43_WL_GPIO_LED_PIN)
        // Wireless boards access the LED through the wireless driver
        return cyw43_arch_init();
    #endif
}

void pico_set_led(bool led_on)
{
    #if defined(PICO_DEFAULT_LED_PIN)
        gpio_put(PICO_DEFAULT_LED_PIN, led_on);
    #elif defined(CYW43_WL_GPIO_LED_PIN)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
    #endif
}

// --------------------------------------------------------------------------------------------
// Fast flash the LED 3 times as a most basic sign of life as we boot.
void hello()
{
    int32_t rval = pico_led_init();
    hard_assert(rval == PICO_OK);

    for (uint32_t i=0; i<3; i++) {
        pico_set_led(true);
        sleep_ms(10);
        pico_set_led(false);
        sleep_ms(50);
    }
}


// --------------------------------------------------------------------------------------------
// All temp sensors get read once per second.
// New temperatures only get displayed if they have changed since the last time they were measured.
void vTempSensorTask(void* arg)
{
    uint64_t deviceAddr[MAX_SENSOR_COUNT];
    static TickType_t lastWakeTime;
    static uint64_t unexpected_address;

    printf("Hello from the TempSensorTask, running on core %d\n", get_core_num());

    // Add the onewire program to the PIO shared address space
    if (!pio_can_add_program (pio, &onewire_program)) {
        _panic("Unable to add PIO program");
    }
    pio_offset = pio_add_program (pio, &onewire_program);

    // claim a state machine and initialize a driver instance
    if (!onewire.init(pio, pio_offset, onewire_bus_gpio)) {
        _panic("Onewire init() failed");
    }

    // Power the onewire device by driving the GPIO connected to its power pin to '1'.
    // This is not a recommended method of powering Onewire sensors, but it will work OK
    // for the purposes of this demo.
    gpio_init(onewire_power_gpio);
    gpio_set_dir(onewire_power_gpio, true);
    gpio_put(onewire_power_gpio, 1);

    // Enable a weak pullup on the onewire GPIO.
    // Again, a real solution would use a 3.3K to 10K resistor as a pullup
    // to work better in the cases of multiple sensors on the bus, or long wire runs.
    // Mostly, this serves to guarantee proper detection of the situation where there are
    // no devices on the bus.
    gpio_set_pulls(onewire_bus_gpio, true, false);


    // Find each device on the onewire bus, adding its device address to the deviceAddr array.
    uint32_t t0_us = time_us_32();
    actual_sensor_count = onewire.romsearch(deviceAddr, MAX_SENSOR_COUNT, OW_SEARCH_ROM);
    volatile uint32_t elapsed_us = time_us_32() - t0_us;
    printf("%s: Detected %d Onewire devices in %d uSec\n", __FUNCTION__, actual_sensor_count, elapsed_us);

    for (uint32_t i=0; i<EXPECTED_SENSOR_COUNT; i++) {
        // Flush any previous temperature history by setting prev to an invalid value
        sensors[i].prev_temp_C = ILLEGAL_TEMP;
    }

    if (actual_sensor_count != EXPECTED_SENSOR_COUNT) {
        printf("%s: Sensors count mismatch! Expected: %d, saw: %d\n", __FUNCTION__, EXPECTED_SENSOR_COUNT, actual_sensor_count);
    }

    lastWakeTime = xTaskGetTickCount();

    while (1) {
        if (actual_sensor_count > 0) {
            // Start a temperature conversion in parallel on all devices on the bus
            //printf("%s: Starting temperature conversion\n", __FUNCTION__);
            onewire.reset();
            onewire.send(OW_SKIP_ROM);
            onewire.send(DS18B20_CONVERT_T);

            // Wait for the conversions to finish
            while (onewire.read() == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Gather temperature readings from all the devices we know about
            for (int i = 0; i < actual_sensor_count; i += 1) {
                // Make sure the Onewire bus is in a known state
                onewire.reset();

                // Send out the address of the sensor we want to talk to
                onewire.send(OW_MATCH_ROM);
                for (int b = 0; b < 64; b += 8) {
                    onewire.send(deviceAddr[i] >> b);
                }

                // Read its most recent temperature conversion result
                onewire.send(DS18B20_READ_SCRATCHPAD);
                sensors[i].rawtemp = onewire.read () | (onewire.read() << 8);

                // Convert the internal temperature format to degrees C
                float t_C = (float)sensors[i].rawtemp / 16.0f;
                sensors[i].curr_temp_C = t_C;
            }

            // Only update the temperatures that have changed since they were last displayed
            bool missing_sensors = false;
            for (uint32_t i=0; i<EXPECTED_SENSOR_COUNT; i++) {
                if (sensors[i].curr_temp_C != sensors[i].prev_temp_C) {
                    // The temperature has changed: we need to display it
                    printf("%s[%d]: Sensor %d temp is %.1fC [%.1fF], raw:%04X\n",
                           __FUNCTION__,
                           get_core_num(),
                           i,
                           sensors[i].curr_temp_C,
                           ((sensors[i].curr_temp_C * 9.0f)/5.0f)+32.0f,
                           sensors[i].rawtemp
                    );
                    sensors[i].prev_temp_C = sensors[i].curr_temp_C;
                }
            }
        }

        // Delay 1 second from the last time we woke up so that our 1 second scan period
        // does not drift by the amount of time to do the sensor processing
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1000));
    }
}


// --------------------------------------------------------------------------------------------
// The RTOS is running when we get here.
// We can use any FreeRTOS mechanisms that we want to.
void bootSystem()
{
    BaseType_t err;

    // Start the rest of the tasks we want to get going:

    err = xTaskCreate(vTempSensorTask, "TempSensors", 1024, NULL, 1, NULL);
    if (err != pdPASS) {
        panic("TempSensor task creation failed!");
    }
}

// --------------------------------------------------------------------------------------------
void vBlink(void* arg)
{
    // The blink task must boot the rest of the tasks before doing its real job
    bootSystem();

    // Blink the board's built-in LED forever, 10% duty cycle
    while (1) {
        //printf("%s[%d]\n", __FUNCTION__, get_core_num());
        pico_set_led(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        pico_set_led(false);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
}

// --------------------------------------------------------------------------------------------
int main()
{
    hello();

    stdio_init_all();

    // Demonstrate that we have printf sends output to the debugger terminal window
    printf("\"Hello, World!\" via RTT running on core %d\n", get_core_num());

    // Create the first task: one of its responsibilities will be to start the rest of the tasks.
    // No FreeRTOS mechanisms will work until the scheduler runs, so the FreeRTOS world should not
    // get booted until FreeRTOS is running.
    BaseType_t err = xTaskCreate(vBlink, "Blink", 512, NULL, 1, NULL);
    if (err != pdPASS) {
        panic("Blink task creation failed!");
    }

    // Hand control to FreeRTOS
    // It will immediately start running our Blink task, created just above.
    vTaskStartScheduler();

    // The scheduler never exits: we will never get here!
}