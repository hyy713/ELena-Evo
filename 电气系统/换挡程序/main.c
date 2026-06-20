#include "stm32f10x.h"
#include <stdbool.h>

/* ================= 状态机 ================= */
typedef enum
{
    SHIFT_IDLE = 0,//空闲
    SHIFT_UP_ACTIVE,//升档执行中
    SHIFT_DOWN_ACTIVE//将当执行中
} ShiftState_t;

/* ================= 参数 ================= */
#define DEBOUNCE_MS 5

/* ================= 全局变量 ================= */
static ShiftState_t gState = SHIFT_IDLE;

/* 升档 */
static uint32_t gUpThrottleEnd = 0;//断火结束时间
static uint32_t gUpSolEnd = 0;//升档阀关闭时间
static uint32_t gUpIgnoreEnd = 0;//升档后的防连击时间

/* 降档 */
static uint32_t gDnSolEnd = 0;//降档阀关闭时间
static uint32_t gDnClutchEnd = 0;//离合关闭时间
static uint32_t gDnIgnoreEnd = 0;//降档后的防连击时间

/* 去抖 + 边沿 */
static uint32_t debUp = 0;
static uint32_t debDown = 0;
static uint32_t debClutch = 0;

static bool upLast = 1;
static bool downLast = 1;
static bool clutchLast = 1;

/* 离合覆盖 */
static bool gClutchOverride = false;

/* ================= SysTick ================= */
volatile uint32_t msTick = 0;

void SysTick_Handler(void)
{
    msTick++;
}//每1ms执行一次

uint32_t millis(void)
{
    return msTick;
}

/* ================= GPIO ================= */
void GPIO_Config(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitTypeDef gpio;

    /* 输入 上拉 */
    gpio.GPIO_Mode = GPIO_Mode_IPU;//未按下=1，按下=0；按下变成低电平
    gpio.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2;//PA0 升档；PA1 降档；PA2 离合
    GPIO_Init(GPIOA, &gpio);

    /* 输出 推挽 */
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 |
                    GPIO_Pin_10 | GPIO_Pin_11;//PB0 断火；PB1 升档；PB10 降档；PB11 离合
    GPIO_Init(GPIOB, &gpio);
	  gpio.GPIO_Pin = GPIO_Pin_13;
    GPIO_Init(GPIOC,&gpio);	

    GPIO_ResetBits(GPIOB, GPIO_Pin_0 | GPIO_Pin_1 |
	GPIO_Pin_10 | GPIO_Pin_11);//上电全部关闭，0，防止误动作
	GPIO_SetBits(GPIOC, GPIO_Pin_13);
}

/* ================= 边沿+去抖 ================= */
bool IsFallingEdge(GPIO_TypeDef* port, uint16_t pin,
                   uint32_t* timer, bool* last)
{
    bool now = GPIO_ReadInputDataBit(port, pin);//拨片按下时，1->0

    if (now == 0)
    {
        if (*timer == 0)
            *timer = millis();
        else if (millis() - *timer >= DEBOUNCE_MS)
        {
            if (*last == 1)
            {
                *last = 0;
                return true; // 长按时只触发一次
            }
        }
    }
    else
    {
        *timer = 0;
        *last = 1;
    }

    return false;
}

/* ================= 主函数 ================= */
int main(void)
{
    SysTick_Config(SystemCoreClock / 1000);
    GPIO_Config();
		uint32_t runLedTimer = 0;

    while (1)
    {
        uint32_t now = millis();
			
			  if(now - runLedTimer >= 500)
				{
					runLedTimer = now;
					GPIOC->ODR ^= GPIO_Pin_13;
				}

        /* ===== 离合按钮（最高优先级）===== */
        bool clutchNow = GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_2);//PA2 离合

        if (clutchNow == 0)
        {
            if (debClutch == 0)
                debClutch = now;
            else if (now - debClutch >= DEBOUNCE_MS)
            {
                gClutchOverride = true;
                GPIO_SetBits(GPIOB, GPIO_Pin_11);//按下时打开离合电磁阀
            }
        }
        else
        {
            debClutch = 0;

            if (gClutchOverride)
            {
                gClutchOverride = false;

                if (gState == SHIFT_DOWN_ACTIVE && now < gDnClutchEnd)//如果正在降档，则离合电磁阀打开，自动控制继续接管
                    GPIO_SetBits(GPIOB, GPIO_Pin_11);
                else
                    GPIO_ResetBits(GPIOB, GPIO_Pin_11);//关闭离合
            }
        }

        /* ===== 空闲状态 ===== */
        if (gState == SHIFT_IDLE)
        {
            /* 升档 */
            if ((now > gUpIgnoreEnd) &&
                (now > gDnIgnoreEnd) &&
                IsFallingEdge(GPIOA, GPIO_Pin_0, &debUp, &upLast))//按下PA0
            {
                GPIO_SetBits(GPIOB, GPIO_Pin_0); // 断火
                GPIO_SetBits(GPIOB, GPIO_Pin_1); // 升档阀打开

                gUpThrottleEnd = now + 100;//100ms后恢复点火
                gUpSolEnd = now + 300;//300ms后关闭升档阀
                gUpIgnoreEnd = now + 400;//400ms内禁止再次升档

                gState = SHIFT_UP_ACTIVE;//状态变为升档中
            }

            /* 降档 */
            else if ((now > gUpIgnoreEnd) &&
                     (now > gDnIgnoreEnd) &&
                     IsFallingEdge(GPIOA, GPIO_Pin_1, &debDown, &downLast))
            {
                GPIO_SetBits(GPIOB, GPIO_Pin_10); // 降档阀
                GPIO_SetBits(GPIOB, GPIO_Pin_11); // 离合

                gDnSolEnd = now + 300;//300ms后关闭降档阀
                gDnClutchEnd = now + 300;//300ms后关闭离合阀
                gDnIgnoreEnd = now + 400;//400ms内禁止再次将当

                gState = SHIFT_DOWN_ACTIVE;//状态变为降档中
            }
        }

        /* ===== 升档过程 ===== */
        if (gState == SHIFT_UP_ACTIVE)
        {
            if (now >= gUpThrottleEnd)
                GPIO_ResetBits(GPIOB, GPIO_Pin_0);

            if (now >= gUpSolEnd)
            {
                GPIO_ResetBits(GPIOB, GPIO_Pin_1);
                gState = SHIFT_IDLE;
            }
        }

        /* ===== 降档过程 ===== */
        if (gState == SHIFT_DOWN_ACTIVE)
        {
            if (now >= gDnSolEnd)
                GPIO_ResetBits(GPIOB, GPIO_Pin_10);

            if (!gClutchOverride && now >= gDnClutchEnd)
                GPIO_ResetBits(GPIOB, GPIO_Pin_11);

            if (now >= gDnSolEnd && now >= gDnClutchEnd)
                gState = SHIFT_IDLE;
        }
    }
}
