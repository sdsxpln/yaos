/*
 * "[...] Sincerity (comprising truth-to-experience, honesty towards the self,
 * and the capacity for human empathy and compassion) is a quality which
 * resides within the laguage of literature. It isn't a fact or an intention
 * behind the work [...]"
 *
 *             - An introduction to Literary and Cultural Theory, Peter Barry
 *
 *
 *                                                   o8o
 *                                                   `"'
 *     oooo    ooo  .oooo.    .ooooo.   .oooo.o     oooo   .ooooo.
 *      `88.  .8'  `P  )88b  d88' `88b d88(  "8     `888  d88' `88b
 *       `88..8'    .oP"888  888   888 `"Y88b.       888  888   888
 *        `888'    d8(  888  888   888 o.  )88b .o.  888  888   888
 *         .8'     `Y888""8o `Y8bod8P' 8""888P' Y8P o888o `Y8bod8P'
 *     .o..P'
 *     `Y8P'                   Kyunghwan Kwon <kwon@yaos.io>
 *
 *  Welcome aboard!
 */

#include <kernel/module.h>
#include <asm/pinmap.h>
#include <error.h>
#include <asm/hw.h>
#include <bitops.h>

#include "rcc.h"
#include "../../../include/power.h"

REGISTER_DEVICE(uart, "uart", 1);
REGISTER_DEVICE(gpio, "led", PIN_LED_GREEN);
REGISTER_DEVICE(gpio, "led", PIN_LED_RED);
//REGISTER_DEVICE(ir, "ir", 0);

static inline unsigned int get_pllclk_mhz()
{
	unsigned int m, n, p;
	unsigned int mhz;
	int res, i;

	switch ((RCC_CFGR >> SWS) & 3) {
	case 0x01:
		mhz = HSE / MHZ;
		break;
	case 0x02:
		if ((RCC_PLLCFGR >> PLL_SRC) & 1)
			mhz = HSE / MHZ;
		else
			mhz = HSI / MHZ;

		for (i = 0, res = 1; i < digits(mhz); i++)
			res *= 10;

		m = (RCC_PLLCFGR >> PLL_M) & 0x3f;
		n = (RCC_PLLCFGR >> PLL_N) & 0x1ff;
		switch ((RCC_PLLCFGR >> PLL_P) & 0x3) {
		case 0: p = 2;
			break;
		case 1: p = 4;
			break;
		case 2: p = 6;
			break;
		case 3: p = 8;
		default:
			break;
		}
		mhz = mhz * res * n / m / p / res;
		break;
	default:
		mhz = HSI / MHZ;
		break;
	}

	return mhz;
}

/*
 * Embedded flash memory latency according to CPU clock frequency:
 *
 *     latency     |    2.7v - 3.6v    |    2.4v - 2.7v    |    2.1v - 2.4v    |    1.8v - 2.1v
 * -------------------------------------------------------------------------------------------------
 * 0 (1 cpu cycle) |   0 < HCLK <= 30  |   0 < HCLK <= 24  |   0 < HCLK <= 22  |   0 < HCLK <= 20
 * 1 (2 cpu cycle) |  30 < HCLK <= 60  |  24 < HCLK <= 48  |  22 < HCLK <= 44  |  20 < HCLK <= 40
 * 2 (3 cpu cycle) |  60 < HCLK <= 90  |  48 < HCLK <= 72  |  44 < HCLK <= 66  |  40 < HCLK <= 60
 * 3 (4 cpu cycle) |  90 < HCLK <= 120 |  72 < HCLK <= 96  |  66 < HCLK <= 88  |  60 < HCLK <= 80
 * 4 (5 cpu cycle) | 120 < HCLK <= 150 |  96 < HCLK <= 120 |  88 < HCLK <= 110 |  80 < HCLK <= 100
 * 5 (6 cpu cycle) | 150 < HCLK <= 180 | 120 < HCLK <= 144 | 110 < HCLK <= 132 | 100 < HCLK <= 120
 * 6 (7 cpu cycle) |                   | 144 < HCLK <= 168 | 132 < HCLK <= 154 | 120 < HCLK <= 140
 * 7 (8 cpu cycle) |                   | 168 < HCLK <= 180 | 154 < HCLK <= 176 | 140 < HCLK <= 160
 * 8 (9 cpu cycle) |                   |                   | 176 < HCLK <= 180 | 160 < HCLK <= 168
 */

static void set_flash_latency(const unsigned int hclk_mhz)
{
	int latency, mul;
#if (VDD < 21)
	mul = 20;
#elif (VDD < 24)
	mul = 22;
#elif (VDD < 27)
	mul = 24;
#else
	mul = 30;
#endif
	latency = (hclk_mhz + (mul - 1)) / mul - 1;

	FLASH_ACR |= latency;
	assert((FLASH_ACR & 0x7) == latency);

	/* enable data cache(8 lines * 128 bits = 128 bytes) and
	 * instruction cache(64 lines * 128 bits = 1024 bytes) */
	FLASH_ACR |= 0x600;
#if (VDD >= 21)
	/* enable prefetch buffer */
	FLASH_ACR |= 0x100;
#endif
}

static inline struct pll_t solve_pll_factors(const unsigned int hclk_mhz,
		const unsigned int osc_mhz, const bool pll48clk)
{
	unsigned int vco, m, n, p, q, t;
	unsigned int m_limit, m_base;
	int res, i;

	for (i = 0, res = 1; i < digits(osc_mhz); i++)
		res *= 10;

	m_limit = min(M_MAX, (osc_mhz * KHZ * res / PLLIN_MIN_KHZ + 5) / res);
	m_base = (osc_mhz * KHZ * res / PLLIN_MAX_KHZ + 5) / res;
	m = max(M_MIN, m_base);
	p = P_MIN;
retry:
	t = hclk_mhz * p * res / osc_mhz; /* t == n * res / m */

	while (m <= m_limit) {
		n = m * t / res;
		if (n >= N_MIN && n <= N_MAX && (t * KHZ / res == n * KHZ / m))
			break;
		m++;
	}

	if (m > m_limit) {
		if ((p += P_STEP) > P_MAX) {
			//error("could not solve for the pll factors correctly");
			return (struct pll_t){ 0, };
		}

		m = max(M_MIN, m_base);
		goto retry;
	}

	vco = osc_mhz * (n * res / m) / res;
	q = vco / PLL48CLK_MHZ;

	if ((hclk_mhz != vco / p) ||
			(q < Q_MIN) || (q > Q_MAX) ||
			(pll48clk && (PLL48CLK_MHZ * res != vco * res / q)) ||
			(vco < VCO_MIN_MHZ) || (vco > VCO_MAX_MHZ)) {
		m++;
		goto retry;
	}

	//debug("hclk %d MHz, vco %d MHz, osc %d MHz, p %d, n %d, m %d, q %d",
	//		vco / p, vco, osc_mhz, p, n, m, q);

	return (struct pll_t){ vco, m, n, p, q };
}

static void rcc_reset()
{
	/* make it back to reset value */
	RCC_CR = 0x80 | (1 << HSI_ON);
	while (!gbi(RCC_CR, HSI_RDY));
	RCC_CFGR = 0;
	while (RCC_CFGR);
	RCC_PLLCFGR = 0;
}

static void rcc_init(const unsigned int hclk_mhz)
{
	unsigned int osc_mhz = 16;
	int div;

#ifdef HSE
	BITBAND(&RCC_CR, HSE_ON, ON);
	while (!gbi(RCC_CR, HSE_RDY));

	RCC_PLLCFGR |= 1 << PLL_SRC;
	osc_mhz = HSE / MHZ;
#else
	if (hclk_mhz == 16) /* do not activate PLL but use HSI directly */
		return;
#endif

	struct pll_t pll = solve_pll_factors(hclk_mhz, osc_mhz, true);

	RCC_PLLCFGR |= pll.m << PLL_M;
	RCC_PLLCFGR |= pll.n << PLL_N;
	RCC_PLLCFGR |= (pll.p / 2 - 1) << PLL_P;
	RCC_PLLCFGR |= pll.q << PLL_Q;

	//assert(pll.vco / pll.p == hclk_mhz);

	/* set prescalers */
	for (div = 1; (hclk_mhz / div) > APB_MAX_MHZ; div <<= 1);
	RCC_CFGR |= (0x4 | (div >> 2)) << APB_PRE;
	for (div = 1; (hclk_mhz / div) > LAPB_MAX_MHZ; div <<= 1);
	RCC_CFGR |= (0x4 | (div >> 2)) << LAPB_PRE;

	/* turn PLL on */
	BITBAND(&RCC_CR, PLL_ON, ON);
	while (!gbi(RCC_CR, PLL_RDY));
}

enum rcc_sysclk_t {
	RCC_SYSCLK_HSI	= 0,
	RCC_SYSCLK_HSE	= 1,
	RCC_SYSCLK_PLL	= 2,
	RCC_SYSCLK_MASK	= RCC_SYSCLK_HSI | RCC_SYSCLK_HSE | RCC_SYSCLK_PLL,
};

static void rcc_sysclk_set(enum rcc_sysclk_t clk)
{
	RCC_CFGR |= clk << SW; /* PLL as system clock; 0:HSI, 1:HSE, 2:PLL */
	while (((RCC_CFGR >> SWS) & 3) != clk); /* wait until changes applied */

#ifdef HSE
	RCC_CR &= ~((unsigned int)(1 * clk) << HSI_ON); /* turn HSI off */
#endif
}

void clock_init()
{
	int scalemode = 3;
	bool overdrive = false;

	assert(OPERATING_FREQUENCY_MHZ <= 180);

	if (OPERATING_FREQUENCY_MHZ > 168) {
		scalemode = 1;
		overdrive = true;
	} else if (OPERATING_FREQUENCY_MHZ > 120) {
		scalemode = 2;
		if (OPERATING_FREQUENCY_MHZ > 144)
			overdrive = true;
	}

	__set_power_regulator(ON, scalemode, overdrive);

	set_flash_latency(OPERATING_FREQUENCY_MHZ);
	rcc_reset();
	rcc_init(OPERATING_FREQUENCY_MHZ);
	rcc_sysclk_set(RCC_SYSCLK_PLL);
}
