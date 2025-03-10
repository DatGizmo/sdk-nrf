/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "hrt.h"
#include <hal/nrf_vpr_csr_vio.h>
#include <hal/nrf_vpr_csr_vtim.h>

/* Hardware requirement, to get n shifts SHIFTCNTB register has to be set to n-1*/
#define SHIFTCNTB_VALUE(shift_count) (shift_count - 1)

#define SPI_INPUT_PIN_NUM 2
#define CNT1_INIT_VALUE	  1
#define MSB_MASK	  (0xff000000)

#define FOURTH_BYTE_SHIFT_CNT 24
#define THIRD_BYTE_SHIFT_CNT  16
#define SECOND_BYTE_SHIFT_CNT 8

/*
 * Macro for calculating TOP value of CNT1. It should be twice as TOP value of CNT0
 * so that input was sampled on the other clock edge than sending.
 * Subtraction of 1 is needed, because value written to the register should be
 * equal to N - 1, where N is the desired value. For the same reason value of
 * cnt0_top needs to be increased by 1 first - it is already in form that should
 * be written to a CNT0 TOP register.
 */
#define CNT1_TOP_CALCULATE(cnt0_top) (2 * ((cnt0_top) + 1) - 1)

static const nrf_vpr_csr_vio_shift_ctrl_t write_final_shift_ctrl_cfg = {
	.shift_count = 1,
	.out_mode = NRF_VPR_CSR_VIO_SHIFT_NONE,
	.frame_width = 4,
	.in_mode = NRF_VPR_CSR_VIO_MODE_IN_CONTINUOUS,
};

static nrf_vpr_csr_vio_shift_ctrl_t xfer_shift_ctrl = {
	.shift_count = SHIFTCNTB_VALUE(BITS_IN_WORD),
	.out_mode = NRF_VPR_CSR_VIO_SHIFT_OUTB_TOGGLE,
	.frame_width = 1,
	.in_mode = NRF_VPR_CSR_VIO_MODE_IN_CONTINUOUS,
};

static void hrt_tx(volatile hrt_xfer_data_t *xfer_data, uint8_t frame_width, bool *counter_running,
		   uint16_t counter_value)
{
	if (xfer_data->word_count == 0) {
		return;
	}

	nrf_vpr_csr_vio_mode_out_t out_mode = {
		.mode = NRF_VPR_CSR_VIO_SHIFT_OUTB_TOGGLE,
		.frame_width = frame_width,
	};
	uint8_t prev_frame_width = xfer_shift_ctrl.frame_width;
	uint32_t data;

	xfer_shift_ctrl.shift_count = SHIFTCNTB_VALUE(BITS_IN_WORD / frame_width);
	xfer_shift_ctrl.frame_width = frame_width;

	nrf_vpr_csr_vio_shift_ctrl_buffered_set(&xfer_shift_ctrl);

	for (uint32_t i = 0; i < xfer_data->word_count; i++) {

		switch (xfer_data->word_count - i) {
		case 1: /* Last transfer */
			xfer_shift_ctrl.shift_count = SHIFTCNTB_VALUE(xfer_data->last_word_clocks);
			nrf_vpr_csr_vio_shift_ctrl_buffered_set(&xfer_shift_ctrl);

			data = xfer_data->last_word;
			break;
		case 2: /* Last but one transfer.*/
			xfer_shift_ctrl.shift_count =
				SHIFTCNTB_VALUE(xfer_data->penultimate_word_clocks);
			nrf_vpr_csr_vio_shift_ctrl_buffered_set(&xfer_shift_ctrl);
		default: /* Intentional fallthrough */
			if (xfer_data->data == NULL) {
				data = 0;
			} else {
				data = ((uint32_t *)xfer_data->data)[i];
			}
		}

		/* In case of bus width change device has to load
		 * new bus width before loading new out data.
		 */
		if (prev_frame_width != frame_width) {
			prev_frame_width = frame_width;
			while (nrf_vpr_csr_vio_shift_cnt_out_get() != 0) {
			}
			nrf_vpr_csr_vio_mode_out_set(&out_mode);
		}

		switch (xfer_data->fun_out) {
		case HRT_FUN_OUT_WORD:
			nrf_vpr_csr_vio_out_buffered_reversed_word_set(data);
			break;
		case HRT_FUN_OUT_BYTE:
			nrf_vpr_csr_vio_out_buffered_reversed_byte_set(data);
			break;
		default:
			break;
		}

		if ((i == 0) && (!*counter_running)) {
			/*
			 * Temporary fix for max frequency. Top value can be set to 0,
			 * but initial value cannot, because counter will not start.
			 */
			if (counter_value == 0) {
				counter_value = 1;
			}
			/* Start counter */
			nrf_vpr_csr_vtim_simple_counter_set(0, counter_value);
			*counter_running = true;
		}
	}
}

void hrt_write(hrt_xfer_t *hrt_xfer_params)
{
	hrt_frame_element_t first_element = HRT_FE_DATA;
	bool counter_running = false;

	nrf_vpr_csr_vio_mode_out_t out_mode = {.mode = NRF_VPR_CSR_VIO_SHIFT_OUTB_TOGGLE};

	/* Configure clock and pins */
	nrf_vpr_csr_vio_dir_set(hrt_xfer_params->tx_direction_mask);

	for (uint8_t i = 0; i < HRT_FE_MAX; i++) {

		if (hrt_xfer_params->xfer_data[i].word_count != 0) {
			first_element = i;
			break;
		}
	}

	switch (first_element) {
	case HRT_FE_COMMAND:
		out_mode.frame_width = hrt_xfer_params->bus_widths.command;
		break;
	case HRT_FE_ADDRESS:
		out_mode.frame_width = hrt_xfer_params->bus_widths.address;
		break;
	case HRT_FE_DATA:
		out_mode.frame_width = hrt_xfer_params->bus_widths.data;
		break;
	default:
		break;
	}
	xfer_shift_ctrl.frame_width = out_mode.frame_width;

	nrf_vpr_csr_vtim_count_mode_set(0, NRF_VPR_CSR_VTIM_COUNT_RELOAD);
	nrf_vpr_csr_vtim_simple_counter_top_set(0, hrt_xfer_params->counter_value);
	nrf_vpr_csr_vio_mode_in_set(NRF_VPR_CSR_VIO_MODE_IN_CONTINUOUS);

	nrf_vpr_csr_vio_mode_out_set(&out_mode);

	switch (hrt_xfer_params->xfer_data[first_element].word_count) {
	case 1:
		nrf_vpr_csr_vio_shift_cnt_out_set(
			hrt_xfer_params->xfer_data[first_element].last_word_clocks);
		break;
	case 2:
		nrf_vpr_csr_vio_shift_cnt_out_set(
			hrt_xfer_params->xfer_data[first_element].penultimate_word_clocks);
		break;
	default:
		nrf_vpr_csr_vio_shift_cnt_out_set(BITS_IN_WORD / out_mode.frame_width);
	}

	/* Enable CE */
	if (hrt_xfer_params->ce_polarity == MSPI_CE_ACTIVE_LOW) {
		nrf_vpr_csr_vio_out_clear_set(BIT(hrt_xfer_params->ce_vio));
	} else {
		nrf_vpr_csr_vio_out_or_set(BIT(hrt_xfer_params->ce_vio));
	}

	/* Transfer command */
	hrt_tx(&hrt_xfer_params->xfer_data[HRT_FE_COMMAND], hrt_xfer_params->bus_widths.command,
	       &counter_running, hrt_xfer_params->counter_value);
	/* Transfer address */
	hrt_tx(&hrt_xfer_params->xfer_data[HRT_FE_ADDRESS], hrt_xfer_params->bus_widths.address,
	       &counter_running, hrt_xfer_params->counter_value);
	/* Transfer dummy cycles */
	hrt_tx(&hrt_xfer_params->xfer_data[HRT_FE_DUMMY_CYCLES],
	       hrt_xfer_params->bus_widths.dummy_cycles, &counter_running,
	       hrt_xfer_params->counter_value);
	/* Transfer data */
	hrt_tx(&hrt_xfer_params->xfer_data[HRT_FE_DATA], hrt_xfer_params->bus_widths.data,
	       &counter_running, hrt_xfer_params->counter_value);

	/* Hardware issue workaround,
	 * additional clock edge when transmitting in modes other than MSPI_CPP_MODE_0.
	 * modes 1 and 3: Disable clock before the last pulse and perform last clock edge manualy.
	 * mode 2: Add one pulse more to the last word in message, and disable clock before the last
	 * pulse.
	 */
	if (hrt_xfer_params->cpp_mode == MSPI_CPP_MODE_0) {
		nrf_vpr_csr_vio_shift_ctrl_buffered_set(&write_final_shift_ctrl_cfg);
		nrf_vpr_csr_vio_out_buffered_reversed_word_set(0x00);
		nrf_vpr_csr_vtim_count_mode_set(0, NRF_VPR_CSR_VTIM_COUNT_STOP);
	} else {
		while (nrf_vpr_csr_vio_shift_cnt_out_get() != 0) {
		}
		nrf_vpr_csr_vtim_count_mode_set(0, NRF_VPR_CSR_VTIM_COUNT_STOP);

		nrf_vpr_csr_vio_shift_ctrl_buffered_set(&write_final_shift_ctrl_cfg);

		if (hrt_xfer_params->cpp_mode == MSPI_CPP_MODE_1) {
			nrf_vpr_csr_vio_out_clear_set(BIT(hrt_xfer_params->clk_vio));
		} else if (hrt_xfer_params->cpp_mode == MSPI_CPP_MODE_3) {
			nrf_vpr_csr_vio_out_or_set(BIT(hrt_xfer_params->clk_vio));
		}
	}

	/* Reset counter 0, Next message may be sent incorrectly if counter is not reset here. */
	nrf_vpr_csr_vtim_simple_counter_set(0, 0);

	/* Disable CE */
	if (!hrt_xfer_params->ce_hold) {

		if (hrt_xfer_params->ce_polarity == MSPI_CE_ACTIVE_LOW) {
			nrf_vpr_csr_vio_out_or_set(BIT(hrt_xfer_params->ce_vio));
		} else {
			nrf_vpr_csr_vio_out_clear_set(BIT(hrt_xfer_params->ce_vio));
		}
	}
}

static void hrt_tx_rx(volatile hrt_xfer_data_t *xfer_data, uint8_t frame_width, bool start_counter,
		      uint16_t cnt0_val, uint16_t cnt1_val)
{
	nrf_vpr_csr_vio_shift_ctrl_t shift_ctrl = {
		.shift_count = SHIFTCNTB_VALUE(BITS_IN_BYTE / frame_width),
		.out_mode = NRF_VPR_CSR_VIO_SHIFT_OUTB_TOGGLE,
		.frame_width = frame_width,
		.in_mode = NRF_VPR_CSR_VIO_MODE_IN_SHIFT,
	};

	uint32_t to_send = *((uint32_t *)xfer_data->data);

	if (xfer_data->word_count == 0) {
		return;
	}

	nrf_vpr_csr_vio_shift_ctrl_buffered_set(&shift_ctrl);

	for (uint32_t i = 0; i < xfer_data->word_count; i++) {
		switch (xfer_data->fun_out) {
		case HRT_FUN_OUT_WORD:
			nrf_vpr_csr_vio_out_buffered_reversed_word_set(to_send & MSB_MASK);
			break;
		case HRT_FUN_OUT_BYTE:
			nrf_vpr_csr_vio_out_buffered_reversed_byte_set(to_send & MSB_MASK);
			break;
		default:
			break;
		}

		to_send = to_send << BITS_IN_BYTE;

		if ((i == 0) && start_counter) {
			/* Start both counters */
			nrf_vpr_csr_vtim_combined_counter_set(
				(cnt0_val << VPRCSR_NORDIC_CNT_CNT0_Pos) +
				(cnt1_val << VPRCSR_NORDIC_CNT_CNT1_Pos));
		} else {
			/*
			 * Since we start reading right after the transmission is started,
			 * we need to read from INB register in the meantime, even if stop_cnt
			 * from nrf_vpr_csr_vio_config_t is set to false. Otherwise clock is
			 * not generated when the actual data is sent by a peripheral device.
			 */
			nrf_vpr_csr_vio_in_buffered_reversed_byte_get();
		}
	}
}

void hrt_read(volatile hrt_xfer_t *hrt_xfer_params)
{
	static const nrf_vpr_csr_vio_shift_ctrl_t shift_ctrl = {
		.out_mode = NRF_VPR_CSR_VIO_SHIFT_NONE,
		.in_mode = NRF_VPR_CSR_VIO_MODE_IN_CONTINUOUS,
	};
	static const nrf_vpr_csr_vio_mode_out_t out_mode = {
		.mode = NRF_VPR_CSR_VIO_SHIFT_OUTB_TOGGLE,
		.frame_width = 1,
	};
	nrf_vpr_csr_vio_mode_out_t out_mode_in = {
		.mode = NRF_VPR_CSR_VIO_SHIFT_OUTB_TOGGLE,
		.frame_width = hrt_xfer_params->bus_widths.data,
	};

	uint32_t rec_data = 0;
	uint32_t iter = 0;

	/* Enable CS */
	if (hrt_xfer_params->ce_polarity == MSPI_CE_ACTIVE_LOW) {
		nrf_vpr_csr_vio_out_clear_set(BIT(hrt_xfer_params->ce_vio));
	} else {
		nrf_vpr_csr_vio_out_or_set(BIT(hrt_xfer_params->ce_vio));
	}

	/* Configure clock and pins */
	/* Set DQ1 as input in SPI case. */
	if (hrt_xfer_params->bus_widths.data == 1) {
		WRITE_BIT(hrt_xfer_params->tx_direction_mask, SPI_INPUT_PIN_NUM,
			  VPRCSR_NORDIC_DIR_INPUT);
		nrf_vpr_csr_vio_dir_set(hrt_xfer_params->tx_direction_mask);
	}

	/* Initial configuration */
	nrf_vpr_csr_vio_mode_in_set(NRF_VPR_CSR_VIO_MODE_IN_SHIFT);
	nrf_vpr_csr_vio_mode_out_set(&out_mode);
	nrf_vpr_csr_vio_shift_cnt_out_set(BITS_IN_BYTE);

	/* Counter settings */
	nrf_vpr_csr_vtim_count_mode_set(0, NRF_VPR_CSR_VTIM_COUNT_RELOAD);
	nrf_vpr_csr_vtim_count_mode_set(1, NRF_VPR_CSR_VTIM_COUNT_RELOAD);

	/* Set top counters value. Trigger data capture every two clock cycles */
	nrf_vpr_csr_vtim_simple_counter_top_set(0, hrt_xfer_params->counter_value);
	nrf_vpr_csr_vtim_simple_counter_top_set(1,
						CNT1_TOP_CALCULATE(hrt_xfer_params->counter_value));

	/* Transfer command */
	hrt_tx_rx(&hrt_xfer_params->xfer_data[HRT_FE_COMMAND], hrt_xfer_params->bus_widths.command,
		  true, hrt_xfer_params->counter_value, CNT1_INIT_VALUE);

	/* Transfer address */
	hrt_tx_rx(&hrt_xfer_params->xfer_data[HRT_FE_ADDRESS], hrt_xfer_params->bus_widths.address,
		  false, hrt_xfer_params->counter_value, CNT1_INIT_VALUE);

	/* Set pins as input for cases other than SINGLE mode. */
	if (hrt_xfer_params->bus_widths.data != 1) {
		nrf_vpr_csr_vio_dir_set(hrt_xfer_params->rx_direction_mask);
	}

	if (hrt_xfer_params->bus_widths.address != hrt_xfer_params->bus_widths.data) {
		/*
		 * When writing to SHIFTCTRLB, reception of data starts 6 clock cycles (3 bytes) too
		 * late. When writing to OUTMODEB and SHIFTCNTB separately, the problem disappears.
		 */
		nrf_vpr_csr_vio_mode_out_buffered_set(&out_mode_in);
		nrf_vpr_csr_vio_shift_cnt_out_buffered_set(
			SHIFTCNTB_VALUE(BITS_IN_BYTE / hrt_xfer_params->bus_widths.data));

		/*
		 * When calling `vpr_csr_vio_in_buffered_reversed_byte_get` for the first time in
		 * 1_1_4 mode, CPU stalls for 8 clock cycles instead of 2 (so there are 4 bytes in
		 * the register, not 1). It is probably due to HW issue causing SHIFT_CNT_IN to be
		 * updated one word too late. For this reason, read all four bytes at once, save
		 * them in a temporary variable, and put them in buffer later to not perform not
		 * needed operations during receive.
		 */
		rec_data = nrf_vpr_csr_vio_in_buffered_reversed_byte_get();
		iter = 4;
	}

	/* Receive data. For QUAD_1_1_4 mode this starts later due to HW issue described above. */
	for (; iter < hrt_xfer_params->xfer_data[HRT_FE_DATA].word_count; iter++) {
		hrt_xfer_params->xfer_data[HRT_FE_DATA].data[iter] =
			nrf_vpr_csr_vio_in_buffered_reversed_byte_get() >> FOURTH_BYTE_SHIFT_CNT;
	}

	/* Stop counters */
	nrf_vpr_csr_vtim_count_mode_set(0, NRF_VPR_CSR_VTIM_COUNT_STOP);
	nrf_vpr_csr_vtim_count_mode_set(1, NRF_VPR_CSR_VTIM_COUNT_STOP);

	/* Final configuration */
	nrf_vpr_csr_vio_shift_ctrl_buffered_set(&shift_ctrl);

	/* Disable CS */
	if (!hrt_xfer_params->ce_hold) {

		if (hrt_xfer_params->ce_polarity == MSPI_CE_ACTIVE_LOW) {
			nrf_vpr_csr_vio_out_or_set(BIT(hrt_xfer_params->ce_vio));
		} else {
			nrf_vpr_csr_vio_out_clear_set(BIT(hrt_xfer_params->ce_vio));
		}
	}

	/* Copy the first 4 bytes of data to buffer. */
	if (hrt_xfer_params->bus_widths.address != hrt_xfer_params->bus_widths.data) {
		hrt_xfer_params->xfer_data[HRT_FE_DATA].data[0] = rec_data;
		hrt_xfer_params->xfer_data[HRT_FE_DATA].data[1] = rec_data >> SECOND_BYTE_SHIFT_CNT;
		hrt_xfer_params->xfer_data[HRT_FE_DATA].data[2] = rec_data >> THIRD_BYTE_SHIFT_CNT;
		hrt_xfer_params->xfer_data[HRT_FE_DATA].data[3] = rec_data >> FOURTH_BYTE_SHIFT_CNT;
	}

	/* Set DQ1 back as output in SINGLE mode. */
	if (hrt_xfer_params->bus_widths.data == 1) {
		WRITE_BIT(hrt_xfer_params->tx_direction_mask, SPI_INPUT_PIN_NUM,
			  VPRCSR_NORDIC_DIR_OUTPUT);
		nrf_vpr_csr_vio_dir_set(hrt_xfer_params->tx_direction_mask);
	}
}
