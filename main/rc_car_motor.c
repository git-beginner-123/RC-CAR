#include "rc_car_motor.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "rc_car_config.h"

static const char* TAG = "rc_car_motor";

typedef struct {
    int in1;
    int in2;
    int pwm;
    ledc_channel_t channel;
} motor_side_t;

static const motor_side_t s_left = {
    .in1 = RC_CAR_MOTOR_LEFT_IN1,
    .in2 = RC_CAR_MOTOR_LEFT_IN2,
    .pwm = RC_CAR_MOTOR_LEFT_PWM,
    .channel = LEDC_CHANNEL_0,
};

static const motor_side_t s_right = {
    .in1 = RC_CAR_MOTOR_RIGHT_IN1,
    .in2 = RC_CAR_MOTOR_RIGHT_IN2,
    .pwm = RC_CAR_MOTOR_RIGHT_PWM,
    .channel = LEDC_CHANNEL_1,
};

static bool s_inited = false;
static bool s_any_pin = false;

static bool pin_valid(int pin)
{
    return pin >= 0;
}

static uint32_t duty_from_pct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint32_t)((pct * ((1 << 10) - 1)) / 100);
}

static void setup_gpio_output_if_valid(int pin)
{
    if (!pin_valid(pin)) return;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)pin, 0);
}

static void setup_side(const motor_side_t* side)
{
    if (!side) return;

    setup_gpio_output_if_valid(side->in1);
    setup_gpio_output_if_valid(side->in2);

    if (pin_valid(side->pwm)) {
        ledc_channel_config_t ch = {
            .gpio_num = side->pwm,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = side->channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&ch);
    }
}

static void drive_side(const motor_side_t* side, int dir, int speed_pct)
{
    if (!side) return;

    int a = 0;
    int b = 0;
    if (dir > 0) {
        a = 1;
    } else if (dir < 0) {
        b = 1;
    }

    if (pin_valid(side->in1)) gpio_set_level((gpio_num_t)side->in1, a);
    if (pin_valid(side->in2)) gpio_set_level((gpio_num_t)side->in2, b);

    if (pin_valid(side->pwm)) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, side->channel, (dir == 0) ? 0 : duty_from_pct(speed_pct));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, side->channel);
    }
}

bool RcCarMotor_Init(void)
{
    s_any_pin = pin_valid(s_left.in1) || pin_valid(s_left.in2) || pin_valid(s_left.pwm) ||
                pin_valid(s_right.in1) || pin_valid(s_right.in2) || pin_valid(s_right.pwm);

    if (!s_any_pin) {
        ESP_LOGW(TAG, "motor pins are not configured; commands will be logged only");
        s_inited = true;
        return true;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = RC_CAR_MOTOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    setup_side(&s_left);
    setup_side(&s_right);
    s_inited = true;
    RcCarMotor_Apply(kRcCarMotorCmdStop);
    return true;
}

void RcCarMotor_Deinit(void)
{
    if (!s_inited) return;
    RcCarMotor_Apply(kRcCarMotorCmdStop);
    s_inited = false;
}

const char* RcCarMotor_CommandName(RcCarMotorCmd cmd)
{
    switch (cmd) {
        case kRcCarMotorCmdForward:  return "forward";
        case kRcCarMotorCmdBackward: return "backward";
        case kRcCarMotorCmdLeft:     return "left";
        case kRcCarMotorCmdRight:    return "right";
        case kRcCarMotorCmdStop:
        default:                     return "stop";
    }
}

void RcCarMotor_Apply(RcCarMotorCmd cmd)
{
    int left_dir = 0;
    int right_dir = 0;

    switch (cmd) {
        case kRcCarMotorCmdForward:
            left_dir = 1;
            right_dir = 1;
            break;
        case kRcCarMotorCmdBackward:
            left_dir = -1;
            right_dir = -1;
            break;
        case kRcCarMotorCmdLeft:
            left_dir = -1;
            right_dir = 1;
            break;
        case kRcCarMotorCmdRight:
            left_dir = 1;
            right_dir = -1;
            break;
        case kRcCarMotorCmdStop:
        default:
            left_dir = 0;
            right_dir = 0;
            break;
    }

    if (!s_inited) return;

    drive_side(&s_left, left_dir, RC_CAR_MOTOR_SPEED_PCT);
    drive_side(&s_right, right_dir, RC_CAR_MOTOR_SPEED_PCT);

    ESP_LOGI(TAG, "apply cmd=%s", RcCarMotor_CommandName(cmd));
}
