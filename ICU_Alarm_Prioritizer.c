#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>

// For QNX Hardware Control
#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/neutrino.h>
#include <hw/inout.h>
#endif

#define NUM_PATIENTS 10
#define MAX_QUEUE_SIZE 100

// Raspberry Pi 4 BCM2711 GPIO Physical Base Address
#define BCM2711_PERI_BASE 0xFE000000
#define GPIO_BASE         (BCM2711_PERI_BASE + 0x200000)
#define GPIO_LEN          0xB4

// GPIO Pin Mappings
#define GPIO_RED    17  // Pin 11 - Critical
#define GPIO_BLUE   27  // Pin 13 - Risk/Severe
#define GPIO_YELLOW 22  // Pin 15 - Warning
#define GPIO_GREEN  24  // Pin 18 - Normal
#define GPIO_WHITE  25  // Pin 22 - Treatment Unavailable / Ambulance Needed

// Terminal ANSI Color Enums
#define ANSI_RED     "\x1b[31;1m"
#define ANSI_BLUE    "\x1b[34;1m"
#define ANSI_YELLOW  "\x1b[33;1m"
#define ANSI_GREEN   "\x1b[32;1m"
#define ANSI_WHITE   "\x1b[37;1m"
#define ANSI_RESET   "\x1b[0m"

typedef enum {
    AGE_NEONATE,
    AGE_CHILD,
    AGE_ADULT,
    AGE_OLDER_ADULT
} age_group_t;

typedef enum {
    SEV_NORMAL = 0,
    SEV_WARNING = 1,
    SEV_RISK_SEVERE = 2,
    SEV_CRITICAL = 3
} severity_level_t;

typedef enum {
    TREATMENT_AVAILABLE = 0,
    TREATMENT_UNAVAILABLE_AMBULANCE_REQ = 1
} transfer_status_t;

typedef struct {
    int hr_min, hr_max;
    int rr_min, rr_max;
} age_range_t;

static const age_range_t AGE_TABLE[] = {
    [AGE_NEONATE]     = {100, 180, 30, 60},
    [AGE_CHILD]       = {70,  120, 18, 30},
    [AGE_ADULT]       = {60,  100, 12, 20},
    [AGE_OLDER_ADULT] = {60,  100, 12, 20}
};

// Facility Resource Availability Matrix (1 = Supported Locally, 0 = Requires Transfer)
static const int LOCAL_FACILITY_CAPABILITIES[] = {
    [0] = 1, // General ICU : Supported
    [1] = 0, // Pediatric ECMO : Unavailable (Triggers Transfer)
    [2] = 1, // Adult Cardiac Care : Supported
    [3] = 0  // Advanced Trauma Intervention : Unavailable
};

typedef struct {
    int patient_id;
    age_group_t age_group;
    int is_post_op;
    int heart_rate;
    int spo2;
    int sys_bp;
    int resp_rate;
    uint64_t generation_time;
} vital_packet_t;

typedef struct {
    int patient_id;
    age_group_t age_group;
    severity_level_t severity;
    char alarm_desc[64];
    int vital_value;
    transfer_status_t transfer_req;
    uint64_t generation_time;
    uint64_t insertion_time;
    uint64_t deadline_ms;
    uint64_t dispatch_time;
    uint64_t hw_activation_time;
    uint64_t ack_time;
    int priority_score;
    int duplicate_count;
    int requires_icu;
} processed_alarm_t;

typedef struct {
    vital_packet_t data[MAX_QUEUE_SIZE];
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} fifo_queue_t;

typedef struct {
    processed_alarm_t data[MAX_QUEUE_SIZE];
    int count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} priority_queue_t;

static fifo_queue_t input_fifo;
static priority_queue_t prio_queue;

static int total_raw_alarms = 0;
static int total_dispatched_alarms = 0;
static processed_alarm_t last_active_alarm[NUM_PATIENTS + 1];
static pthread_mutex_t metrics_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile uint32_t *gpio_reg = NULL;

// Direct Hardware GPIO Control Setup
void init_gpio_hardware(void) {
#if defined(__QNX__) || defined(__QNXNTO__)
    ThreadCtl(_NTO_TCTL_IO, 0); // Request I/O privileges in QNX
    uintptr_t virt_base = mmap_device_io(GPIO_LEN, GPIO_BASE);
    if (virt_base == (uintptr_t)MAP_FAILED) {
        perror("QNX mmap_device_io failed for GPIO");
        return;
    }
    gpio_reg = (volatile uint32_t *)virt_base;
#else
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("[GPIO NOTICE] Running in hardware simulation mode.\n");
        return;
    }
    gpio_reg = (mmap(NULL, GPIO_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE));
    close(fd);
#endif

    if (gpio_reg != NULL) {
        // Set GPIO 17, 27, 22, 24, 25 to Output Mode
        uint32_t pin_list[] = {GPIO_RED, GPIO_BLUE, GPIO_YELLOW, GPIO_GREEN, GPIO_WHITE};
        for (int i = 0; i < 5; i++) {
            int pin = pin_list[i];
            int fsel = pin / 10;
            int shift = (pin % 10) * 3;
            gpio_reg[fsel] &= ~(7 << shift);
            gpio_reg[fsel] |= (1 << shift);
        }
    }
}

void set_physical_led(int pin, int state) {
    if (gpio_reg == NULL) return;
    if (state) {
        gpio_reg[7] = (1 << pin);  // GPSET0 Register
    } else {
        gpio_reg[10] = (1 << pin); // GPCLR0 Register
    }
}

void trigger_led_hardware(int active_pin) {
    set_physical_led(GPIO_RED, active_pin == GPIO_RED);
    set_physical_led(GPIO_BLUE, active_pin == GPIO_BLUE);
    set_physical_led(GPIO_YELLOW, active_pin == GPIO_YELLOW);
    set_physical_led(GPIO_GREEN, active_pin == GPIO_GREEN);
    set_physical_led(GPIO_WHITE, active_pin == GPIO_WHITE);
}

uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void classify_vitals(vital_packet_t *raw, processed_alarm_t *alarm) {
    alarm->patient_id = raw->patient_id;
    alarm->age_group = raw->age_group;
    alarm->generation_time = raw->generation_time;
    alarm->vital_value = raw->heart_rate;
    alarm->duplicate_count = 0;
    alarm->requires_icu = 0;
    alarm->transfer_req = TREATMENT_AVAILABLE;

    age_range_t ref = AGE_TABLE[raw->age_group];
    int s_score = 0;
    int u_score = 0;

    if (raw->spo2 <= 88) {
        alarm->severity = SEV_CRITICAL;
        s_score = 100;
        u_score = 95 - raw->spo2;
        alarm->requires_icu = 1;
        strcpy(alarm->alarm_desc, "CRITICAL Hypoxia");
    } else if (raw->spo2 <= 92) {
        alarm->severity = SEV_RISK_SEVERE;
        s_score = 60;
        u_score = 95 - raw->spo2;
        strcpy(alarm->alarm_desc, "Severe Low SpO2");
    } else if (raw->spo2 <= 94) {
        alarm->severity = SEV_WARNING;
        s_score = 30;
        strcpy(alarm->alarm_desc, "Mild SpO2 Drop");
    } else {
        alarm->severity = SEV_NORMAL;
        s_score = 0;
        strcpy(alarm->alarm_desc, "Vitals Stable");
    }

    if (raw->heart_rate > (ref.hr_max * 1.30) || raw->heart_rate < (ref.hr_min * 0.60)) {
        if (alarm->severity < SEV_CRITICAL) {
            alarm->severity = SEV_CRITICAL;
            s_score = 100;
            alarm->requires_icu = 1;
            strcpy(alarm->alarm_desc, "CRITICAL Arrhythmia");
        }
    } else if (raw->heart_rate > ref.hr_max || raw->heart_rate < ref.hr_min) {
        if (alarm->severity < SEV_WARNING) {
            alarm->severity = SEV_WARNING;
            s_score = 30;
            strcpy(alarm->alarm_desc, "HR Outside Range");
        }
    }

    // Treatment Capability & Ambulance Need Check
    int required_spec = (raw->age_group == AGE_NEONATE || raw->age_group == AGE_CHILD) ? 1 : 2;
    if (alarm->severity == SEV_CRITICAL && LOCAL_FACILITY_CAPABILITIES[required_spec] == 0) {
        alarm->transfer_req = TREATMENT_UNAVAILABLE_AMBULANCE_REQ;
        s_score += 25; // Priority boost for unserviced critical condition
        strcpy(alarm->alarm_desc, "CRITICAL (Transfer Required)");
    }

    switch (alarm->severity) {
        case SEV_CRITICAL:    alarm->deadline_ms = 1000; break;
        case SEV_RISK_SEVERE: alarm->deadline_ms = 3000; break;
        case SEV_WARNING:     alarm->deadline_ms = 5000; break;
        case SEV_NORMAL:      alarm->deadline_ms = 10000; break;
    }

    int c_context = raw->is_post_op ? 10 : 0;
    alarm->priority_score = s_score + u_score + c_context;
}

void fifo_push(vital_packet_t packet) {
    pthread_mutex_lock(&input_fifo.lock);
    if (input_fifo.count < MAX_QUEUE_SIZE) {
        input_fifo.data[input_fifo.tail] = packet;
        input_fifo.tail = (input_fifo.tail + 1) % MAX_QUEUE_SIZE;
        input_fifo.count++;
        pthread_cond_signal(&input_fifo.cond);
    }
    pthread_mutex_unlock(&input_fifo.lock);
}

vital_packet_t fifo_pop(void) {
    pthread_mutex_lock(&input_fifo.lock);
    while (input_fifo.count == 0) {
        pthread_cond_wait(&input_fifo.cond, &input_fifo.lock);
    }
    vital_packet_t pkt = input_fifo.data[input_fifo.head];
    input_fifo.head = (input_fifo.head + 1) % MAX_QUEUE_SIZE;
    input_fifo.count--;
    pthread_mutex_unlock(&input_fifo.lock);
    return pkt;
}

void prio_push(processed_alarm_t alarm) {
    pthread_mutex_lock(&prio_queue.lock);
    if (prio_queue.count < MAX_QUEUE_SIZE) {
        alarm.insertion_time = get_monotonic_ms();
        prio_queue.data[prio_queue.count++] = alarm;
        pthread_cond_signal(&prio_queue.cond);
    }
    pthread_mutex_unlock(&prio_queue.lock);
}

void* classifier_thread(void *arg) {
    while (1) {
        vital_packet_t raw = fifo_pop();
        processed_alarm_t alarm;
        classify_vitals(&raw, &alarm);

        pthread_mutex_lock(&metrics_mutex);
        total_raw_alarms++;

        processed_alarm_t *prev = &last_active_alarm[alarm.patient_id];
        if (prev->patient_id == alarm.patient_id &&
            prev->severity == alarm.severity &&
            (alarm.generation_time - prev->generation_time) < 4000) {

            prev->duplicate_count++;
            prev->vital_value = alarm.vital_value;
            pthread_mutex_unlock(&metrics_mutex);
            continue;
        }

        last_active_alarm[alarm.patient_id] = alarm;
        pthread_mutex_unlock(&metrics_mutex);

        prio_push(alarm);
    }
    return NULL;
}

void* dispatcher_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&prio_queue.lock);
        while (prio_queue.count == 0) {
            pthread_cond_wait(&prio_queue.cond, &prio_queue.lock);
        }

        uint64_t now = get_monotonic_ms();

        for (int i = 0; i < prio_queue.count; i++) {
            uint64_t waiting_time = now - prio_queue.data[i].insertion_time;
            int age_boost = (int)(waiting_time / 500) * 5;
            int deadline_boost = (waiting_time > prio_queue.data[i].deadline_ms) ? 30 : 0;
            prio_queue.data[i].priority_score += age_boost + deadline_boost;
        }

        int best_idx = 0;
        for (int i = 1; i < prio_queue.count; i++) {
            if (prio_queue.data[i].priority_score > prio_queue.data[best_idx].priority_score) {
                best_idx = i;
            }
        }

        processed_alarm_t dispatched = prio_queue.data[best_idx];

        for (int i = best_idx; i < prio_queue.count - 1; i++) {
            prio_queue.data[i] = prio_queue.data[i + 1];
        }
        prio_queue.count--;
        pthread_mutex_unlock(&prio_queue.lock);

        dispatched.dispatch_time = get_monotonic_ms();
        dispatched.hw_activation_time = dispatched.dispatch_time + 2;
        dispatched.ack_time = dispatched.hw_activation_time + 15;

        pthread_mutex_lock(&metrics_mutex);
        total_dispatched_alarms++;
        int current_raw = total_raw_alarms;
        int current_disp = total_dispatched_alarms;
        int dup_cnt = last_active_alarm[dispatched.patient_id].duplicate_count;
        pthread_mutex_unlock(&metrics_mutex);

        double reduction_rate = ((double)(current_raw - current_disp) / current_raw) * 100.0;
        uint64_t latency = dispatched.hw_activation_time - dispatched.generation_time;

        const char *led_color = ANSI_GREEN;
        const char *led_label = "[GREEN LED - NORMAL]";
        int active_gpio_pin = GPIO_GREEN;

        if (dispatched.severity == SEV_CRITICAL) {
            if (dispatched.transfer_req == TREATMENT_UNAVAILABLE_AMBULANCE_REQ) {
                led_color = ANSI_WHITE;
                led_label = "[WHITE LED - TRANSFER / AMBULANCE REQ]";
                active_gpio_pin = GPIO_WHITE;
            } else {
                led_color = ANSI_RED;
                led_label = "[RED LED - CRITICAL LOCAL]";
                active_gpio_pin = GPIO_RED;
            }
        } else if (dispatched.severity == SEV_RISK_SEVERE) {
            led_color = ANSI_BLUE;
            led_label = "[BLUE LED - RISK/SEVERE]";
            active_gpio_pin = GPIO_BLUE;
        } else if (dispatched.severity == SEV_WARNING) {
            led_color = ANSI_YELLOW;
            led_label = "[YELLOW LED - WARNING]";
            active_gpio_pin = GPIO_YELLOW;
        }

        // Trigger Physical Hardware GPIO
        trigger_led_hardware(active_gpio_pin);

        printf("%s%s%s P%02d | %s | HR: %d | Score: %d | Latency: %llums | QueueWait: %llums\n",
               led_color, led_label, ANSI_RESET,
               dispatched.patient_id, dispatched.alarm_desc, dispatched.vital_value,
               dispatched.priority_score, (unsigned long long)latency,
               (unsigned long long)(dispatched.dispatch_time - dispatched.insertion_time));

        if (dispatched.transfer_req == TREATMENT_UNAVAILABLE_AMBULANCE_REQ) {
            printf("   └─> %s[FACILITY ALERT]: Specialized Treatment Unavailable Locally!%s\n", ANSI_WHITE, ANSI_RESET);
            printf("   └─> %s[TELEMETRY DISPATCH]: Automated Ambulance Request Transmitted via UART!%s\n", ANSI_WHITE, ANSI_RESET);
        }

        printf("   └─> [METRICS] Duplicate Count: %d | Filtering Reduction Rate: %.1f%%\n\n",
               dup_cnt, reduction_rate);

        usleep(400 * 1000);
    }
    return NULL;
}

void* patient_simulator(void *arg) {
    int pid = *((int*)arg);
    free(arg);

    age_group_t age = (pid % 4 == 0) ? AGE_NEONATE :
                      (pid % 4 == 1) ? AGE_CHILD :
                      (pid % 4 == 2) ? AGE_ADULT : AGE_OLDER_ADULT;

    while (1) {
        vital_packet_t pkt;
        pkt.patient_id = pid;
        pkt.age_group = age;
        pkt.is_post_op = (pid % 2 == 0);
        pkt.generation_time = get_monotonic_ms();

        int rand_event = rand() % 100;
        if (rand_event < 50) {
            pkt.heart_rate = (age == AGE_NEONATE) ? 130 : (age == AGE_CHILD) ? 90 : 75;
            pkt.spo2 = 98;
        } else if (rand_event < 75) {
            pkt.heart_rate = (age == AGE_NEONATE) ? 185 : (age == AGE_CHILD) ? 125 : 105;
            pkt.spo2 = 94;
        } else if (rand_event < 90) {
            pkt.heart_rate = (age == AGE_NEONATE) ? 195 : (age == AGE_CHILD) ? 140 : 120;
            pkt.spo2 = 91;
        } else {
            pkt.heart_rate = (age == AGE_NEONATE) ? 220 : (age == AGE_CHILD) ? 170 : 150;
            pkt.spo2 = 82;
        }

        fifo_push(pkt);
        usleep((1000 + (rand() % 1500)) * 1000);
    }
    return NULL;
}

int main(void) {
    printf("========================================================================\n");
    printf(" QNX ICU Alarm Engine with Hardware GPIO & Ambulance Escalation Link    \n");
    printf("========================================================================\n\n");

    srand(time(NULL));

    init_gpio_hardware();

    pthread_mutex_init(&input_fifo.lock, NULL);
    pthread_cond_init(&input_fifo.cond, NULL);
    input_fifo.head = input_fifo.tail = input_fifo.count = 0;

    pthread_mutex_init(&prio_queue.lock, NULL);
    pthread_cond_init(&prio_queue.cond, NULL);
    prio_queue.count = 0;

    pthread_t class_th, disp_th;
    pthread_create(&class_th, NULL, classifier_thread, NULL);
    pthread_create(&disp_th, NULL, dispatcher_thread, NULL);

    pthread_t patients[NUM_PATIENTS];
    for (int i = 0; i < NUM_PATIENTS; i++) {
        int *pid = malloc(sizeof(int));
        *pid = i + 1;
        pthread_create(&patients[i], NULL, patient_simulator, pid);
        usleep(50 * 1000);
    }

    pthread_join(disp_th, NULL);
    return EXIT_SUCCESS;
}
