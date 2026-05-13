#include "platform.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "uart.h"
#include "leds.h"
#include "timer.h"
#include "gpio.h"

// =========================================================================
// SYSTEM DEFINITIONS & STATES
// =========================================================================
#define BUFF_SIZE 20

typedef enum {
    STATE_IDLE,          // Waiting for profile input
    STATE_EXECUTING,     // Executing profile (LED blinking)
    STATE_ESTOP,         // Emergency Stop (Locked)
    STATE_AWAIT_UNLOCK   // Waiting for "UNLOCK" password
} SystemState;

volatile SystemState currentState = STATE_IDLE;

// =========================================================================
// UART VARIABLES
// =========================================================================
volatile char rx_buffer[BUFF_SIZE];
volatile uint8_t rx_index = 0;
volatile bool profile_ready = false;

// =========================================================================
// PROFILE EXECUTION VARIABLES
// =========================================================================
char current_profile[BUFF_SIZE];
volatile uint8_t profile_length = 0;
volatile uint8_t current_digit_index = 0;
volatile bool loop_profile = false;
volatile bool next_digit_flag = false; // Raised by the 2-second timer

// =========================================================================
// SOFTWARE TIMERS VARIABLES (Driven by 1ms SysTick)
// =========================================================================
volatile uint32_t timeout_counter = 0;
volatile uint32_t timeout_limit = 0;
volatile bool timeout_active = false;

volatile uint32_t digit_counter = 0;
volatile bool digit_active = false;

volatile uint32_t led_counter = 0;
volatile uint32_t led_toggle_limit = 0; 
volatile bool led_blinking = false;
volatile int current_led_state = 0;


// =========================================================================
// 1. LED CONTROL FUNCTIONS
// =========================================================================

void turn_led_on_solid(void) {
    led_blinking = false;
    leds_set(1, 0, 0);     // Turn on RED LED for E-Stop
}

void turn_led_off(void) {
    led_blinking = false;
    leds_set(0, 0, 0);     // Turn off all LEDs
}

void set_led_frequency(uint8_t hz) {
    if (hz == 0) {
        turn_led_off();
    } else {
        // 1 Hz = 1 full cycle per second (1000ms). Toggle every 1000 / (Hz * 2) ms.
        led_toggle_limit = 1000 / (hz * 2);
        led_counter = 0;
        led_blinking = true;
    }
}


// =========================================================================
// 2. TIMER CONTROL FUNCTIONS
// =========================================================================

void start_timeout_timer(uint8_t seconds) {
    timeout_limit = seconds * 1000; // Convert to ms
    timeout_counter = 0;
    timeout_active = true;
}

void stop_timeout_timer(void) {
    timeout_active = false;
}

void reset_timeout_timer(void) {
    timeout_counter = 0;
}

void start_digit_timer(void) {
    digit_counter = 0;
    digit_active = true;
}

void stop_digit_timer(void) {
    digit_active = false;
}


// =========================================================================
// 3. INTERRUPT SERVICE ROUTINES (ISRs)
// =========================================================================

// --- Logic for Timeout ---
void timeout_timer_isr(void) {
    stop_timeout_timer();
    
    if (currentState == STATE_AWAIT_UNLOCK) {
        // Failed to provide UNLOCK in time
        currentState = STATE_ESTOP;
        uart_print("\r\n[ERROR] Unlock timeout. System remains locked.\r\n");
    } else {
        // Input typing timeout (4 seconds)
        rx_index = 0; 
        uart_print("\r\n[TIMEOUT] Input sequence cancelled.\r\n");
        if (currentState != STATE_EXECUTING) {
            currentState = STATE_IDLE;
            uart_print("Enter profile: ");
        }
    }
}

// --- Logic for 2-Second Digit Switch ---
void digit_timer_isr(void) {
    next_digit_flag = true; 
}

// --- MASTER SYSTICK CALLBACK (Executes every 1 ms) ---
void my_systick_callback(void) {
    // A. Check Timeout (4s or 5s)
    if (timeout_active) {
        timeout_counter++;
        if (timeout_counter >= timeout_limit) {
            timeout_active = false;
            timeout_timer_isr(); 
        }
    }
    
    // B. Check Digit Execution (Every 2 seconds = 2000 ms)
    if (digit_active) {
        digit_counter++;
        if (digit_counter >= 2000) {
            digit_counter = 0; 
            digit_timer_isr(); 
        }
    }
    
    // C. Check LED Blinking (Frequency/PWM)
    if (led_blinking) {
        led_counter++;
        if (led_counter >= led_toggle_limit) {
            led_counter = 0;
            current_led_state = !current_led_state; // Toggle
            leds_set(current_led_state, 0, 0);
        }
    }
}

// --- EXTI Button Interrupt Handler (Highest Priority) ---
void button_exti_isr() {
    
    if (currentState != STATE_ESTOP && currentState != STATE_AWAIT_UNLOCK) {
        // 1st Press: Enter E-STOP mode
        currentState = STATE_ESTOP;
        stop_digit_timer();
        stop_timeout_timer();
        turn_led_on_solid(); 
        uart_print("\r\n[SYSTEM HALTED] E-STOP ACTIVATED!\r\n");
        rx_index = 0; // Clear buffer
    } 
    else if (currentState == STATE_ESTOP) {
        // 2nd Press: Request Unlock
        currentState = STATE_AWAIT_UNLOCK;
        uart_print("\r\nOverride requested. Awaiting password...\r\n");
        rx_index = 0;
        start_timeout_timer(5); // 5 seconds to type UNLOCK
    }
}

// --- UART RX Interrupt Handler ---
void uart_rx_isr(uint8_t rx) {
    char received_char = (char)rx;

    // Check E-STOP lock condition
    if (currentState == STATE_ESTOP) {
        uart_print("[ERROR] SYSTEM LOCKED\r\n");
        return;
    }

    // Reset inactivity timer on any keystroke
    reset_timeout_timer();

    if (received_char == '\r' || received_char == '\n') { 
        if (rx_index > 0) {
            rx_buffer[rx_index] = '\0'; // Null-terminate string
            profile_ready = true;       // Notify main()
            rx_index = 0;               // Reset for next input
            stop_timeout_timer();       // Stop timeout timer since Enter was pressed
        }
    }
		else if (received_char == '\b' || received_char == 0x7F) {
        if (rx_index > 0) {
            rx_index--; // Delete last character from buffer
            uart_print("\b \b"); // Erase character visually on terminal
        }
    }
		else {
        if (rx_index == 0) {
            // Start 4-second timeout timer on first character
            start_timeout_timer(4); 
        }
        
        if (rx_index < BUFF_SIZE - 1) {
            rx_buffer[rx_index++] = received_char;
            uart_tx(rx); // Echo character back to terminal
        }
    }
}


// =========================================================================
// 4. MAIN & STATE MACHINE
// =========================================================================

void execute_current_digit(void) {
    char current_char = current_profile[current_digit_index];
    
    if (current_char >= '1' && current_char <= '9') {
        uint8_t hz = current_char - '0'; // Convert char to integer
        set_led_frequency(hz);
    } else {
        set_led_frequency(0); // LED off
    }
}

int main() {
    // --- 1. HARDWARE INIT ---
    
    // Init UART
    uart_init(9600);
    uart_set_rx_callback(uart_rx_isr);
    uart_enable();
    
    // Init LEDs
    leds_init();
    turn_led_off();
    
    // Init Button (E-Stop) on P_SW (PC_13)
    gpio_set_mode(P_SW, Input); 
    gpio_set_trigger(P_SW, Rising); 
    gpio_set_callback(P_SW, button_exti_isr); 
    
    // Init Master Timer (SysTick) to 1 ms (1000 us)
    timer_init(1000); 
    timer_set_callback(my_systick_callback);
    timer_enable();

    // --- 2. NVIC PRIORITIES CONFIGURATION ---
    // Lower number means higher priority.
    
    // EXTI Button (Highest Priority)
    NVIC_SetPriority(EXTI15_10_IRQn, 0); 
    
    // Timers / SysTick (Medium Priority)
    NVIC_SetPriority(SysTick_IRQn, 1);
    
    // UART (Lowest Priority)
    NVIC_SetPriority(USART2_IRQn, 2); 

    __enable_irq(); // Enable global interrupts
    
    uart_print("\r\nSystem Initialized. Enter profile: ");

    // --- 3. MAIN LOOP (State Machine) ---
    while(1) {
        switch (currentState) {
            
            case STATE_IDLE:
                if (profile_ready) {
                    profile_ready = false;
                    
                    // Copy profile from rx_buffer
                    strcpy(current_profile, (char*)rx_buffer);
                    profile_length = strlen(current_profile);
                    
                    // Check for Loop modifier (-)
                    if (current_profile[profile_length - 1] == '-') {
                        loop_profile = true;
                        profile_length--; // Ignore the dash during execution
                    } else {
                        loop_profile = false;
                    }
                    
                    current_digit_index = 0;
                    currentState = STATE_EXECUTING;
                    
                    uart_print("\r\nStarting profile execution...\r\n");
                    execute_current_digit();
                    start_digit_timer(); // Start 2-second switching
                }
                break;
                
            case STATE_EXECUTING:
                // Check if a NEW profile was submitted (Interrupt current profile)
                if (profile_ready) {
                    profile_ready = false;
                    stop_digit_timer();
                    set_led_frequency(0);
                    currentState = STATE_IDLE; 
                    profile_ready = true; // Keep flag high to be processed in STATE_IDLE
                    break;
                }
                
                // Switch digit driven by the 2-second timer flag
                if (next_digit_flag) {
                    next_digit_flag = false;
                    current_digit_index++;
                    
                    if (current_digit_index >= profile_length) {
                        if (loop_profile) {
                            current_digit_index = 0; // Repeat from start
                            execute_current_digit();
                        } else {
                            // Finish execution
                            stop_digit_timer();
                            set_led_frequency(0);
                            currentState = STATE_IDLE;
                            uart_print("\r\nProfile finished. Enter new profile: ");
                        }
                    } else {
                        execute_current_digit();
                    }
                }
                break;
                
            case STATE_ESTOP:
                // System halted. Waiting for button interrupt.
                break;
                
            case STATE_AWAIT_UNLOCK:
                // Waiting for "UNLOCK" password via UART
                if (profile_ready) {
                    profile_ready = false;
                    stop_timeout_timer();
                    
                    if (strcmp((char*)rx_buffer, "UNLOCK") == 0) {
                        currentState = STATE_IDLE;
                        turn_led_off();
                        uart_print("\r\nSystem Unlocked. Enter profile: ");
                    } else {
                        currentState = STATE_ESTOP;
                        uart_print("\r\n[ERROR] Invalid password. System locked.\r\n");
                    }
                }
                break;
        }
        
        // Non-blocking sleep. Microcontroller wakes up on any interrupt.
        __WFI(); 
    }
}