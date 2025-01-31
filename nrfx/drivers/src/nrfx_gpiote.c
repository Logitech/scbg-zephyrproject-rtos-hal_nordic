/*
 * Copyright (c) 2015 - 2023, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <nrfx.h>

#if NRFX_CHECK(NRFX_GPIOTE_ENABLED)

#include <nrfx_gpiote.h>
#include <helpers/nrfx_flag32_allocator.h>
#include "nrf_bitmask.h"
#include <string.h>

#define NRFX_LOG_MODULE GPIOTE
#include <nrfx_log.h>

/* Macro returning number of pins in the port */
#define GPIO_PIN_NUM(periph, prefix, i, _) NRFX_CONCAT(periph, prefix, i, _PIN_NUM)

/* Macro for calculating total number of pins. */
#define MAX_PIN_NUMBER NRFX_FOREACH_PRESENT(P, GPIO_PIN_NUM, (+), (0), _)

/* Macro returns true if port has 32 pins. */
#define GPIO_IS_FULL_PORT(periph, prefix, i, _) \
    (NRFX_CONCAT(periph, prefix, i, _PIN_NUM) == 32)

/* Macro return true if all ports has 32 pins. In that case pin numbers are continuous. */
#define FULL_PORTS_PRESENT (NRFX_FOREACH_PRESENT(P, GPIO_IS_FULL_PORT, (&&), (1), _))

/* Use legacy configuration if new is not present. That will lead to slight
 * increase of RAM usage since number of slots will exceed application need.
 */
#ifndef NRFX_GPIOTE_CONFIG_NUM_OF_EVT_HANDLERS
#define NRFX_GPIOTE_CONFIG_NUM_OF_EVT_HANDLERS \
        (GPIOTE_CH_NUM + NRFX_GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS)
#endif

/* Verify that trigger matches gpiote enum. */
NRFX_STATIC_ASSERT(NRFX_GPIOTE_TRIGGER_LOTOHI == GPIOTE_CONFIG_POLARITY_LoToHi);
NRFX_STATIC_ASSERT(NRFX_GPIOTE_TRIGGER_HITOLO == GPIOTE_CONFIG_POLARITY_HiToLo);
NRFX_STATIC_ASSERT(NRFX_GPIOTE_TRIGGER_TOGGLE == GPIOTE_CONFIG_POLARITY_Toggle);

/*
 * 2 bytes are dedicated for each pin to store it's current state.
 *
 * +--------+-------+-----------------------+-----+--------+--------+---------+-------+
 * | 0      | 1     | 2-4                   | 5   | 6      | 7      | 8-12    | 13-15 |
 * +--------+-------+-----------------------+-----+--------+--------+---------+-------+
 * | in use | dir   | nrfx_gpiote_trigger_t | te  | skip   | N/A    |8:       | TE    |
 * | 0: no  | 0:in  |                       | used| config |        | present | index |
 * | 1: yes | 1:out |                       |     |        |        |9-12:    | (when |
 * |        |       |                       |     |        |        | handler |  used)|
 * |        |       |                       |     |        |        | index   |       |
 * +--------+-------+-----------------------+-----+--------+--------+---------+-------+
 *
 */

/* Flags content when pin is not used by the driver. */
#define PIN_FLAG_NOT_USED 0

#define PIN_FLAG_IN_USE NRFX_BIT(0)

#define PIN_FLAG_DIR_MASK NRFX_BIT(1)

/* Flag indicating output direction. */
#define PIN_FLAG_OUTPUT PIN_FLAG_DIR_MASK

/* Macro checks if pin is output. */
#define PIN_FLAG_IS_OUTPUT(flags) ((flags & PIN_FLAG_DIR_MASK) == PIN_FLAG_OUTPUT)

/* Trigger mode field. It stores the information about a trigger type. If trigger
 * is not enabled, it holds information about task usage and pin direction. */
#define PIN_FLAG_TRIG_MODE_OFFSET 2
#define PIN_FLAG_TRIG_MODE_BITS 3
#define PIN_FLAG_TRIG_MODE_MASK \
        (NRFX_BIT_MASK(PIN_FLAG_TRIG_MODE_BITS) << PIN_FLAG_TRIG_MODE_OFFSET)
NRFX_STATIC_ASSERT(NRFX_GPIOTE_TRIGGER_MAX <= NRFX_BIT(PIN_FLAG_TRIG_MODE_BITS));

/* Macro sets trigger mode field. */
#define PIN_FLAG_TRIG_MODE_SET(trigger) (trigger << PIN_FLAG_TRIG_MODE_OFFSET)

/* Macro gets trigger mode from pin flags. */
#define PIN_FLAG_TRIG_MODE_GET(flags) \
        (nrfx_gpiote_trigger_t)((flags & PIN_FLAG_TRIG_MODE_MASK) >> PIN_FLAG_TRIG_MODE_OFFSET)

#define PIN_FLAG_TE_USED        NRFX_BIT(5)
#define PIN_FLAG_SKIP_CONFIG    NRFX_BIT(6)

#define PIN_FLAG_HANDLER_PRESENT NRFX_BIT(8)

#define PIN_HANDLER_ID_SHIFT 9
#define PIN_HANDLER_ID_BITS 4
#define PIN_HANDLER_ID_MASK (NRFX_BIT_MASK(PIN_HANDLER_ID_BITS) << PIN_HANDLER_ID_SHIFT)
#define PIN_HANDLER_MASK (PIN_FLAG_HANDLER_PRESENT | PIN_HANDLER_ID_MASK)

/* Macro for encoding handler index into the flags. */
#define PIN_FLAG_HANDLER(x) \
        (PIN_FLAG_HANDLER_PRESENT | ((x) << PIN_HANDLER_ID_SHIFT))

/* Pin in use but no handler attached. */
#define PIN_FLAG_NO_HANDLER -1

/* Macro for getting handler index from flags. -1 is returned when no handler */
#define PIN_GET_HANDLER_ID(flags) \
        ((flags & PIN_FLAG_HANDLER_PRESENT) \
         ? (int32_t)((flags & PIN_HANDLER_ID_MASK) >> PIN_HANDLER_ID_SHIFT) \
         : PIN_FLAG_NO_HANDLER)

#define PIN_HANDLER_MAX_COUNT NRFX_BIT_MASK(PIN_HANDLER_ID_BITS)
NRFX_STATIC_ASSERT(NRFX_GPIOTE_CONFIG_NUM_OF_EVT_HANDLERS <= PIN_HANDLER_MAX_COUNT);

#define PIN_TE_ID_SHIFT 13
#define PIN_TE_ID_BITS 3
#define PIN_TE_ID_MASK (NRFX_BIT_MASK(PIN_TE_ID_BITS) << PIN_TE_ID_SHIFT)

/* Validate that field is big enough for number of channels. */
NRFX_STATIC_ASSERT((NRFX_BIT(PIN_TE_ID_BITS)) >= GPIOTE_CH_NUM);

/* Macro for encoding Task/Event index into the flags. */
#define PIN_FLAG_TE_ID(x) \
        (PIN_FLAG_TE_USED | (((x) << PIN_TE_ID_SHIFT) & PIN_TE_ID_MASK))

/* Macro for getting Task/Event index from flags. */
#define PIN_GET_TE_ID(flags) ((flags & PIN_TE_ID_MASK) >> PIN_TE_ID_SHIFT)

/* Structure holding state of the pins */
typedef struct
{
    /* Pin specific handlers. */
    nrfx_gpiote_handler_config_t handlers[NRFX_GPIOTE_CONFIG_NUM_OF_EVT_HANDLERS];

    /* Global handler called on each event */
    nrfx_gpiote_handler_config_t global_handler;

    /* Each pin state */
    uint16_t                     pin_flags[MAX_PIN_NUMBER];

    /* Mask for tracking gpiote channel allocation. */
    nrfx_atomic_t                available_channels_mask;

    /* Mask for tracking event handler entries allocation. */
    nrfx_atomic_t                available_evt_handlers;

#if !defined(NRF_GPIO_LATCH_PRESENT)
    uint32_t                     port_pins[GPIO_COUNT];
#endif
    nrfx_drv_state_t             state;
} gpiote_control_block_t;

static gpiote_control_block_t m_cb = {
    .available_channels_mask = NRFX_GPIOTE_APP_CHANNELS_MASK
};

#if defined(NRF_GPIO_LATCH_PRESENT) || (!FULL_PORTS_PRESENT)
static const uint8_t ports[GPIO_COUNT] = GPIO_PORT_NUM_LIST;
#endif

#define GPIO_PORT_OFFSET(i, _) \
    NRFX_COND_CODE_1(NRFX_INSTANCE_PRESENT(NRFX_CONCAT(P, i)),(NRFX_CONCAT(P, i, _PIN_NUM)), (0))

static uint8_t get_pin_idx(nrfx_gpiote_pin_t pin)
{
#if FULL_PORTS_PRESENT
    // If all ports have 32 pins then array ordering matches pin ordering.
    return pin;
#else
    // Possible instances must be explicitely listed as NRFX_LISTIFY cannot be nested.
    static const uint8_t port_offset[] = {
        0,
        NRFX_LISTIFY(1, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(2, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(3, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(4, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(5, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(6, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(7, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(8, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(9, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(10, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(11, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(12, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(13, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(14, GPIO_PORT_OFFSET, (+), _),
        NRFX_LISTIFY(15, GPIO_PORT_OFFSET, (+), _),
    };

    return port_offset[pin >> 5] + (pin & 0x1F);
#endif
}

/** @brief Checks if pin is in use by the driver.
 *
 * @param[in] pin Absolute pin.
 *
 * @return True if pin is in use.
 */
static bool pin_in_use(uint32_t pin)
{
    return m_cb.pin_flags[get_pin_idx(pin)] & PIN_FLAG_IN_USE;
}

/** @brief Check if Task/Event is used.
 *
 * Assuming that pin is in use.
 *
 * @param[in] pin Absolute pin.
 *
 * @return True if pin uses GPIOTE task/event.
 */
static bool pin_in_use_by_te(uint32_t pin)
{
    return m_cb.pin_flags[get_pin_idx(pin)] & PIN_FLAG_TE_USED;
}

/** @brief Check if pin has trigger.
 *
 * @param[in] pin Absolute pin.
 *
 * @return True if pin has trigger.
 */
static bool pin_has_trigger(uint32_t pin)
{
    return PIN_FLAG_TRIG_MODE_GET(m_cb.pin_flags[get_pin_idx(pin)]) != NRFX_GPIOTE_TRIGGER_NONE;
}

/** @brief Check if pin is output.
 *
 * Assuming that pin is in use.
 *
 * @param[in] pin Absolute pin.
 *
 * @return True if pin is output.
 */
static bool pin_is_output(uint32_t pin)
{
    return PIN_FLAG_IS_OUTPUT(m_cb.pin_flags[get_pin_idx(pin)]);
}

/** @brief Check if pin is output controlled by GPIOTE task.
 *
 * @param[in] pin Absolute pin.
 *
 * @return True if pin is task output.
 */
static bool pin_is_task_output(uint32_t pin)
{
    return pin_is_output(pin) && pin_in_use_by_te(pin);
}

/** @brief Check if pin is used by the driver and configured as input.
 *
 * @param[in] pin Absolute pin.
 *
 * @return True if pin is configured as input.
 */
static bool pin_is_input(uint32_t pin)
{
    return !pin_is_output(pin);
}

/* Convert polarity enum (HAL) to trigger enum. */
static nrfx_gpiote_trigger_t gpiote_polarity_to_trigger(nrf_gpiote_polarity_t polarity)
{
   return (nrfx_gpiote_trigger_t)polarity;
}

/* Convert trigger enum to polarity enum (HAL). */
static nrf_gpiote_polarity_t gpiote_trigger_to_polarity(nrfx_gpiote_trigger_t trigger)
{
    return (nrf_gpiote_polarity_t)trigger;
}

/* Returns gpiote TE channel associated with the pin */
static uint8_t pin_te_get(nrfx_gpiote_pin_t pin)
{
    return PIN_GET_TE_ID(m_cb.pin_flags[get_pin_idx(pin)]);
}

static bool is_level(nrfx_gpiote_trigger_t trigger)
{
    return trigger >= NRFX_GPIOTE_TRIGGER_LOW;
}

static bool handler_in_use(int32_t handler_id)
{
    for (uint32_t i = 0; i < MAX_PIN_NUMBER; i++)
    {
        if (PIN_GET_HANDLER_ID(m_cb.pin_flags[i]) == handler_id)
        {
            return true;
        }
    }
    return false;
}

/* Function clears pin handler flag and releases handler slot if handler+context
 * pair is not used by other pin. */
static void release_handler(nrfx_gpiote_pin_t pin)
{
    uint8_t idx = get_pin_idx(pin);
    int32_t handler_id = PIN_GET_HANDLER_ID(m_cb.pin_flags[idx]);

    if (handler_id == PIN_FLAG_NO_HANDLER)
    {
        return;
    }

    m_cb.pin_flags[idx] &= ~PIN_HANDLER_MASK;

    /* Check if other pin is using same handler and release handler only if handler
     * is not used by others.
     */
    if (!handler_in_use(handler_id))
    {
        m_cb.handlers[handler_id].handler = NULL;
        nrfx_err_t err = nrfx_flag32_free(&m_cb.available_evt_handlers, handler_id);
        (void)err;
        NRFX_ASSERT(err == NRFX_SUCCESS);
    }
}

/* Function releases the handler associated with the pin and sets GPIOTE channel
 * configuration to default if it was used with the pin.
 */
static void pin_handler_trigger_uninit(nrfx_gpiote_pin_t pin)
{
    if (pin_in_use_by_te(pin))
    {
        /* te to default */
        nrfy_gpiote_te_default(NRF_GPIOTE, pin_te_get(pin));
    }
    else
    {
#if !defined(NRF_GPIO_LATCH_PRESENT)
        nrf_bitmask_bit_clear(pin, (uint8_t *)m_cb.port_pins);
#endif
    }

    release_handler(pin);
    m_cb.pin_flags[get_pin_idx(pin)] = PIN_FLAG_NOT_USED;
}

nrfx_err_t nrfx_gpiote_pin_uninit(nrfx_gpiote_pin_t pin)
{
    if (!pin_in_use(pin))
    {
        return NRFX_ERROR_INVALID_PARAM;
    }

    nrfx_gpiote_trigger_disable(pin);
    pin_handler_trigger_uninit(pin);
    nrfy_gpio_cfg_default(pin);

    return NRFX_SUCCESS;
}

static int32_t find_handler(nrfx_gpiote_interrupt_handler_t handler, void * p_context)
{
    for (uint32_t i = 0; i < NRFX_GPIOTE_CONFIG_NUM_OF_EVT_HANDLERS; i++)
    {
        if ((m_cb.handlers[i].handler == handler) && (m_cb.handlers[i].p_context == p_context))
        {
            return i;
        }
    }

    return -1;
}

/** @brief Set new handler, if handler was not previously set allocate it. */
static nrfx_err_t pin_handler_set(nrfx_gpiote_pin_t               pin,
                                  nrfx_gpiote_interrupt_handler_t handler,
                                  void *                          p_context)
{
    nrfx_err_t err;
    int32_t handler_id;

    release_handler(pin);
    if (!handler)
    {
        return NRFX_SUCCESS;
    }

    handler_id = find_handler(handler, p_context);
    /* Handler not found, new must be allocated. */
    if (handler_id < 0)
    {
        uint8_t id;

        err = nrfx_flag32_alloc(&m_cb.available_evt_handlers, &id);
        if (err != NRFX_SUCCESS)
        {
            return err;
        }
        handler_id = (int32_t)id;
    }

    m_cb.handlers[handler_id].handler = handler;
    m_cb.handlers[handler_id].p_context = p_context;
    m_cb.pin_flags[get_pin_idx(pin)] |= PIN_FLAG_HANDLER(handler_id);

    return NRFX_SUCCESS;
}

static inline nrf_gpio_pin_sense_t get_initial_sense(nrfx_gpiote_pin_t pin)
{
    nrfx_gpiote_trigger_t trigger = PIN_FLAG_TRIG_MODE_GET(m_cb.pin_flags[get_pin_idx(pin)]);
    nrf_gpio_pin_sense_t sense;

    if (trigger == NRFX_GPIOTE_TRIGGER_LOW)
    {
        sense = NRF_GPIO_PIN_SENSE_LOW;
    }
    else if (trigger == NRFX_GPIOTE_TRIGGER_HIGH)
    {
        sense = NRF_GPIO_PIN_SENSE_HIGH;
    }
    else
    {
        /* If edge detection start with sensing opposite state. */
        sense = nrfy_gpio_pin_read(pin) ? NRF_GPIO_PIN_SENSE_LOW : NRF_GPIO_PIN_SENSE_HIGH;
    }

    return sense;
}

nrfx_err_t nrfx_gpiote_input_configure(nrfx_gpiote_pin_t                    pin,
                                       nrfx_gpiote_input_config_t const *   p_input_config,
                                       nrfx_gpiote_trigger_config_t const * p_trigger_config,
                                       nrfx_gpiote_handler_config_t const * p_handler_config)
{
    nrfx_err_t err;
    uint8_t idx = get_pin_idx(pin);

    if (p_input_config)
    {
        if (pin_is_task_output(pin))
        {
            return NRFX_ERROR_INVALID_PARAM;
        }

        nrf_gpio_pin_dir_t dir = NRF_GPIO_PIN_DIR_INPUT;
        nrf_gpio_pin_input_t input_connect = NRF_GPIO_PIN_INPUT_CONNECT;

        nrfy_gpio_reconfigure(pin, &dir, &input_connect, &p_input_config->pull, NULL, NULL);

        m_cb.pin_flags[idx] &= ~PIN_FLAG_OUTPUT;
        m_cb.pin_flags[idx] |= PIN_FLAG_IN_USE;
    }

    if (p_trigger_config)
    {
        nrfx_gpiote_trigger_t trigger = p_trigger_config->trigger;
        bool use_evt = p_trigger_config->p_in_channel ? true : false;

        if (pin_is_output(pin))
        {
            if (use_evt)
            {
                return NRFX_ERROR_INVALID_PARAM;
            }
        }
        else
        {
            m_cb.pin_flags[idx] &= ~(PIN_TE_ID_MASK | PIN_FLAG_TE_USED);
            if (use_evt)
            {
                bool edge = trigger <= NRFX_GPIOTE_TRIGGER_TOGGLE;

                /* IN event used. */
                if (!edge)
                {
                    /* IN event supports only edge trigger. */
                    return NRFX_ERROR_INVALID_PARAM;
                }

                uint8_t ch = *p_trigger_config->p_in_channel;

                if (trigger == NRFX_GPIOTE_TRIGGER_NONE)
                {
                    nrfy_gpiote_te_default(NRF_GPIOTE, ch);
                }
                else
                {
                    nrf_gpiote_polarity_t polarity = gpiote_trigger_to_polarity(trigger);

                    nrfy_gpiote_event_disable(NRF_GPIOTE, ch);
                    nrfy_gpiote_event_configure(NRF_GPIOTE, ch, pin, polarity);

                    m_cb.pin_flags[idx] |= PIN_FLAG_TE_ID(ch);
                }
            }
        }
#if !defined(NRF_GPIO_LATCH_PRESENT)
        if (use_evt || trigger == NRFX_GPIOTE_TRIGGER_NONE)
        {
            nrf_bitmask_bit_clear(pin, (uint8_t *)m_cb.port_pins);
        }
        else
        {
            nrf_bitmask_bit_set(pin, (uint8_t *)m_cb.port_pins);
        }
#endif
        m_cb.pin_flags[idx] &= ~PIN_FLAG_TRIG_MODE_MASK;
        m_cb.pin_flags[idx] |= PIN_FLAG_TRIG_MODE_SET(trigger);
    }

    if (p_handler_config)
    {
        err = pin_handler_set(pin, p_handler_config->handler, p_handler_config->p_context);
    }
    else
    {
        err = NRFX_SUCCESS;
    }

    return err;
}

nrfx_err_t nrfx_gpiote_output_configure(nrfx_gpiote_pin_t                   pin,
                                        nrfx_gpiote_output_config_t const * p_config,
                                        nrfx_gpiote_task_config_t const *   p_task_config)
{
    uint8_t idx = get_pin_idx(pin);

    if (p_config)
    {
        /* Cannot configure pin to output if pin was using TE event. */
        if (pin_is_input(pin) && pin_in_use_by_te(pin))
        {
            return NRFX_ERROR_INVALID_PARAM;
        }

        /* If reconfiguring to output pin that has trigger configured then accept
         * only when input is still connected. */
        if (pin_has_trigger(pin) && (p_config->input_connect == NRF_GPIO_PIN_INPUT_DISCONNECT))
        {
            return NRFX_ERROR_INVALID_PARAM;
        }

        nrf_gpio_pin_dir_t dir = NRF_GPIO_PIN_DIR_OUTPUT;

        nrfy_gpio_reconfigure(pin, &dir, &p_config->input_connect, &p_config->pull,
                              &p_config->drive, NULL);

        m_cb.pin_flags[idx] |= PIN_FLAG_IN_USE | PIN_FLAG_OUTPUT;
    }

    if (p_task_config)
    {
        if (pin_is_input(pin))
        {
            return NRFX_ERROR_INVALID_PARAM;
        }

        uint32_t ch = p_task_config->task_ch;

        nrfy_gpiote_te_default(NRF_GPIOTE, ch);
        m_cb.pin_flags[idx] &= ~(PIN_FLAG_TE_USED | PIN_TE_ID_MASK);
        if (p_task_config->polarity != NRF_GPIOTE_POLARITY_NONE)
        {
            nrfy_gpiote_task_configure(NRF_GPIOTE, ch, pin,
                                       p_task_config->polarity,
                                       p_task_config->init_val);
            m_cb.pin_flags[idx] |= PIN_FLAG_TE_ID(ch);
        }
    }

    return NRFX_SUCCESS;
}

void nrfx_gpiote_global_callback_set(nrfx_gpiote_interrupt_handler_t handler, void * p_context)
{
    m_cb.global_handler.handler = handler;
    m_cb.global_handler.p_context = p_context;
}

nrfx_err_t nrfx_gpiote_channel_get(nrfx_gpiote_pin_t pin, uint8_t *p_channel)
{
    NRFX_ASSERT(p_channel);

    if (pin_in_use_by_te(pin))
    {
        *p_channel = PIN_GET_TE_ID(m_cb.pin_flags[get_pin_idx(pin)]);
        return NRFX_SUCCESS;
    }
    else
    {
        return NRFX_ERROR_INVALID_PARAM;
    }
}

/* Return handler associated with given pin or null. */
static nrfx_gpiote_handler_config_t const * channel_handler_get(nrfx_gpiote_pin_t pin)
{
    int32_t handler_id = PIN_GET_HANDLER_ID(m_cb.pin_flags[get_pin_idx(pin)]);

    if (handler_id == PIN_FLAG_NO_HANDLER)
    {
        return NULL;
    }

    return &m_cb.handlers[handler_id];
}

nrfx_err_t nrfx_gpiote_init(uint8_t interrupt_priority)
{
    nrfx_err_t err_code;

    if (m_cb.state != NRFX_DRV_STATE_UNINITIALIZED)
    {
        err_code = NRFX_ERROR_INVALID_STATE;
        NRFX_LOG_WARNING("Function: %s, error code: %s.",
                         __func__,
                         NRFX_LOG_ERROR_STRING_GET(err_code));
        return err_code;
    }

    memset(m_cb.pin_flags, 0, sizeof(m_cb.pin_flags));

    nrfy_gpiote_int_init(NRF_GPIOTE, (uint32_t)NRF_GPIOTE_INT_PORT_MASK, interrupt_priority, true);

    m_cb.state = NRFX_DRV_STATE_INITIALIZED;
    m_cb.available_evt_handlers = NRFX_BIT_MASK(NRFX_GPIOTE_CONFIG_NUM_OF_EVT_HANDLERS);

    err_code = NRFX_SUCCESS;
    NRFX_LOG_INFO("Function: %s, error code: %s.", __func__, NRFX_LOG_ERROR_STRING_GET(err_code));
    return err_code;
}


bool nrfx_gpiote_is_init(void)
{
    return (m_cb.state != NRFX_DRV_STATE_UNINITIALIZED) ? true : false;
}

static void pin_uninit(uint32_t pin)
{
    if (nrfy_gpio_pin_present_check(pin) && pin_in_use(pin))
    {
        nrfx_gpiote_pin_uninit(pin);
    }
}

void nrfx_gpiote_uninit(void)
{
    NRFX_ASSERT(m_cb.state != NRFX_DRV_STATE_UNINITIALIZED);

#if FULL_PORTS_PRESENT
    // Simple iteration for simple case to save memory
    for (size_t i = 0; i < MAX_PIN_NUMBER; i++)
    {
            pin_uninit(i);
    }
#else
#define _PORT_LEN(periph, prefix, i, _) NRFX_CONCAT(periph, prefix, i, _PIN_NUM),
    static const uint8_t port_lens[] =
    {
        NRFX_FOREACH_PRESENT(P, _PORT_LEN, (), (), _)
    };

    // Iterate over all pins in all ports.
    for (size_t i = 0; i < NRFX_ARRAY_SIZE(ports); i++)
    {
        for (size_t j = 0; j < port_lens[i]; j++)
        {
            pin_uninit(32 * ports[i] + j);
        }
    }
#undef _PORT_LEN
#endif

    m_cb.state = NRFX_DRV_STATE_UNINITIALIZED;
    NRFX_LOG_INFO("Uninitialized.");
}

nrfx_err_t nrfx_gpiote_channel_free(uint8_t channel)
{
    return nrfx_flag32_free(&m_cb.available_channels_mask, channel);
}

nrfx_err_t nrfx_gpiote_channel_alloc(uint8_t * p_channel)
{
    return nrfx_flag32_alloc(&m_cb.available_channels_mask, p_channel);
}

void nrfx_gpiote_out_set(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_output(pin) && !pin_in_use_by_te(pin));

    nrfy_gpio_pin_set(pin);
}


void nrfx_gpiote_out_clear(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_output(pin) && !pin_in_use_by_te(pin));

    nrfy_gpio_pin_clear(pin);
}


void nrfx_gpiote_out_toggle(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_output(pin) && !pin_in_use_by_te(pin));

    nrfy_gpio_pin_toggle(pin);
}

void nrfx_gpiote_out_task_enable(nrfx_gpiote_pin_t pin)
{
    (void)pin_is_task_output; /* Add to avoid compiler warnings when asserts disabled.*/
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    nrfy_gpiote_task_enable(NRF_GPIOTE, (uint32_t)pin_te_get(pin));
}


void nrfx_gpiote_out_task_disable(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    nrfy_gpiote_task_disable(NRF_GPIOTE, (uint32_t)pin_te_get(pin));
}


nrf_gpiote_task_t nrfx_gpiote_out_task_get(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    return nrfy_gpiote_out_task_get((uint8_t)pin_te_get(pin));
}


uint32_t nrfx_gpiote_out_task_address_get(nrfx_gpiote_pin_t pin)
{
    nrf_gpiote_task_t task = nrfx_gpiote_out_task_get(pin);
    return nrfy_gpiote_task_address_get(NRF_GPIOTE, task);
}


#if defined(GPIOTE_FEATURE_SET_PRESENT)
nrf_gpiote_task_t nrfx_gpiote_set_task_get(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    return nrfy_gpiote_set_task_get((uint8_t)pin_te_get(pin));
}


uint32_t nrfx_gpiote_set_task_address_get(nrfx_gpiote_pin_t pin)
{
    nrf_gpiote_task_t task = nrfx_gpiote_set_task_get(pin);
    return nrfy_gpiote_task_address_get(NRF_GPIOTE, task);
}
#endif // defined(GPIOTE_FEATURE_SET_PRESENT)


#if defined(GPIOTE_FEATURE_CLR_PRESENT)
nrf_gpiote_task_t nrfx_gpiote_clr_task_get(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    return nrfy_gpiote_clr_task_get((uint8_t)pin_te_get(pin));
}


uint32_t nrfx_gpiote_clr_task_address_get(nrfx_gpiote_pin_t pin)
{
    nrf_gpiote_task_t task = nrfx_gpiote_clr_task_get(pin);
    return nrfy_gpiote_task_address_get(NRF_GPIOTE, task);
}
#endif // defined(GPIOTE_FEATURE_CLR_PRESENT)


void nrfx_gpiote_out_task_force(nrfx_gpiote_pin_t pin, uint8_t state)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    nrf_gpiote_outinit_t init_val =
        state ? NRF_GPIOTE_INITIAL_VALUE_HIGH : NRF_GPIOTE_INITIAL_VALUE_LOW;
    nrfy_gpiote_task_force(NRF_GPIOTE, (uint32_t)pin_te_get(pin), init_val);
}


void nrfx_gpiote_out_task_trigger(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_task_output(pin));

    nrf_gpiote_task_t task = nrfy_gpiote_out_task_get((uint8_t)pin_te_get(pin));
    nrfy_gpiote_task_trigger(NRF_GPIOTE, task);
}


#if defined(GPIOTE_FEATURE_SET_PRESENT)
void nrfx_gpiote_set_task_trigger(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_in_use(pin));
    NRFX_ASSERT(pin_in_use_by_te(pin));

    nrf_gpiote_task_t task = nrfy_gpiote_set_task_get((uint8_t)pin_te_get(pin));
    nrfy_gpiote_task_trigger(NRF_GPIOTE, task);
}


#endif // defined(GPIOTE_FEATURE_SET_PRESENT)

#if  defined(GPIOTE_FEATURE_CLR_PRESENT)
void nrfx_gpiote_clr_task_trigger(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_in_use(pin));
    NRFX_ASSERT(pin_in_use_by_te(pin));

    nrf_gpiote_task_t task = nrfy_gpiote_clr_task_get((uint8_t)pin_te_get(pin));
    nrfy_gpiote_task_trigger(NRF_GPIOTE, task);
}


#endif // defined(GPIOTE_FEATURE_CLR_PRESENT)

void nrfx_gpiote_trigger_enable(nrfx_gpiote_pin_t pin, bool int_enable)
{
    NRFX_ASSERT(pin_has_trigger(pin));

    if (pin_in_use_by_te(pin) && pin_is_input(pin))
    {
        uint8_t ch = pin_te_get(pin);

        nrfy_gpiote_event_clear(NRF_GPIOTE, nrf_gpiote_in_event_get(ch));
        nrfy_gpiote_event_enable(NRF_GPIOTE, ch);
        if (int_enable)
        {
            nrfy_gpiote_int_enable(NRF_GPIOTE, NRFX_BIT(ch));
        }
    }
    else
    {
        NRFX_ASSERT(int_enable);
        nrfy_gpio_cfg_sense_set(pin, get_initial_sense(pin));
    }
}

void nrfx_gpiote_trigger_disable(nrfx_gpiote_pin_t pin)
{
    if (pin_in_use_by_te(pin) && pin_is_input(pin))
    {
        uint8_t ch = pin_te_get(pin);

        nrfy_gpiote_int_disable(NRF_GPIOTE, NRFX_BIT(ch));
        nrfy_gpiote_event_disable(NRF_GPIOTE, ch);
    }
    else
    {
        nrfy_gpio_cfg_sense_set(pin, NRF_GPIO_PIN_NOSENSE);
    }
}

bool nrfx_gpiote_in_is_set(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    return nrfy_gpio_pin_read(pin) ? true : false;
}


nrf_gpiote_event_t nrfx_gpiote_in_event_get(nrfx_gpiote_pin_t pin)
{
    NRFX_ASSERT(nrfy_gpio_pin_present_check(pin));
    NRFX_ASSERT(pin_is_input(pin));
    NRFX_ASSERT(pin_has_trigger(pin));

    if (pin_in_use_by_te(pin))
    {
        return nrfy_gpiote_in_event_get((uint8_t)pin_te_get(pin));
    }

    return NRF_GPIOTE_EVENT_PORT;
}


uint32_t nrfx_gpiote_in_event_address_get(nrfx_gpiote_pin_t pin)
{
    nrf_gpiote_event_t event = nrfx_gpiote_in_event_get(pin);
    return nrfy_gpiote_event_address_get(NRF_GPIOTE, event);
}

static void call_handler(nrfx_gpiote_pin_t pin, nrfx_gpiote_trigger_t trigger)
{
    nrfx_gpiote_handler_config_t const * handler = channel_handler_get(pin);

    if (handler)
    {
        handler->handler(pin, trigger, handler->p_context);
    }
    if (m_cb.global_handler.handler)
    {
        m_cb.global_handler.handler(pin, trigger, m_cb.global_handler.p_context);
    }
}

static void next_sense_cond_call_handler(nrfx_gpiote_pin_t     pin,
                                         nrfx_gpiote_trigger_t trigger,
                                         nrf_gpio_pin_sense_t  sense)
{
    if (is_level(trigger))
    {
        call_handler(pin, trigger);
        if (nrfy_gpio_pin_sense_get(pin) == sense)
        {
            /* The sensing mechanism needs to be reenabled here so that the PORT event
             * is generated again for the pin if it stays at the sensed level. */
            nrfy_gpio_cfg_sense_set(pin, NRF_GPIO_PIN_NOSENSE);
            nrfy_gpio_cfg_sense_set(pin, sense);
        }
    }
    else
    {
        /* Reconfigure sense to the opposite level, so the internal PINx.DETECT signal
         * can be deasserted. Therefore PORT event can be generated again,
         * unless some other PINx.DETECT signal is still active. */
        nrf_gpio_pin_sense_t next_sense = (sense == NRF_GPIO_PIN_SENSE_HIGH) ?
                NRF_GPIO_PIN_SENSE_LOW : NRF_GPIO_PIN_SENSE_HIGH;

        nrfy_gpio_cfg_sense_set(pin, next_sense);

        /* Invoke user handler only if the sensed pin level matches its polarity
         * configuration. Call handler unconditionally in case of toggle trigger or
         * level trigger. */
        if ((trigger == NRFX_GPIOTE_TRIGGER_TOGGLE) ||
            (sense == NRF_GPIO_PIN_SENSE_HIGH && trigger == NRFX_GPIOTE_TRIGGER_LOTOHI) ||
            (sense == NRF_GPIO_PIN_SENSE_LOW && trigger == NRFX_GPIOTE_TRIGGER_HITOLO))
        {
            call_handler(pin, trigger);
        }
    }
}

#if defined(NRF_GPIO_LATCH_PRESENT)
static bool latch_pending_read_and_check(uint32_t * latch)
{
    nrfy_gpio_latches_read_and_clear(0, GPIO_COUNT, latch);

    for (uint32_t port_idx = 0; port_idx < GPIO_COUNT; port_idx++)
    {
        if (latch[port_idx])
        {
            /* If any of the latch bits is still set, it means another edge has been captured
             * before or during the interrupt processing. Therefore event-processing loop
             * should be executed again. */
            return true;
        }
    }
    return false;
}

static void port_event_handle(void)
{
    uint32_t latch[GPIO_COUNT];

    nrfy_gpio_latches_read_and_clear(0, GPIO_COUNT, latch);

    do {
        for (uint32_t i = 0; i < GPIO_COUNT; i++)
        {
            while (latch[i])
            {
                uint32_t pin = NRF_CTZ(latch[i]);

                /* Convert to absolute value. */
                uint32_t abs_pin = NRF_PIN_PORT_TO_PIN_NUMBER(pin, ports[i]);
                nrf_gpio_pin_sense_t sense;
                nrfx_gpiote_trigger_t trigger =
                    PIN_FLAG_TRIG_MODE_GET(m_cb.pin_flags[get_pin_idx(abs_pin)]);

                nrf_bitmask_bit_clear(pin, &latch[i]);
                sense = nrfy_gpio_pin_sense_get(abs_pin);

                next_sense_cond_call_handler(abs_pin, trigger, sense);
                /* Try to clear LATCH bit corresponding to currently processed pin.
                 * This may not succeed if the pin's state changed during the interrupt processing
                 * and now it matches the new sense configuration. In such case,
                 * the pin will be processed again in another iteration of the outer loop. */
                nrfy_gpio_pin_latch_clear(abs_pin);
           }
        }

        /* All pins have been handled, clear PORT, check latch again in case
         * something came between deciding to exit and clearing PORT event. */
        (void)nrfy_gpiote_events_process(NRF_GPIOTE, (uint32_t)NRF_GPIOTE_INT_PORT_MASK);
    } while (latch_pending_read_and_check(latch));
}

#else

static bool input_read_and_check(uint32_t * input, uint32_t * pins_to_check)
{
    bool process_inputs_again;
    uint32_t new_input[GPIO_COUNT];

    nrfy_gpio_ports_read(0, GPIO_COUNT, new_input);

    process_inputs_again = false;
    for (uint32_t port_idx = 0; port_idx < GPIO_COUNT; port_idx++)
    {
        /* Execute XOR to find out which inputs have changed. */
        uint32_t input_diff = input[port_idx] ^ new_input[port_idx];
        input[port_idx] = new_input[port_idx];
        if (input_diff)
        {
            /* If any differences among inputs were found, mark those pins
             * to be processed again. */
            pins_to_check[port_idx] &= input_diff;
            process_inputs_again = true;
        }
        else
        {
            pins_to_check[port_idx] = 0;
        }
    }
    return process_inputs_again;
}

static void port_event_handle(void)
{
    uint32_t pins_to_check[GPIO_COUNT];
    uint32_t input[GPIO_COUNT] = {0};
    uint8_t rel_pin;
    uint8_t pin;
    nrfx_gpiote_trigger_t trigger;

    nrfy_gpio_ports_read(0, GPIO_COUNT, input);

    for (uint32_t port_idx = 0; port_idx < GPIO_COUNT; port_idx++)
    {
        pins_to_check[port_idx] = m_cb.port_pins[port_idx];
    }

    do {
        for (uint32_t i = 0; i < GPIO_COUNT; i++)
        {
            while (pins_to_check[i])
            {
                nrf_gpio_pin_sense_t sense;
                bool pin_state;

                rel_pin = NRF_CTZ(pins_to_check[i]);
                pins_to_check[i] &= ~NRFX_BIT(rel_pin);
                /* Absolute */
                pin = rel_pin + 32 * i;

                trigger = PIN_FLAG_TRIG_MODE_GET(m_cb.pin_flags[get_pin_idx(pin)]);
                sense = nrfy_gpio_pin_sense_get(pin);
                pin_state = nrf_bitmask_bit_is_set(pin, input);

                /* Process pin further only if its state matches its sense level. */
                if ((pin_state && (sense == NRF_GPIO_PIN_SENSE_HIGH)) ||
                    (!pin_state && (sense == NRF_GPIO_PIN_SENSE_LOW)) )
                {
                    next_sense_cond_call_handler(pin, trigger, sense);
                }
            }
        }

        /* All pins used with PORT must be rechecked because it's content and
         * number of port pins may have changed during handler execution. */
        for (uint32_t port_idx = 0; port_idx < GPIO_COUNT; port_idx++)
        {
            pins_to_check[port_idx] = m_cb.port_pins[port_idx];
        }

        /* Small trick to continue check if input level is equal to the trigger:
         * Set input to the opposite level. If input equals trigger level that
         * it will be set in pins_to_check. */
        for (uint32_t i = 0; i < GPIO_COUNT; i++)
        {
            uint32_t pin_mask = pins_to_check[i];

            while (pin_mask)
            {
                rel_pin = NRF_CTZ(pin_mask);
                pin_mask &= ~NRFX_BIT(rel_pin);
                pin = rel_pin + 32 * i;
                if (nrfy_gpio_pin_sense_get(pin) != NRF_GPIO_PIN_NOSENSE)
                {
                    trigger = PIN_FLAG_TRIG_MODE_GET(m_cb.pin_flags[get_pin_idx(pin)]);
                    if (trigger == NRFX_GPIOTE_TRIGGER_HIGH)
                    {
                        input[i] &= ~NRFX_BIT(rel_pin);
                    }
                    else if (trigger == NRFX_GPIOTE_TRIGGER_LOW)
                    {
                        input[i] |= NRFX_BIT(rel_pin);
                    }
                }
            }
        }

        (void)nrfy_gpiote_events_process(NRF_GPIOTE, (uint32_t)NRF_GPIOTE_INT_PORT_MASK);
    } while (input_read_and_check(input, pins_to_check));
}
#endif // defined(NRF_GPIO_LATCH_PRESENT)

static void gpiote_evt_handle(uint32_t mask)
{
    while (mask)
    {
        uint32_t ch = NRF_CTZ(mask);
        mask &= ~NRFX_BIT(ch);
        nrfx_gpiote_pin_t pin = nrfy_gpiote_event_pin_get(NRF_GPIOTE, ch);
        nrf_gpiote_polarity_t polarity = nrfy_gpiote_event_polarity_get(NRF_GPIOTE, ch);

        call_handler(pin, gpiote_polarity_to_trigger(polarity));
    }
}

void nrfx_gpiote_irq_handler(void)
{
    /* Collect status of all GPIOTE pin events. Processing is done once all are collected and cleared.*/
    uint32_t enabled_in_events = nrf_gpiote_int_enable_check(NRF_GPIOTE, NRF_GPIOTE_INT_IN_MASK);
    uint32_t evt_mask = nrfy_gpiote_events_process(NRF_GPIOTE,
                                                   enabled_in_events |
                                                   (uint32_t)NRF_GPIOTE_INT_PORT_MASK);

    /* Handle PORT event. */
    if (evt_mask & (uint32_t)NRF_GPIOTE_INT_PORT_MASK)
    {
        port_event_handle();
        evt_mask &= ~(uint32_t)NRF_GPIOTE_INT_PORT_MASK;
    }

    /* Process pin events. */
    gpiote_evt_handle(evt_mask);
}

#endif // NRFX_CHECK(NRFX_GPIOTE_ENABLED)
