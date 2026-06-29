#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"

// --- HARDWARE PIN ASSIGNMENTS ---
#define SERVO_PIN        7
#define SERVO_WRAP       39062  

#define DISP_RED_PIN    10
#define DISP_GREEN_PIN  11
#define DISP_BLUE_PIN   12

#define SENS_BLUE_PIN   13
#define SENS_GREEN_PIN  14
#define SENS_RED_PIN    15

#define LDR_ADC_PIN     28  // GPIO 28 maps to ADC2
#define LDR_ADC_CH      2   // ADC Channel 2

#define PWM_MAX         65535
#define ADC_CONV        (3.3f / 4095.0f)

// --- CONFIGURABLE SELECTION THRESHOLDS ---
#define MATCH_THRESHOLD       0.25f  
#define BELT_DIRT_THRESHOLD   0.18f  // Noise gate: items must differ from NONE by at least this voltage to count

// --- PRE-MADE COLOR TARGET TABLE ---
typedef struct {
    const char* label;
    uint8_t r_disp;
    uint8_t g_disp;
    uint8_t b_disp;
} PreMadeTarget;

#define TARGET_COUNT 7
const PreMadeTarget color_table[TARGET_COUNT] = {
    {"RED",    100, 0,   0  },
    {"GREEN",  0,   100, 0  },
    {"BLUE",   0,   0,   100},
    {"YELLOW", 60,  100, 0  },
    {"ORANGE", 100, 30,  0  },
    {"PINK",   100, 0,   60 },
    {"NONE",   0,   0,   0  }  // Index 6: Background belt profile
};

typedef struct {
    char name[20];
    float r_val;
    float g_val;
    float b_val;
    uint16_t servo_angle;
    uint8_t disp_r;
    uint8_t disp_g;
    uint8_t disp_b;
    bool active;
} ColorProfile;

ColorProfile trained_profiles[TARGET_COUNT];
int active_profile_count = 0;

uint32_t decay_cooldown_r = 100; 
uint32_t decay_cooldown_g = 100;
uint32_t decay_cooldown_b = 100;
bool latency_calibrated = false; 

// Asynchronous Timing Control Registers
absolute_time_t servo_unlock_time;
bool is_servo_locked = false;
uint16_t current_active_angle = 90;

// Idle Blink Timer State Tracking
absolute_time_t next_idle_blink_time;
bool idle_blink_state = false;

void clear_serial_buffer() {
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
}

void turn_off_sensor_rgb() {
    gpio_put(SENS_RED_PIN, 0);
    gpio_put(SENS_GREEN_PIN, 0);
    gpio_put(SENS_BLUE_PIN, 0);
}

void set_display_rgb(uint8_t r, uint8_t g, uint8_t b) {
    pwm_set_gpio_level(DISP_RED_PIN,   (r * r * PWM_MAX) / (100 * 100));
    pwm_set_gpio_level(DISP_GREEN_PIN, (g * g * PWM_MAX) / (100 * 100));
    pwm_set_gpio_level(DISP_BLUE_PIN,  (b * b * PWM_MAX) / (100 * 100));
}

void write_servo_angle(uint16_t angle) {
    if (angle > 180) angle = 180;
    uint32_t level = 1172 + ((angle * (4687 - 1172)) / 180);
    pwm_set_gpio_level(SERVO_PIN, level);
}

float sample_raw_voltage_averages(int iterations) {
    uint32_t total = 0;
    for (int i = 0; i < iterations; i++) {
        total += adc_read();
        sleep_us(30); // Shorter sampling separation
    }
    return ((float)total / (float)iterations) * ADC_CONV;
}

void capture_raw_rgb(float *r_out, float *g_out, float *b_out) {
    turn_off_sensor_rgb();
    sleep_ms(30); // Accelerated settling window

    gpio_put(SENS_RED_PIN, 1);   sleep_ms(65); // High-speed shutter pulse
    *r_out = sample_raw_voltage_averages(20);
    gpio_put(SENS_RED_PIN, 0);   sleep_ms(decay_cooldown_r); 

    gpio_put(SENS_GREEN_PIN, 1); sleep_ms(65);
    *g_out = sample_raw_voltage_averages(20);
    gpio_put(SENS_GREEN_PIN, 0); sleep_ms(decay_cooldown_g); 

    gpio_put(SENS_BLUE_PIN, 1);  sleep_ms(65);
    *b_out = sample_raw_voltage_averages(20);
    gpio_put(SENS_BLUE_PIN, 0);  sleep_ms(decay_cooldown_b); 
}

// Non-blocking white status blink function used during scanning loops
void update_idle_blink_status() {
    if (absolute_time_diff_us(get_absolute_time(), next_idle_blink_time) < 0) {
        idle_blink_state = !idle_blink_state;
        next_idle_blink_time = make_timeout_time_ms(120); // Fast 120ms cycle cadence
    }
    
    if (idle_blink_state) {
        set_display_rgb(15, 15, 15); // Subtle white heartbeat pulse
    } else {
        set_display_rgb(0, 0, 0);
    }
}

void run_ldr_latency_calibration() {
    printf("\n======================================================\n");
    printf("    LDR DECAY HORIZON AUTO-TUNING UTILITY             \n");
    printf("======================================================\n");
    printf("Place your brightest object (ideally WHITE) over the sensor.\n");
    printf("Press [ANY KEY] to measure channel transitions...\n");
    clear_serial_buffer(); getchar();

    turn_off_sensor_rgb();
    sleep_ms(500); 
    float v_ambient = sample_raw_voltage_averages(35);

    uint32_t calibrated_latencies[3] = {0, 0, 0};
    uint target_pins[3] = {SENS_RED_PIN, SENS_GREEN_PIN, SENS_BLUE_PIN};
    const char* color_names[3] = {"Red", "Green", "Blue"};

    for (int c = 0; c < 3; c++) {
        uint target_pin = target_pins[c];
        printf(" -> Evaluating %s channel decay envelope... ", color_names[c]);
        
        gpio_put(target_pin, 1);
        sleep_ms(250); 
        float v_lit = sample_raw_voltage_averages(35);
        float peak_delta = (v_lit > v_ambient) ? (v_lit - v_ambient) : 0.05f;

        float target_decay_threshold = v_ambient + (peak_delta * 0.20f);

        gpio_put(target_pin, 0);
        uint32_t decay_start = to_ms_since_boot(get_absolute_time());
        uint32_t elapsed_ms = 0;
        
        while (elapsed_ms < 2000) { 
            float current_v = sample_raw_voltage_averages(5);
            if (current_v <= target_decay_threshold) break;
            sleep_ms(1);
            elapsed_ms = to_ms_since_boot(get_absolute_time()) - decay_start;
        }
        
        calibrated_latencies[c] = (elapsed_ms > 20) ? elapsed_ms : 20;
        printf("Cleared in %lu ms\n", calibrated_latencies[c]);
        sleep_ms(200); 
    }

    decay_cooldown_r = calibrated_latencies[0];
    decay_cooldown_g = calibrated_latencies[1];
    decay_cooldown_b = calibrated_latencies[2];
    latency_calibrated = true; 

    printf("\n>>> DELAY PARAMS SECURED AND INJECTED INTO SAMPLING LOOP <<<\n");
    printf("  * Cooldowns -> Red: %lu ms | Green: %lu ms | Blue: %lu ms\n", decay_cooldown_r, decay_cooldown_g, decay_cooldown_b);
}

void train_new_color_slot() {
    clear_serial_buffer();

    printf("\n======================================================\n");
    printf("    MENU: SELECT PRE-MADE COLOR SLOT TO CALIBRATE     \n");
    printf("======================================================\n");
    for (int i = 0; i < TARGET_COUNT; i++) {
        printf("[%d] %-10s (Display Driver mapping: R:%3d G:%3d B:%3d)\n", 
               i, color_table[i].label, color_table[i].r_disp, color_table[i].g_disp, color_table[i].b_disp);
    }
    printf("Select choice index token (0-%d): ", TARGET_COUNT - 1);
    
    int chosen_idx = -1;
    while (true) {
        int ch = getchar();
        if (ch >= '0' && ch < '0' + TARGET_COUNT) {
            putchar(ch);
            chosen_idx = ch - '0';
            break;
        }
    }
    printf("\nTarget Slot Selected: [%s]\n\n", color_table[chosen_idx].label);

    printf("Enter target lock servo angle output value (0 to 180):\n>> ");
    clear_serial_buffer();
    uint32_t parsed_angle = 90;
    char num_buff[10]; int num_idx = 0;
    while (num_idx < 9) {
        int ch = getchar();
        if (ch == '\r' || ch == '\n') { if (num_idx > 0) break; continue; }
        if (ch >= '0' && ch <= '9') { putchar(ch); num_buff[num_idx++] = (char)ch; }
    }
    num_buff[num_idx] = '\0';
    sscanf(num_buff, "%lu", &parsed_angle);
    if (parsed_angle > 180) parsed_angle = 180;

    printf("\nPlace your [%s] surface directly over the sensor assembly now.\n", color_table[chosen_idx].label);
    printf("Press [ANY KEY] to flash sensor light profiles and store values...\n");
    clear_serial_buffer(); getchar();

    float r = 0, g = 0, b = 0;
    printf("Scanning color signature data... ");
    capture_raw_rgb(&r, &g, &b);
    
    strcpy(trained_profiles[chosen_idx].name, color_table[chosen_idx].label);
    trained_profiles[chosen_idx].r_val = r;
    trained_profiles[chosen_idx].g_val = g;
    trained_profiles[chosen_idx].b_val = b;
    trained_profiles[chosen_idx].servo_angle = (uint16_t)parsed_angle;
    trained_profiles[chosen_idx].disp_r = color_table[chosen_idx].r_disp;
    trained_profiles[chosen_idx].disp_g = color_table[chosen_idx].g_disp;
    trained_profiles[chosen_idx].disp_b = color_table[chosen_idx].b_disp;
    
    if (!trained_profiles[chosen_idx].active) {
        trained_profiles[chosen_idx].active = true;
        active_profile_count++;
    }

    printf("DONE!\nSignature stored: R:%.3fV | G:%.3fV | B:%.3fV -> Locked Angle: %d deg\n", r, g, b, parsed_angle);
}

void print_trained_database() {
    printf("\n--- CURRENT TRAINED DATABASE PROFILES ---\n");
    int active_printed = 0;
    for (int i = 0; i < TARGET_COUNT; i++) {
        if (trained_profiles[i].active) {
            printf(" Slot Index [%d] Name: %-10s | R:%.3fV G:%.3fV B:%.3fV | Lock Servo: %3d deg\n",
                   i, trained_profiles[i].name, trained_profiles[i].r_val, 
                   trained_profiles[i].g_val, trained_profiles[i].b_val, trained_profiles[i].servo_angle);
            active_printed++;
        }
    }
    if (active_printed == 0) printf("Empty matrix database. Please train custom colors using [t].\n");
}

void process_conveyor_sorting_pipeline() {
    if (active_profile_count == 0) {
        printf(">> [WARN] No active trained profiles. Press [t] to calibrate parameters first! <<\n");
        sleep_ms(1000);
        return;
    }

    // 1. Asynchronous check for physical gate release timeout
    if (is_servo_locked) {
        if (absolute_time_diff_us(get_absolute_time(), servo_unlock_time) < 0) {
            is_servo_locked = false; 
            printf("[TIMER EXPIRED] Sorting track cleared. Resuming active belt scanning...\n");
        }
    }

    // 2. High-speed scan pass
    float cur_r = 0, cur_g = 0, cur_b = 0;
    capture_raw_rgb(&cur_r, &cur_g, &cur_b);

    int closest_slot_match = -1;
    float minimum_calculated_distance = 999.0f;

    for (int i = 0; i < TARGET_COUNT; i++) {
        if (!trained_profiles[i].active) continue;

        float delta_r = cur_r - trained_profiles[i].r_val;
        float delta_g = cur_g - trained_profiles[i].g_val;
        float delta_b = cur_b - trained_profiles[i].b_val;
        float calculated_distance = sqrtf((delta_r * delta_r) + (delta_g * delta_g) + (delta_b * delta_b));

        if (calculated_distance < minimum_calculated_distance) {
            minimum_calculated_distance = calculated_distance;
            closest_slot_match = i;
        }
    }

    // 3. Noise gate logic to reject belt surface scratches and dust
    float dist_from_none = 999.0f;
    if (trained_profiles[6].active) { // Index 6 is our "NONE" baseline profile
        float d_r = cur_r - trained_profiles[6].r_val;
        float d_g = cur_g - trained_profiles[6].g_val;
        float d_b = cur_b - trained_profiles[6].b_val;
        dist_from_none = sqrtf((d_r * d_r) + (d_g * d_g) + (d_b * d_b));
    }

    // 4. State Assignment Parsing
    if (closest_slot_match != -1 && minimum_calculated_distance <= MATCH_THRESHOLD) {
        
        // Scenario A: Sensor registers a match to empty belt, OR reflection variation is too weak to be a real item
        if (closest_slot_match == 6 || dist_from_none < BELT_DIRT_THRESHOLD) { 
            if (!is_servo_locked) {
                current_active_angle = trained_profiles[6].active ? trained_profiles[6].servo_angle : 90;
                write_servo_angle(current_active_angle);
                update_idle_blink_status(); // Run the white heartbeat blink when idle
            }
            printf("CONVEYOR: [R:%.3fV G:%.3fV B:%.3fV] -> [EMPTY/DIRTY BELT FILTERED] (Servo: %d deg)\n", 
                   cur_r, cur_g, cur_b, current_active_angle);
        } 
        // Scenario B: Valid item cleanly verified outside background noise parameters
        else { 
            servo_unlock_time = make_timeout_time_ms(2800);
            is_servo_locked = true;
            current_active_angle = trained_profiles[closest_slot_match].servo_angle;
            
            write_servo_angle(current_active_angle);
            set_display_rgb(trained_profiles[closest_slot_match].disp_r, trained_profiles[closest_slot_match].disp_g, trained_profiles[closest_slot_match].disp_b);
            
            printf("CONVEYOR: [R:%.3fV G:%.3fV B:%.3fV] -> DETECTED VALID ITEM: [%s] -> LOCKING SERVO to %d deg (2.8s)\n",
                   cur_r, cur_g, cur_b, trained_profiles[closest_slot_match].name, current_active_angle);
        }
    } else {
        printf("CONVEYOR: [R:%.3fV G:%.3fV B:%.3fV] -> >> UNKNOWN OBJECT SIGNAL OUTSIDE PARAMETERS <<\n", cur_r, cur_g, cur_b);
        if (!is_servo_locked) update_idle_blink_status();
    }
}

void print_user_interface() {
    printf("\n======================================================\n");
    printf("    ANTI-DIRT RELIABLE HIGH-SPEED CONVEYOR SCANNER    \n");
    printf("======================================================\n");
    if (!latency_calibrated) printf(" [!] STATUS: DECAY CALIBRATION REQUIRED BEFORE USE\n");
    else                     printf(" [*] STATUS: FILTER ACTIVE / PILOT READY\n");
    printf("------------------------------------------------------\n");
    printf("[c] Run LDR Physical Latency Auto-Tuning Profile Pass\n");
    printf("[t] Select & Train a Pre-Made Color Matrix Slot (Train NONE on clean belt)\n");
    printf("[p] Print Current Trained Database Configuration\n");
    printf("[m] Trigger Single Manual Match Check Pass\n");
    printf("[a] Start Continuous Active Conveyor Sorting Stream (Blinks White while idle)\n");
    printf("------------------------------------------------------\n");
    printf("Select option token: ");
}

int main() {
    stdio_init_all();
    
    gpio_set_function(DISP_RED_PIN,   GPIO_FUNC_PWM);
    gpio_set_function(DISP_GREEN_PIN, GPIO_FUNC_PWM);
    gpio_set_function(DISP_BLUE_PIN,  GPIO_FUNC_PWM);
    gpio_set_function(SERVO_PIN,      GPIO_FUNC_PWM);
    
    gpio_init(SENS_RED_PIN);   gpio_set_dir(SENS_RED_PIN, GPIO_OUT);
    gpio_init(SENS_GREEN_PIN); gpio_set_dir(SENS_GREEN_PIN, GPIO_OUT);
    gpio_init(SENS_BLUE_PIN);  gpio_set_dir(SENS_BLUE_PIN, GPIO_OUT);
    
    pwm_config led_config = pwm_get_default_config();
    pwm_config_set_clkdiv(&led_config, 4.0f); 
    pwm_init(pwm_gpio_to_slice_num(DISP_RED_PIN),    &led_config, true);
    pwm_init(pwm_gpio_to_slice_num(DISP_GREEN_PIN),  &led_config, true);
    pwm_init(pwm_gpio_to_slice_num(DISP_BLUE_PIN),   &led_config, true);

    pwm_config servo_config = pwm_get_default_config();
    pwm_config_set_clkdiv(&servo_config, 64.0f); 
    pwm_config_set_wrap(&servo_config, SERVO_WRAP);
    pwm_init(pwm_gpio_to_slice_num(SERVO_PIN), &servo_config, true);

    set_display_rgb(0, 0, 0);
    turn_off_sensor_rgb();
    write_servo_angle(90); 

    for (int i = 0; i < TARGET_COUNT; i++) trained_profiles[i].active = false;
    next_idle_blink_time = get_absolute_time();

    adc_init();
    adc_gpio_init(LDR_ADC_PIN);    
    adc_select_input(LDR_ADC_CH);  

    sleep_ms(2000);
    print_user_interface();

    while (true) {
        int incoming_token = getchar_timeout_us(0);
        if (incoming_token == PICO_ERROR_TIMEOUT) { sleep_ms(10); continue; }

        switch (incoming_token) {
            case 'c': case 'C': run_ldr_latency_calibration(); print_user_interface(); break;
            case 't': case 'T': 
                if (!latency_calibrated) printf("\n[REJECTED] Run decay auto-tuning calibration [c] first!\n");
                else                     train_new_color_slot();
                print_user_interface(); break;
            case 'p': case 'P': print_trained_database(); print_user_interface(); break;
            case 'm': case 'M': 
                if (!latency_calibrated) {
                    printf("\n[REJECTED] Run decay auto-tuning calibration [c] first!\n");
                } else {
                    printf("\nExecuting manual pattern scan...\n"); 
                    process_conveyor_sorting_pipeline();
                }
                print_user_interface(); break;
            case 'a': case 'A':
                if (!latency_calibrated) {
                    printf("\n[REJECTED] Action blocked. Run decay auto-tuning [c] first.\n");
                    print_user_interface(); break;
                }
                printf("\nLaunching continuous conveyor sorting stream. Press 'e' or 'q' to stop...\n\n");
                while (true) {
                    process_conveyor_sorting_pipeline();
                    int break_tok = getchar_timeout_us(0);
                    if (break_tok == 'e' || break_tok == 'E' || break_tok == 'q' || break_tok == 'Q') break;
                    sleep_ms(2); // Tight processing delay ensures instantaneous response tracking
                }
                set_display_rgb(0, 0, 0); print_user_interface(); break;
            default: if (incoming_token != '\n' && incoming_token != '\r') print_user_interface(); break;
        }
    }
}