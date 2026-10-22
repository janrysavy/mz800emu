/*
 * File:   joy.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 1. února 2018, 9:20
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef JOY_H
#define JOY_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum en_JOY_STATEBIT
{
    JOY_STATEBIT_UP = 0,
    JOY_STATEBIT_DOWN = 1,
    JOY_STATEBIT_LEFT = 2,
    JOY_STATEBIT_RIGHT = 3,
    JOY_STATEBIT_TRIG1 = 4,
    JOY_STATEBIT_TRIG2 = 5,
} en_JOY_STATEBIT;

typedef enum en_JOY_DEVID
{
    JOY_DEVID_0 = 0,
    JOY_DEVID_1,
    JOY_DEVID_COUNT
} en_JOY_DEVID;

typedef enum en_JOY_TYPE
{
    JOY_TYPE_NONE = 0,
    JOY_TYPE_NUM_KEYPAD,
    JOY_TYPE_JOYSTICK,
} en_JOY_TYPE;

typedef struct st_JOY_DEV
{
    en_JOY_DEVID id;
    en_JOY_TYPE type;
    uint8_t state;
    /* Active-low automation source, independent of physical/keypad polling. */
    uint8_t virtual_state;
} st_JOY_DEV;

typedef struct st_JOY
{
    st_JOY_DEV dev[JOY_DEVID_COUNT];
} st_JOY;

extern st_JOY g_joy;

#define joy_reset_dev_state(joy_devid)     \
    {                                      \
        g_joy.dev[joy_devid].state = 0xff; \
    }

#define JOY_STATEBIT_RESET(state, bit) \
    {                                  \
        state &= ~(1 << bit);          \
    }

#ifdef __cplusplus
extern "C"
{
#endif
    extern void joy_init(void);
    extern uint8_t joy_read_byte(en_JOY_DEVID joy_devid);
#ifdef __cplusplus
}
#endif

#endif /* JOY_H */
