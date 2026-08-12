#include "core/timer.h"
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>



void timer_setup(void){
 
    rcc_periph_clock_enable(RCC_TIM2);
    

}

void timer_pwm_set_duty_cycle(float duty_cycle){

}