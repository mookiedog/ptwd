#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pll.h"

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

// Use GPIO21/GPOUT0 to drive CLKOUT. This GPIO/GPOUTx combo works for both RP2040 and RP2350.
const uint32_t clkout_gpio          = 21;       // Pico board pin 27

// For simplicity, we will power the onewire sensor directly from a GPIO pin.
// This is not recommended in real life!
// The GPIOs are chosen so that they use 3 pins side-by-side on the Pico boards.
const uint32_t onewire_power_gpio   = 15;       // The temp sensor power is pico board pin 19
                                                // The temp sensor ground is pico board pin 18
const uint32_t onewire_bus_gpio     = 14;       // The temp sensor Onewire bus is pico board pin 17
const uint32_t trigger_gpio         = 0;
const uint32_t button_gpio          = 9;

#define ILLEGAL_TEMP -300.0f            // below absolute zero (-273.15C)

typedef struct {
    uint64_t onewire_address;
    uint16_t rawtemp;
    float curr_temp_C;
    float prev_temp_C;
} sensor_info_t;

// We will look for a maximum of this many sensors when we scan the onewire bus
#define MAX_SENSOR_COUNT 20

sensor_info_t sensors[MAX_SENSOR_COUNT];

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
void init_pio()
{
    // Add the onewire program to the PIO shared address space
    if (!pio_can_add_program (pio, &onewire_program)) {
        _panic("Unable to add PIO program");
    }
    pio_offset = pio_add_program (pio, &onewire_program);

    // claim a state machine and initialize a driver instance
    if (!onewire.init(pio, pio_offset, onewire_bus_gpio)) {
        _panic("Onewire init() failed");
    }
}

// --------------------------------------------------------------------------------------------
void sensorPower(bool powerOn)
{
    if (powerOn) {
        // Power the onewire device by driving the GPIO connected to its power pin to '1'.
        // GPIO default is 4 mA drive strength.
        // This is not a recommended method of powering Onewire sensors, but it will work OK
        // for the purposes of this demo to power up to 2 sensors.
        gpio_init(onewire_power_gpio);
        gpio_set_dir(onewire_power_gpio, true);
        gpio_put(onewire_power_gpio, 1);

        // Enable a weak pullup on the onewire GPIO.
        // Again, a real solution would use a 3.3K to 10K resistor as a pullup
        // to work better in the cases of multiple sensors on the bus, or long wire runs.
        // Mostly, this serves to guarantee proper detection of the situation where there are
        // no devices on the bus.
        // We do not need to set the GPIO direction for this pin because the PIO unit controls it.
        gpio_set_pulls(onewire_bus_gpio, true, false);
    }
    else {
        // Turn off power to the sensors by driving the GPIO low
        gpio_init(onewire_power_gpio);
        gpio_set_dir(onewire_power_gpio, true);
        gpio_put(onewire_power_gpio, 0);

        // Turn on a weak pulldown so that we do not backpower the device through its data pin
        gpio_set_pulls(onewire_bus_gpio, false, true);
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

    init_pio();

    sensorPower(false);
    vTaskDelay(pdMS_TO_TICKS(10));
    sensorPower(true);


    // Set up a scope trigger output. The line will be driven high during the temperature conversion cycle.
    gpio_init(trigger_gpio);
    gpio_set_dir(trigger_gpio, true);
    gpio_put(trigger_gpio, 0);


    // Find each device on the onewire bus, adding its device address to the deviceAddr array.
    uint32_t t0_us = time_us_32();
    actual_sensor_count = onewire.romsearch(deviceAddr, MAX_SENSOR_COUNT, OW_SEARCH_ROM);
    uint32_t elapsed_us = time_us_32() - t0_us;
    printf("%s: Detected %d Onewire devices in %d uSec\n", __FUNCTION__, actual_sensor_count, elapsed_us);

    for (uint32_t i=0; i<MAX_SENSOR_COUNT; i++) {
        // Flush any previous temperature history by setting prev to an invalid value
        sensors[i].prev_temp_C = ILLEGAL_TEMP;
    }

    lastWakeTime = xTaskGetTickCount();

    while (1) {
        if (actual_sensor_count > 0) {
            // Start a temperature conversion in parallel on all devices on the bus
            //printf("%s: Starting temperature conversion\n", __FUNCTION__);
            onewire.reset();
            onewire.send(OW_SKIP_ROM);
            onewire.send(DS18B20_CONVERT_T);

            gpio_put(trigger_gpio, 1);
            // Wait for the conversions to finish
            // Note: some knock-off DS18B220's malfunction if you try to talk to them too soon after a start conversion command.
            // The work-around is to delay before initially asking them if they are done yet.
            // The entire conversion time is approx 750 mSec, so there is no harm in delaying before asking the first time.
            do {
                vTaskDelay(pdMS_TO_TICKS(50));
            } while (onewire.read() == 0);
            gpio_put(trigger_gpio, 0);

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
            for (uint32_t i=0; i<actual_sensor_count; i++) {
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

    // Blink the board's built-in LED forever, 10% duty cycle at 1Hz
    while (1) {
        //printf("%s[%d]\n", __FUNCTION__, get_core_num());
        pico_set_led(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        pico_set_led(false);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
}

// --------------------------------------------------------------------------------------------
// Add an idle task where we sleep, unless the pushbutton is pressed.
// Measuring the VSYS supply current with button pressed or not will indicated power savings.
void vApplicationIdleHook( void )
{
    static bool inited = false;

    if (!inited) {
        // Pullup on button input will default button input to '1' if not pressed
        gpio_init(button_gpio);
        gpio_set_dir(button_gpio, false);
        gpio_set_pulls(button_gpio, true, false);
    }

    if (gpio_get(button_gpio)) {
        // Button is not pressed: sleep until next interrupt
        __wfi();
    }
}

// --------------------------------------------------------------------------------------------
// Some approximate average VSYS current consumption numbers while running this demo:
// Pico  200 MHz    33 mA
// Pico  125 Mhz    23 mA
// Pico2 150 Mhz    16 mA   (sysPLL and usbPLL running)
// Pico2  48 MHz     7 mA   (no sysPLL; usbPLL is source of 48 MHz clock)
// Pico2  12 MHz     3 mA   (no sysPLL or usbPLL; XOSC is source of 12 MHz clock)
//
// The Pico2 is far more power efficient probably because it generates its 1.1V core voltage
// via a switch-mode voltage regulator instead of the RP2040's linear regulator.
void clockConfig()
{
    // Enable one of these blocks to change the processor clock speed from its default.
    // The point of running at 12 MHz or 48 MHz is to reduce current consumption by shutting
    // down one or more of the PLL circuits.
    // Merely changing the PLL frequency does not necessarily change power (current x time) consumption.

    #if 0
    // To lower the current consumption, configure the sysclk and peripheral clock to
    // run off the 12MHz xtal, with PLL disabled.
    clock_configure(
        clk_sys,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
    	CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
        12 * MHZ,
        12 * MHZ
    );

    // Turning off the sysclk PLL saves 0.9 mA on a Pico2
    pll_deinit(pll_sys);

    // Turning off the USB PLL saves 0.9 mA on a Pico2
    pll_deinit(pll_usb);

    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        12 * MHZ,
        12 * MHZ);
    #endif

    #if 0
    // Change clk_sys to be 48MHz by setting clksrc to use the PLL_USB
    // which always runs at 48MHz
    clock_configure(clk_sys,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        48 * MHZ,
        48 * MHZ);

    // Shut down the sys PLL, but leave the USB PLL running because that's where our 48 MHz clock is coming from
    pll_deinit(pll_sys);

    clock_configure(clk_peri,
            0,
            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
            48 * MHZ,
            48 * MHZ);
    #endif
}

// --------------------------------------------------------------------------------------------
void clockDriveOut()
{
    // Drive our XOSC/12 to the outside world via gpio clkout_gpio.
    // It's a 12 MHz crystal, so it should be a 1MHz squarewave at the GPIO.
    clock_gpio_init(clkout_gpio, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_XOSC_CLKSRC, 12);
}

// --------------------------------------------------------------------------------------------
int main()
 {
    hello();

    // Configuring the system clock to run at a different rate is completely optional.
    // It can reduce current consumption, if that is a concern for a particular project.
    clockConfig();
    clockDriveOut();

    stdio_init_all();

    // Demonstrate that we have printf sends output to the debugger terminal window
    printf("\"Hello, World!\" via RTT running on core %d\n", get_core_num());

    // Just for fun, display what freq we are running at:
    uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    printf("System clock: %.1f MHz\n", f_clk_sys / 1000.0);

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